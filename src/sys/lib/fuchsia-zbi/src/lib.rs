// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod zbi_abi;

use {
    fuchsia_zircon as zx,
    lazy_static::lazy_static,
    log::info,
    std::{
        collections::{HashMap, HashSet},
        convert::TryFrom,
        mem::size_of,
    },
    thiserror::Error,
    zbi_abi::{
        is_zbi_type_driver_metadata, zbi_header_t, ZbiType, ZBI_ALIGNMENT_BYTES,
        ZBI_CONTAINER_MAGIC, ZBI_FLAG_CRC32, ZBI_FLAG_VERSION, ZBI_ITEM_MAGIC, ZBI_ITEM_NO_CRC32,
    },
    zerocopy::LayoutVerified,
};

const ZBI_HEADER_SIZE: usize = size_of::<zbi_header_t>();

lazy_static! {
    static ref PAGE_SIZE: u32 = zx::system_get_page_size();
}

#[derive(Debug, Error, Eq, PartialEq)]
pub enum ZbiParserError {
    #[error("Failed to read {} bytes at offset {}: {}", size, offset, status)]
    FailedToReadPayload { size: usize, offset: u32, status: zx::Status },

    #[error("Failed to zero {} bytes at offset {}: {}", size, offset, status)]
    FailedToZeroMemory { size: usize, offset: u32, status: zx::Status },

    #[error("Failed to parse bytes as an unaligned zbi_header_t")]
    FailedToParseHeader,

    #[error("Failed to validate header, magic was {} but expected {}", actual, ZBI_ITEM_MAGIC)]
    InvalidHeaderMagic { actual: u32 },

    #[error(
        "Failed to validate container header, type was {:#?} but expected {:#?}",
        zbi_type,
        ZbiType::Container
    )]
    InvalidContainerHeaderType { zbi_type: ZbiType },

    #[error(
        "Failed to validate container header, extra magic was {} but expected {}",
        actual,
        ZBI_CONTAINER_MAGIC
    )]
    InvalidContainerHeaderExtraMagic { actual: u32 },

    #[error("Header flags {:#b} missing flag version {:#b}", flags, ZBI_FLAG_VERSION)]
    MissingZbiVersionFlag { flags: u32 },

    #[error("ZBI header contains a bad CRC32")]
    BadCRC32,

    #[error("{:?} was not found in the ZBI", zbi_type)]
    ItemNotFound { zbi_type: ZbiType },

    #[error("Failed to decommit pages: {}", status)]
    FailedToDecommitPages { status: zx::Status },

    #[error("ZBI parser bailed to prevent a u32 overflow.")]
    Overflow,

    #[error("Unknown error in the ZBI parser!")]
    Unknown,
}

#[derive(Debug, PartialEq)]
pub struct DecommitRange {
    start: u32,
    end: u32,
}

#[derive(Debug, Clone, Copy)]
pub struct ZbiItem {
    header_offset: u32,
    item_offset: u32,
    item_length: u32,
    extra: u32,
}

#[derive(Debug, PartialEq)]
pub struct ZbiResult {
    bytes: Vec<u8>,
    extra: u32, // Optional metadata that might be used to identify ZBI items with the same type.
}

#[derive(Debug)]
pub struct ZbiParser {
    vmo: zx::Vmo,
    parsed: bool,
    items: HashMap<ZbiType, Vec<ZbiItem>>,
    items_to_store: HashSet<ZbiType>,
    decommit_ranges: Vec<DecommitRange>,
}

impl ZbiParser {
    fn get_type(raw_type: u32) -> ZbiType {
        match raw_type {
            x if x == ZbiType::Container as u32 => ZbiType::Container,
            x if x == ZbiType::Cmdline as u32 => ZbiType::Cmdline,
            x if x == ZbiType::Crashlog as u32 => ZbiType::Crashlog,
            x if x == ZbiType::KernelDriver as u32 => ZbiType::KernelDriver,
            x if x == ZbiType::PlatformId as u32 => ZbiType::PlatformId,
            x if x == ZbiType::StorageBootfsFactory as u32 => ZbiType::StorageBootfsFactory,
            x if x == ZbiType::StorageRamdisk as u32 => ZbiType::StorageRamdisk,
            x if x == ZbiType::ImageArgs as u32 => ZbiType::ImageArgs,
            x if x == ZbiType::SerialNumber as u32 => ZbiType::SerialNumber,
            x if x == ZbiType::BootloaderFile as u32 => ZbiType::BootloaderFile,
            x if x == ZbiType::DeviceTree as u32 => ZbiType::DeviceTree,
            x if is_zbi_type_driver_metadata(x) => ZbiType::DriverMetadata,
            _ => ZbiType::Unknown,
        }
    }

    // ZBI items are padded to 8 byte boundaries.
    fn align_zbi_item(length: u32) -> Result<u32, ZbiParserError> {
        let rem = length % ZBI_ALIGNMENT_BYTES;
        if rem > 0 {
            length.checked_add(ZBI_ALIGNMENT_BYTES - rem).ok_or(ZbiParserError::Overflow)
        } else {
            Ok(length)
        }
    }

    // Get the address of the next page. This will be replaced by the nightly
    // `checked_next_multiple_of` when available.
    fn round_up_to_page(address: u32) -> Result<u32, ZbiParserError> {
        Ok(ZbiParser::round_down_to_page(
            address.checked_add(*PAGE_SIZE - 1).ok_or(ZbiParserError::Overflow)?,
        ))
    }

    // Get the address of the previous page. This will be replaced by the nightly
    // `checked_next_multiple_of` when available.
    fn round_down_to_page(address: u32) -> u32 {
        address - (address % *PAGE_SIZE)
    }

    fn get_header<'a>(
        &self,
        bytes: &'a [u8],
    ) -> Result<(ZbiType, LayoutVerified<&'a [u8], zbi_header_t>), ZbiParserError> {
        let header = LayoutVerified::<&[u8], zbi_header_t>::new_unaligned(&bytes[..])
            .ok_or(ZbiParserError::FailedToParseHeader)?;

        if header.magic.get() != ZBI_ITEM_MAGIC {
            return Err(ZbiParserError::InvalidHeaderMagic { actual: header.magic.get() });
        }

        if header.flags.get() & ZBI_FLAG_VERSION == 0 {
            return Err(ZbiParserError::MissingZbiVersionFlag { flags: header.flags.get() });
        }

        if (header.flags.get() & ZBI_FLAG_CRC32 == 0) && (header.crc32.get() != ZBI_ITEM_NO_CRC32) {
            return Err(ZbiParserError::BadCRC32);
        }

        Ok((ZbiParser::get_type(header.zbi_type.get()), header))
    }

    fn should_store_item(&self, zbi_type: ZbiType) -> bool {
        if self.items_to_store.len() == 0 {
            // If there isn't a list of items to store, it means store every item found
            // in the ZBI unless the type is unknown.
            zbi_type != ZbiType::Unknown
        } else if self.items_to_store.contains(&zbi_type) {
            true
        } else {
            false
        }
    }

    fn decommit_range(&mut self, start: u32, end: u32) -> Result<(), ZbiParserError> {
        let start = ZbiParser::round_up_to_page(start)?;
        let end = ZbiParser::round_down_to_page(end);

        if start < end {
            self.vmo
                .op_range(zx::VmoOp::DECOMMIT, start.into(), (end - start).into())
                .map_err(|status| ZbiParserError::FailedToDecommitPages { status })?;
            self.decommit_ranges.push(DecommitRange { start, end });
            info!("[ZBI Parser] Decommitted BOOTDATA VMO from {:x} to {:x}", start, end);
        }

        Ok(())
    }

    pub fn new(vmo: zx::Vmo) -> ZbiParser {
        Self {
            vmo,
            parsed: false,
            items: HashMap::new(),
            items_to_store: HashSet::new(),
            decommit_ranges: Vec::new(),
        }
    }

    /// Set a ZBI item type as should be stored. If no item types are set to store, then all
    /// known items are stored.
    pub fn set_store_item(mut self, zbi_type: ZbiType) -> Self {
        assert!(
            ZbiParser::get_type(zbi_type as u32) != ZbiType::Unknown,
            "You must add a u32 -> ZbiType mapping for any new item you wish to store"
        );
        self.items_to_store.insert(zbi_type);
        self
    }

    /// Try and get one stored item type.
    pub fn try_get_item(&self, zbi_type: ZbiType) -> Result<Vec<ZbiResult>, ZbiParserError> {
        if !self.items.contains_key(&zbi_type) {
            return Err(ZbiParserError::ItemNotFound { zbi_type });
        }

        let mut result: Vec<ZbiResult> = Vec::new();
        for item in &self.items[&zbi_type] {
            let mut bytes = vec![0; item.item_length as usize];
            self.vmo.read(&mut bytes, item.item_offset.into()).map_err(|status| {
                ZbiParserError::FailedToReadPayload {
                    size: bytes.len(),
                    offset: item.item_offset,
                    status,
                }
            })?;
            result.push(ZbiResult { bytes, extra: item.extra });
        }

        Ok(result)
    }

    /// Get all stored ZBI items.
    pub fn get_items(&self) -> Result<HashMap<ZbiType, Vec<ZbiResult>>, ZbiParserError> {
        let mut result = HashMap::new();
        for key in self.items.keys() {
            result.insert(key.clone(), self.try_get_item(key.clone())?);
        }
        Ok(result)
    }

    /// Release an item type, zeroing the VMO memory and decommitting the pages if possible.
    pub fn release_item(&mut self, zbi_type: ZbiType) -> Result<(), ZbiParserError> {
        if !self.items.contains_key(&zbi_type) {
            return Err(ZbiParserError::ItemNotFound { zbi_type });
        }

        let mut possible_decommit_range = Vec::new();
        for item in &self.items[&zbi_type] {
            let length = u32::try_from(ZBI_HEADER_SIZE).map_err(|_| ZbiParserError::Unknown)?
                + item.item_length;
            self.vmo.op_range(zx::VmoOp::ZERO, item.header_offset.into(), length.into()).map_err(
                |status| ZbiParserError::FailedToZeroMemory {
                    size: length as usize,
                    offset: item.header_offset,
                    status,
                },
            )?;

            possible_decommit_range.push(DecommitRange {
                start: item.header_offset,
                end: item.item_offset + item.item_length,
            });
        }

        self.items.remove(&zbi_type);
        let on_same_page = |start: u32, end: u32| -> Result<bool, ZbiParserError> {
            // Start is inclusive, but end is exclusive. That means that an item ending on
            // PAGE_SIZE does not overlap with an item starting on PAGE_SIZE. We can trivially
            // fix that during this calculation by subtracting 1 from end to bring it into an
            // inclusive range.
            let end = end.checked_sub(1).ok_or(ZbiParserError::Overflow)?;
            Ok((start / *PAGE_SIZE) == (end / *PAGE_SIZE))
        };

        for mut decommit_range in possible_decommit_range {
            let mut adjusted_end = false;
            let mut adjusted_start = false;
            'zbi_key_loop: for items in self.items.values() {
                for item in items {
                    // If any other item starts or ends on the same pages as our range, retreat
                    // the range one page away. Either we cross ourselves and know we have nothing
                    // to decommit, or we have retreated into the bounds of this single item.
                    let item_start = item.header_offset;
                    let item_end = item.item_offset + item.item_length;

                    if !adjusted_start && on_same_page(decommit_range.start, item_end)? {
                        // An item ends on the same page this item starts.
                        adjusted_start = true;
                        decommit_range.start = decommit_range
                            .start
                            .checked_add(*PAGE_SIZE)
                            .ok_or(ZbiParserError::Overflow)?;
                    }

                    if !adjusted_end && on_same_page(item_start, decommit_range.end)? {
                        // An item starts on the same page this item ends.
                        adjusted_end = true;
                        decommit_range.end =
                            decommit_range.end.checked_sub(*PAGE_SIZE).unwrap_or(0);
                    }

                    if adjusted_start && adjusted_end {
                        break 'zbi_key_loop;
                    }
                }
            }

            // The previous loop verified that no other items start or end on the current page
            // (they might have already been decomitted), so it's safe to extend the start and
            // end ranges to the limit of their respective pages.
            decommit_range.end = ZbiParser::round_up_to_page(decommit_range.end)?;
            decommit_range.start = ZbiParser::round_down_to_page(decommit_range.start);

            self.decommit_range(decommit_range.start, decommit_range.end)?;
        }

        Ok(())
    }

    /// Parse a ZBI VMO, storing the offset for each item. If `set_store_item` was used, only
    /// those item types will be stored, and the other item types will be zeroed and decommitted.
    pub fn parse(mut self) -> Result<Self, ZbiParserError> {
        // The ZBI parser may decommit unused pages of memory, so trying to parse this ZBI twice
        // will result in nonsense.
        assert!(!self.parsed, "Parse should only be invoked once!");
        self.parsed = true;

        let mut vmo_offset: u32 = 0;
        let mut header_bytes = [0; ZBI_HEADER_SIZE];

        self.vmo.read(&mut header_bytes, vmo_offset.into()).map_err(|status| {
            ZbiParserError::FailedToReadPayload {
                size: header_bytes.len(),
                offset: vmo_offset,
                status,
            }
        })?;

        let (zbi_type, header) = self.get_header(&header_bytes)?;
        if zbi_type != ZbiType::Container {
            return Err(ZbiParserError::InvalidContainerHeaderType { zbi_type });
        }
        if header.extra.get() != ZBI_CONTAINER_MAGIC {
            return Err(ZbiParserError::InvalidContainerHeaderExtraMagic {
                actual: header.extra.get(),
            });
        }

        // Cast this to a u32 once for convenience.
        let header_offset = u32::try_from(ZBI_HEADER_SIZE).map_err(|_| ZbiParserError::Unknown)?;

        vmo_offset = header_offset;
        let mut remaining_length = header.length.get();

        let mut decommit_start = 0;
        let mut decommit_end = 0;

        while remaining_length > header_offset {
            let mut header_bytes = [0; ZBI_HEADER_SIZE];
            let current_offset = vmo_offset;
            self.vmo.read(&mut header_bytes, current_offset.into()).map_err(|status| {
                ZbiParserError::FailedToReadPayload {
                    size: header_bytes.len(),
                    offset: vmo_offset,
                    status,
                }
            })?;

            let (zbi_type, header) = self.get_header(&header_bytes)?;
            let next_item = ZbiParser::align_zbi_item(
                header_offset.checked_add(header.length.get()).ok_or(ZbiParserError::Overflow)?,
            )?;

            vmo_offset = vmo_offset.checked_add(next_item).ok_or(ZbiParserError::Overflow)?;
            remaining_length =
                remaining_length.checked_sub(next_item).ok_or(ZbiParserError::Overflow)?;

            if self.should_store_item(zbi_type) {
                let entry = self.items.entry(zbi_type).or_insert(Vec::new());
                entry.push(ZbiItem {
                    header_offset: current_offset,
                    item_offset: current_offset + header_offset,
                    item_length: header.length.get(),
                    extra: header.extra.get(),
                });

                // If there is a decommit range, resolve it now so that it doesn't include this
                // item, and then start the next range at the end of this item. If start >= end,
                // nothing will be decommitted.
                self.decommit_range(decommit_start, decommit_end)?;
                decommit_start = vmo_offset;
            } else {
                // Extend the decommit range to cover this item.
                decommit_end = vmo_offset;
            }
        }

        if decommit_end > decommit_start {
            // The last item was included in a decommit range, so the parser should extend the
            // range up to the end of the current page.
            decommit_end = ZbiParser::round_up_to_page(decommit_end)?;
            self.decommit_range(decommit_start, decommit_end)?;
        }

        Ok(self)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::Error,
        byteorder::LittleEndian,
        std::convert::TryInto,
        zerocopy::{byteorder::U32, AsBytes},
    };

    fn check_item_bytes(builder: &ZbiBuilder, parser: &ZbiParser) {
        for (zbi_type, items) in &parser.items {
            let expected = builder.get_bytes(items);
            let actual = match parser.try_get_item(zbi_type.clone()) {
                Ok(val) => val.iter().map(|result| result.bytes.clone()).collect(),
                Err(_) => {
                    assert!(false);
                    Vec::new()
                }
            };
            assert_eq!(expected, actual);
        }
    }

    fn check_extracted_items(parser: &ZbiParser, expected: &[ZbiType]) {
        let actual = parser.get_items().unwrap();
        assert_eq!(actual.keys().len(), expected.len());
        assert!(expected.iter().all(|k| { actual.contains_key(k) }));
    }

    struct ZbiBuilder {
        zbi_bytes: Vec<u8>,
    }

    impl ZbiBuilder {
        fn get_bytes(&self, zbi_items: &[ZbiItem]) -> Vec<&[u8]> {
            zbi_items
                .iter()
                .map(|item| {
                    let start = item.item_offset as usize;
                    let end = (item.item_offset + item.item_length) as usize;

                    &self.zbi_bytes[start..end]
                })
                .collect()
        }

        fn simple_header(zbi_type: ZbiType, length: u32) -> zbi_header_t {
            zbi_header_t {
                zbi_type: U32::<LittleEndian>::new(zbi_type as u32),
                length: U32::<LittleEndian>::new(length),
                extra: U32::<LittleEndian>::new(if zbi_type == ZbiType::Container {
                    ZBI_CONTAINER_MAGIC
                } else {
                    0
                }),
                flags: U32::<LittleEndian>::new(ZBI_FLAG_VERSION),
                reserved_0: U32::<LittleEndian>::new(0),
                reserved_1: U32::<LittleEndian>::new(0),
                magic: U32::<LittleEndian>::new(ZBI_ITEM_MAGIC),
                crc32: U32::<LittleEndian>::new(ZBI_ITEM_NO_CRC32),
            }
        }

        fn new() -> Self {
            Self { zbi_bytes: Vec::new() }
        }

        fn add_header(mut self, header: zbi_header_t) -> Self {
            let bytes = header.as_bytes();
            self.zbi_bytes.extend(bytes);
            self
        }

        fn add_item(mut self, length: u32) -> Self {
            for i in 0..ZbiParser::align_zbi_item(length).unwrap() {
                // Arbitrary values we can check later.
                self.zbi_bytes.push((i % u8::MAX as u32).try_into().unwrap());
            }
            self
        }

        fn calculate_item_length(mut self) -> Self {
            let item_length = U32::<LittleEndian>::new(
                u32::try_from(self.zbi_bytes.len() - ZBI_HEADER_SIZE).unwrap(),
            );
            let item_length_bytes = item_length.as_bytes();

            let mut i = 4usize;
            for x in item_length_bytes {
                self.zbi_bytes[i] = *x;
                i += 1;
            }

            self
        }

        fn generate(self) -> Result<(zx::Vmo, Self), Error> {
            let vmo = zx::Vmo::create(self.zbi_bytes.len().try_into()?)?;
            vmo.write(&self.zbi_bytes, 0)?;
            Ok((vmo, self))
        }
    }

    #[fuchsia::test]
    async fn empty_zbi() {
        let (zbi, _builder) = ZbiBuilder::new().generate().expect("Failed to create ZBI");
        let parser = ZbiParser::new(zbi).parse();

        assert!(parser.is_err());
        assert_eq!(
            parser.unwrap_err(),
            ZbiParserError::FailedToReadPayload {
                size: ZBI_HEADER_SIZE,
                offset: 0,
                status: zx::Status::OUT_OF_RANGE
            }
        );
    }

    #[fuchsia::test]
    async fn zbi_smaller_than_header() {
        let mut builder =
            ZbiBuilder::new().add_header(ZbiBuilder::simple_header(ZbiType::Container, 0));

        // Truncate the header giving us an invalid header.
        builder.zbi_bytes.resize(builder.zbi_bytes.len() / 2, 0);

        let (zbi, _builder) = builder.generate().expect("Failed to create ZBI");
        let parser = ZbiParser::new(zbi).parse();

        // VMOs have size rounded up to the nearest page, so when our VMO doesn't contain
        // a complete header we'll generally see invalid header magic instead of a read failure.
        assert!(parser.is_err());
        assert_eq!(parser.unwrap_err(), ZbiParserError::InvalidHeaderMagic { actual: 0 });
    }

    #[fuchsia::test]
    async fn invalid_container_header_fields() {
        let mut header = ZbiBuilder::simple_header(ZbiType::Container, 0);

        // Remove the container specific magic field.
        header.extra = U32::<LittleEndian>::new(0);

        let (zbi, _builder) =
            ZbiBuilder::new().add_header(header).generate().expect("Failed to create ZBI");
        let parser = ZbiParser::new(zbi).parse();

        assert!(parser.is_err());
        assert_eq!(
            parser.unwrap_err(),
            ZbiParserError::InvalidContainerHeaderExtraMagic { actual: 0 }
        );

        // The first header is the wrong type -- ZBI images start with a container type header
        // describing the entire volume.
        let (zbi, _builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, 0))
            .generate()
            .expect("failed to create zbi");
        let parser = ZbiParser::new(zbi).parse();

        assert!(parser.is_err());
        assert_eq!(
            parser.unwrap_err(),
            ZbiParserError::InvalidContainerHeaderType { zbi_type: ZbiType::Crashlog }
        );
    }

    #[fuchsia::test]
    async fn invalid_header_flags() {
        let mut header = ZbiBuilder::simple_header(ZbiType::Container, 0);

        // Remove the required ZBI_FLAG_VERSION flag.
        header.flags = U32::<LittleEndian>::new(0);

        let (zbi, _builder) =
            ZbiBuilder::new().add_header(header).generate().expect("Failed to create ZBI");
        let parser = ZbiParser::new(zbi).parse();

        assert!(parser.is_err());
        assert_eq!(parser.unwrap_err(), ZbiParserError::MissingZbiVersionFlag { flags: 0 });

        let mut header = ZbiBuilder::simple_header(ZbiType::Container, 0);

        // Remove the required CRC32 disabled value.
        header.crc32 = U32::<LittleEndian>::new(0);

        let (zbi, _builder) =
            ZbiBuilder::new().add_header(header).generate().expect("failed to create zbi");
        let parser = ZbiParser::new(zbi).parse();

        assert!(parser.is_err());
        assert_eq!(parser.unwrap_err(), ZbiParserError::BadCRC32);
    }

    #[fuchsia::test]
    async fn zbi_item_overflows_u32() {
        let (zbi, _builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Container, 12345))
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, u32::MAX))
            .generate()
            .expect("failed to create zbi");
        let parser = ZbiParser::new(zbi).parse();

        assert!(parser.is_err());
        assert_eq!(parser.unwrap_err(), ZbiParserError::Overflow);
    }

    #[fuchsia::test]
    async fn zbi_item_has_correct_extra_field() {
        let mut crash_header = ZbiBuilder::simple_header(ZbiType::Crashlog, 0x80);
        crash_header.extra = U32::<LittleEndian>::new(0xABCD);

        let (zbi, _builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Container, 0))
            .add_header(crash_header)
            .add_item(0x80)
            .calculate_item_length()
            .generate()
            .expect("failed to create zbi");
        let parser = ZbiParser::new(zbi).parse().expect("Failed to parse ZBI");

        let items = parser.try_get_item(ZbiType::Crashlog).expect("Failed to get item");
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].extra, 0xABCD);
    }

    #[fuchsia::test]
    async fn zbi_items_have_eight_byte_alignment() {
        let (zbi, builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Container, 0))
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, 25)) // Not 8 byte aligned.
            .add_item(25)
            .add_header(ZbiBuilder::simple_header(ZbiType::Cmdline, 32))
            .add_item(32)
            .calculate_item_length()
            .generate()
            .expect("failed to create zbi");
        let parser = ZbiParser::new(zbi).parse().expect("Failed to parse ZBI");

        let crash_items = &parser.items[&ZbiType::Crashlog];
        assert!(crash_items.len() == 1);
        let crash_item = &crash_items[0];

        let cmdline_items = &parser.items[&ZbiType::Cmdline];
        assert!(cmdline_items.len() == 1);
        let cmdline_item = &cmdline_items[0];

        // The cmdline item's header should be eight byte aligned, but also within eight bytes
        // of where the crashlog item ends.
        let crash_item_length = crash_item.item_offset + crash_item.item_length;
        assert!(crash_item_length != cmdline_item.header_offset + 1);
        assert!(cmdline_item.header_offset % 8 == 0);
        assert!((8 - crash_item_length % 8) + crash_item_length == cmdline_item.header_offset);

        check_item_bytes(&builder, &parser);
    }

    #[fuchsia::test]
    async fn pages_with_only_filtered_items_decommitted() {
        // In this test setup we have the following page allocation
        // Page 0  : Container header
        //         : Crashlog #1
        //         : Crashlog #2 (Start)
        // Page 1  : Crashlog #2 (End)
        //         : Cmdline     (Start)
        // Page 2-4: Cmdline     (Continued)
        // Page 5  : Cmdline     (End)
        //         : StorageBootfsFactory
        //         : ImageArgs (Start)
        // Page 6: : ImageArgs (End)
        //
        // As we want to save Crashlog and StorageBootfsFactory, only pages that are purely
        // Cmdline or ImageArgs items can be decommitted.
        let (zbi, builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Container, 0))
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, 2048))
            .add_item(2048)
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, 4096))
            .add_item(4096)
            .add_header(ZbiBuilder::simple_header(ZbiType::Cmdline, 16384))
            .add_item(16384)
            .add_header(ZbiBuilder::simple_header(ZbiType::StorageBootfsFactory, 250))
            .add_item(250)
            .add_header(ZbiBuilder::simple_header(ZbiType::ImageArgs, 2048))
            .add_item(2048)
            .calculate_item_length()
            .generate()
            .expect("failed to create zbi");
        let parser = ZbiParser::new(zbi)
            .set_store_item(ZbiType::Crashlog)
            .set_store_item(ZbiType::StorageBootfsFactory)
            .parse()
            .expect("Failed to parse ZBI");

        check_extracted_items(&parser, &[ZbiType::Crashlog, ZbiType::StorageBootfsFactory]);
        check_item_bytes(&builder, &parser);

        assert_eq!(
            parser.decommit_ranges,
            vec![
                // The Cmdline item on pages 2 to 4. This item is also present on page
                // 1 and 5, but stored items are also present on those pages preventing us from
                // decommitting them.
                DecommitRange { start: 0x2000, end: 0x5000 },
                // The ImageArgs item on page 6. This item is also present on the end of page 5,
                // but StorageBootfsFactory is a stored item which prevents us from decommitting.
                DecommitRange { start: 0x6000, end: 0x7000 }
            ]
        );
    }

    #[fuchsia::test]
    async fn get_then_decommit_driver_metadata() {
        // Driver metadata ZBI types are special -- they can have any u32 value as long as the
        // least significant byte is 0x6D (lowercase 'm' in ASCII).
        //
        // Note that the length of the first driver metadata item is 0xFC0, which puts it right
        // at the page boundary as the two headers before this as 32 bytes. The third driver
        // metadata item is also page aligned on the third page. This is to check for off by one
        // errors.
        let mut driver_metadata_header1 = ZbiBuilder::simple_header(ZbiType::Unknown, 0xFC0);
        let mut driver_metadata_header2 = ZbiBuilder::simple_header(ZbiType::Unknown, 0x40);
        let mut driver_metadata_header3 = ZbiBuilder::simple_header(ZbiType::Unknown, 0x40);

        driver_metadata_header1.zbi_type =
            U32::<LittleEndian>::new((0xABCD << 8) | ZbiType::DriverMetadata as u32);
        assert_eq!(
            ZbiParser::get_type(driver_metadata_header1.zbi_type.get()),
            ZbiType::DriverMetadata
        );

        driver_metadata_header2.zbi_type =
            U32::<LittleEndian>::new((0xDCBA << 8) | ZbiType::DriverMetadata as u32);
        assert_eq!(
            ZbiParser::get_type(driver_metadata_header2.zbi_type.get()),
            ZbiType::DriverMetadata
        );

        driver_metadata_header3.zbi_type =
            U32::<LittleEndian>::new((0xEEEE << 8) | ZbiType::DriverMetadata as u32);
        assert_eq!(
            ZbiParser::get_type(driver_metadata_header3.zbi_type.get()),
            ZbiType::DriverMetadata
        );

        let (zbi, builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Container, 0))
            .add_header(driver_metadata_header1)
            .add_item(0xFC0)
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, 0xF80))
            .add_item(0xF80)
            .add_header(driver_metadata_header2)
            .add_item(0x40)
            .add_header(driver_metadata_header3)
            .add_item(0x40)
            .calculate_item_length()
            .generate()
            .expect("failed to create zbi");
        let mut parser = ZbiParser::new(zbi)
            .set_store_item(ZbiType::DriverMetadata)
            .set_store_item(ZbiType::Crashlog)
            .parse()
            .expect("Failed to parse ZBI");

        // Check we set the ZBI up correctly in this test. The first driver metadata should end
        // at the end of the first page, and the third driver metadata should start at the start
        // of the third page.
        assert_eq!(parser.items.get(&ZbiType::Crashlog).unwrap()[0].header_offset, 0x1000);
        assert_eq!(parser.items.get(&ZbiType::DriverMetadata).unwrap()[2].header_offset, 0x2000);

        check_item_bytes(&builder, &parser);

        assert!(parser.release_item(ZbiType::DriverMetadata).is_ok());
        assert!(parser.try_get_item(ZbiType::DriverMetadata).is_err());

        assert_eq!(
            parser.decommit_ranges,
            vec![
                // The first page contains the ZBI container header, and one of the driver
                // metadata items which we just released.
                DecommitRange { start: 0x0, end: 0x1000 },
                // Third page only contains a single driver metadata item.
                DecommitRange { start: 0x2000, end: 0x3000 },
            ]
        );

        check_item_bytes(&builder, &parser);
    }

    #[fuchsia::test]
    async fn unknown_items_are_decommitted() {
        let (zbi, builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Container, 0))
            .add_header(ZbiBuilder::simple_header(ZbiType::Unknown, 0x1500))
            .add_item(0x1500)
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, 0x100))
            .add_item(0x100)
            .calculate_item_length()
            .generate()
            .expect("failed to create zbi");
        let parser = ZbiParser::new(zbi).parse().expect("Failed to parse ZBI");

        check_extracted_items(&parser, &[ZbiType::Crashlog]);
        check_item_bytes(&builder, &parser);

        assert_eq!(
            parser.decommit_ranges,
            vec![
                // First page just contains an unknown item and the container header.
                DecommitRange { start: 0x0, end: 0x1000 },
            ]
        );
    }

    #[fuchsia::test]
    async fn get_once_item_zeroes_parent_memory() {
        let (zbi, builder) = ZbiBuilder::new()
            .add_header(ZbiBuilder::simple_header(ZbiType::Container, 0))
            .add_header(ZbiBuilder::simple_header(ZbiType::Crashlog, 2048))
            .add_item(2048)
            .add_header(ZbiBuilder::simple_header(ZbiType::ImageArgs, 2048))
            .add_item(2048)
            .add_header(ZbiBuilder::simple_header(ZbiType::ImageArgs, 8192))
            .add_item(8192)
            .add_header(ZbiBuilder::simple_header(ZbiType::StorageBootfsFactory, 32))
            .add_item(32)
            .calculate_item_length()
            .generate()
            .expect("failed to create zbi");

        let mut parser = ZbiParser::new(zbi).parse().expect("Failed to parse ZBI");

        check_extracted_items(
            &parser,
            &[ZbiType::Crashlog, ZbiType::ImageArgs, ZbiType::StorageBootfsFactory],
        );
        check_item_bytes(&builder, &parser);

        assert!(parser.try_get_item(ZbiType::ImageArgs).is_ok());

        let first_image_item = parser.items[&ZbiType::ImageArgs][0];
        let second_image_item = parser.items[&ZbiType::ImageArgs][0];

        assert!(parser.release_item(ZbiType::ImageArgs).is_ok());

        // The item's actual memory in the VMO has been zeroed.
        let mut bytes = vec![0; ZBI_HEADER_SIZE + first_image_item.item_length as usize];
        let expected_bytes = vec![0; ZBI_HEADER_SIZE + first_image_item.item_length as usize];
        parser.vmo.read(&mut bytes, first_image_item.header_offset.into()).unwrap();
        assert_eq!(bytes, expected_bytes);

        let mut bytes = vec![0; ZBI_HEADER_SIZE + second_image_item.item_length as usize];
        let expected_bytes = vec![0; ZBI_HEADER_SIZE + second_image_item.item_length as usize];
        parser.vmo.read(&mut bytes, second_image_item.header_offset.into()).unwrap();
        assert_eq!(bytes, expected_bytes);

        // Trying to get the item again results in a failure.
        assert!(parser.try_get_item(ZbiType::ImageArgs).is_err());

        assert_eq!(
            parser.decommit_ranges,
            vec![
                // This covers both the first and second ImageArgs item. Note the overlap in the
                // range since we have calculated this in one pass without trying to de-overlap,
                // but an additional syscall is worth the reduction in code complexity.
                DecommitRange { start: 0x1000, end: 0x2000 },
                DecommitRange { start: 0x1000, end: 0x3000 }
            ]
        );

        // Check bytes for all remaining items to ensure we didn't wipe something unexpectedly.
        check_item_bytes(&builder, &parser);
    }
}
