// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::{
        asset::{AssetCollectionBuilder, AssetLoader, AssetLoaderImpl},
        family::{FamilyOrAliasBuilder, FontFamilyBuilder},
        inspect::ServiceInspectData,
        typeface::{Typeface, TypefaceCollectionBuilder, TypefaceId},
        FontService,
    },
    anyhow::{format_err, Error},
    font_info::FontInfoLoaderImpl,
    fuchsia_inspect as finspect,
    fuchsia_syslog::fx_vlog,
    fuchsia_trace as trace,
    manifest::{v2, FontManifestWrapper, FontsManifest},
    std::{
        collections::BTreeMap,
        convert::TryFrom,
        fmt::{self, Display},
        path::{Path, PathBuf},
        sync::Arc,
    },
    thiserror::Error,
    unicase::UniCase,
};

/// Builder for [`FontService`]. Allows populating the fields that remain immutable for the
/// lifetime of the service.
///
/// Create a new builder with [`new()`](FontServiceBuilder::new), then populate using
/// [`load_manifest()`](FontServiceBuilder::load_manifest), and finally construct a `FontService`
/// using [`build()`](FontServiceBuilder::build).
#[derive(Debug)]
pub struct FontServiceBuilder<'a, L>
where
    L: AssetLoader,
{
    manifests: Vec<ManifestOrPath>,
    assets: AssetCollectionBuilder<L>,
    /// Maps the font family name from the manifest (`families.family`) to a FamilyOrAlias.
    families: BTreeMap<UniCase<String>, FamilyOrAliasBuilder>,
    fallback_collection: TypefaceCollectionBuilder,
    inspect_root: &'a finspect::Node,
}

impl<'a> FontServiceBuilder<'a, AssetLoaderImpl> {
    /// Creates a new, empty builder with a real asset loader.
    pub fn with_default_asset_loader(
        cache_capacity_bytes: u64,
        inspect_root: &'a finspect::Node,
    ) -> FontServiceBuilder<'a, AssetLoaderImpl> {
        FontServiceBuilder::<'a, AssetLoaderImpl>::new(
            AssetLoaderImpl::new(),
            cache_capacity_bytes,
            inspect_root,
        )
    }
}

impl<'a, L> FontServiceBuilder<'a, L>
where
    L: AssetLoader,
{
    /// Creates a new, empty builder.
    pub fn new(
        asset_loader: L,
        cache_capacity_bytes: u64,
        inspect_root: &'a finspect::Node,
    ) -> FontServiceBuilder<'a, L> {
        FontServiceBuilder {
            manifests: vec![],
            assets: AssetCollectionBuilder::new(asset_loader, cache_capacity_bytes, inspect_root),
            families: BTreeMap::new(),
            fallback_collection: TypefaceCollectionBuilder::new(),
            inspect_root,
        }
    }

    /// Add a manifest path to be parsed and processed.
    pub fn add_manifest_from_file(&mut self, manifest_path: &Path) -> &mut Self {
        self.manifests.push(ManifestOrPath::Path(manifest_path.to_path_buf()));
        self
    }

    /// Adds a parsed manifest to be processed.
    #[allow(dead_code)]
    #[cfg(test)]
    pub fn add_manifest(&mut self, manifest_wrapper: FontManifestWrapper) -> &mut Self {
        self.manifests.push(ManifestOrPath::Manifest(manifest_wrapper));
        self
    }

    /// Tries to build a [`FontService`] from the provided manifests, with some additional error
    /// checking.
    pub async fn build(mut self) -> Result<FontService<L>, Error> {
        let manifest_paths = self.manifests.iter().map(ManifestOrPath::to_string).collect();
        let manifests: Result<Vec<(FontManifestWrapper, Option<PathBuf>)>, Error> = self
            .manifests
            .drain(..)
            .map(|manifest_or_path| match manifest_or_path {
                ManifestOrPath::Manifest(manifest) => Ok((manifest, None)),
                ManifestOrPath::Path(path) => {
                    fx_vlog!(1, "Loading manifest {:?}", &path);
                    Ok((FontsManifest::load_from_file(&path)?, Some(path)))
                }
            })
            .collect();

        for (wrapper, path) in manifests? {
            match wrapper {
                FontManifestWrapper::Version1(v1) => {
                    self.add_fonts_from_manifest_v1(v1, path).await?
                }
                FontManifestWrapper::Version2(v2) => {
                    self.add_fonts_from_manifest_v2(v2, path).await?
                }
            }
        }

        // It's fine to have no fallback collection IFF we loaded an empty manifest.
        if self.fallback_collection.is_empty() && !self.families.is_empty() {
            return Err(FontServiceBuilderError::NoFallbackCollection.into());
        }

        let assets = self.assets.build();
        let families = self.families.into_iter().map(|(key, value)| (key, value.build())).collect();
        let fallback_collection = self.fallback_collection.build();

        let inspect_data = ServiceInspectData::new(
            self.inspect_root,
            manifest_paths,
            &assets,
            &families,
            &fallback_collection,
        );

        Ok(FontService { assets, families, fallback_collection, inspect_data })
    }

    async fn add_fonts_from_manifest_v2(
        &mut self,
        manifest: v2::FontsManifest,
        manifest_path: Option<PathBuf>,
    ) -> Result<(), Error> {
        // Hold on to the typefaces defined in this manifest so that they can be referenced when
        // building the fallback chain.
        let mut manifest_typefaces: BTreeMap<TypefaceId, Arc<Typeface>> = BTreeMap::new();

        for mut manifest_family in manifest.families {
            // Register the family itself
            let family_name = UniCase::new(manifest_family.name.clone());
            let family = match self.families.entry(family_name.clone()).or_insert_with(|| {
                FamilyOrAliasBuilder::Family(FontFamilyBuilder::new(
                    family_name.to_string(),
                    manifest_family.generic_family,
                ))
            }) {
                FamilyOrAliasBuilder::Family(f) => f,
                FamilyOrAliasBuilder::Alias(_, _) => {
                    return Err(FontServiceBuilderError::AliasFamilyConflict {
                        conflicting_name: family_name.to_string(),
                        manifest_path: manifest_path.clone(),
                    }
                    .into());
                }
            };

            // Register the family's assets and their typefaces.

            // We have to use `.drain()` here (instead of moving `assets` out) in order to leave
            // `manifest_family` in a valid state to be able to keep using it further down.
            for manifest_asset in manifest_family.assets.drain(..) {
                let asset_id = self.assets.add_or_get_asset_id(&manifest_asset);
                for manifest_typeface in manifest_asset.typefaces {
                    if manifest_typeface.code_points.is_empty() {
                        return Err(FontServiceBuilderError::NoCodePoints {
                            asset_name: manifest_asset.file_name.to_string(),
                            typeface_idx: manifest_typeface.index,
                            manifest_path: manifest_path.clone(),
                        }
                        .into());
                    }
                    let generic_family = manifest_family.generic_family;
                    let typeface_id = TypefaceId { asset_id, index: manifest_typeface.index };
                    // Deduplicate typefaces across multiple manifests
                    if !family.has_typeface_id(&typeface_id) {
                        // .unwrap() because we already checked for missing code points above
                        let typeface = Arc::new(
                            Typeface::new(asset_id, manifest_typeface, generic_family).unwrap(),
                        );
                        manifest_typefaces.insert(typeface_id, typeface.clone());
                        family.add_typeface_once(typeface);
                    }
                }
            }

            // Above, we're working with `family` mutably borrowed from `self.families`. We have to
            // finish using any mutable references to `self.families` before we can create further
            // references to `self.families` below.

            // Register aliases
            let aliases = FamilyOrAliasBuilder::aliases_from_family(&manifest_family);
            for (key, value) in aliases {
                match self.families.get(&key) {
                    None => {
                        self.families.insert(key, value);
                    }
                    Some(FamilyOrAliasBuilder::Family(_)) => {
                        return Err(FontServiceBuilderError::AliasFamilyConflict {
                            conflicting_name: key.to_string(),
                            manifest_path: manifest_path.clone(),
                        }
                        .into());
                    }
                    Some(FamilyOrAliasBuilder::Alias(other_family_name, _)) => {
                        // If the alias exists then it must be for the same font family.
                        if *other_family_name != family_name {
                            return Err(FontServiceBuilderError::AmbiguousAlias {
                                alias: key.to_string(),
                                canonical_1: other_family_name.to_string(),
                                canonical_2: family_name.to_string(),
                                manifest_path: manifest_path.clone(),
                            }
                            .into());
                        }
                    }
                }
            }
        }

        // We add all the fallback typefaces, preserving their order from the product font
        // configuration file.
        //
        // Unfortunately, when there are multiple manifests with fallback chains, the best we can
        // do is concatenate the fallback chains (with de-duplication). Multiple manifests are not
        // expected in production use cases, so this isn't as awful as it sounds.
        for fallback_typeface in &manifest.fallback_chain {
            let asset_id = self
                .assets
                .get_asset_id_by_name(&fallback_typeface.file_name)
                .ok_or_else(|| FontServiceBuilderError::UnknownFallbackEntry {
                    file_name: fallback_typeface.file_name.clone(),
                    index: fallback_typeface.index,
                    manifest_path: manifest_path.clone(),
                })?;

            let typeface_id = TypefaceId { asset_id, index: fallback_typeface.index };
            if !self.fallback_collection.has_typeface_id(&typeface_id) {
                let typeface = manifest_typefaces
                    .get(&typeface_id)
                    .expect("Invalid state in FontServiceBuilder")
                    .clone();
                self.fallback_collection.add_typeface_once(typeface);
            }
        }

        Ok(())
    }

    async fn add_fonts_from_manifest_v1(
        &mut self,
        manifest_v1: FontsManifest,
        manifest_path: Option<PathBuf>,
    ) -> Result<(), Error> {
        let path_string: String = manifest_path
            .as_ref()
            .map(|path| path.to_string_lossy().to_string())
            .unwrap_or_default();
        trace::duration!(
                "fonts",
                "font_service:builder:add_fonts_from_manifest_v2",
                "path" => &path_string[..]);
        let manifest_v2 = self.convert_manifest_v1_to_v2(manifest_v1).await.map_err(|e| {
            FontServiceBuilderError::ConversionFromV1 {
                manifest_path: manifest_path.clone(),
                cause: e.into(),
            }
        })?;
        self.add_fonts_from_manifest_v2(manifest_v2, manifest_path).await
    }

    /// Converts data format from manifest v1 to v2 and loads character sets for any typefaces that
    /// lack them.
    async fn convert_manifest_v1_to_v2(
        &self,
        manifest_v1: FontsManifest,
    ) -> Result<v2::FontsManifest, Error> {
        let mut manifest_v2 = v2::FontsManifest::try_from(manifest_v1)?;
        let asset_loader = AssetLoaderImpl::new();
        let font_info_loader = FontInfoLoaderImpl::new()?;

        for manifest_family in &mut manifest_v2.families {
            for manifest_asset in &mut manifest_family.assets {
                for manifest_typeface in &mut manifest_asset.typefaces {
                    if manifest_typeface.code_points.is_empty() {
                        match &manifest_asset.location {
                            v2::AssetLocation::LocalFile(v2::LocalFileLocator { directory }) => {
                                let asset_path = directory.join(&manifest_asset.file_name);
                                let buffer = asset_loader.load_vmo_from_path(&asset_path)?;
                                let font_info = {
                                    trace::duration!("fonts", "FontInfoLoaderImpl:load_font_info");
                                    font_info_loader
                                        .load_font_info(buffer, manifest_typeface.index)?
                                };
                                manifest_typeface.code_points = font_info.char_set;
                            }
                            _ => {
                                return Err(format_err!(
                                    "Impossible asset location: {:?}",
                                    &manifest_asset
                                ));
                            }
                        }
                    }
                }
            }
        }

        Ok(manifest_v2)
    }
}

#[allow(dead_code)]
#[derive(Debug)]
enum ManifestOrPath {
    Manifest(FontManifestWrapper),
    Path(PathBuf),
}

impl Display for ManifestOrPath {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ManifestOrPath::Manifest(_manifest) => write!(f, "{}", "FontManifestWrapper {{ ... }}"),
            ManifestOrPath::Path(path) => write!(f, "{}", path.to_string_lossy().to_string()),
        }
    }
}

/// Errors arising from the use of [`FontServiceBuilder`].
#[derive(Debug, Error)]
pub enum FontServiceBuilderError {
    /// A name was used as both a canonical family name and a font family alias.
    #[error(
        "Conflict in {:?}: {} cannot be both a canonical family name and an alias",
        manifest_path,
        conflicting_name
    )]
    AliasFamilyConflict { conflicting_name: String, manifest_path: Option<PathBuf> },

    /// One string was used as an alias for two different font families.
    #[error(
        "Conflict in {:?}: {} cannot be an alias for both {} and {}",
        manifest_path,
        alias,
        canonical_1,
        canonical_2
    )]
    AmbiguousAlias {
        alias: String,
        canonical_1: String,
        canonical_2: String,
        manifest_path: Option<PathBuf>,
    },

    /// Something went wrong when converting a manifest from v1 to v2.
    #[error("Conversion from manifest v1 failed in {:?}: {:?}", manifest_path, cause)]
    ConversionFromV1 {
        manifest_path: Option<PathBuf>,
        #[source]
        cause: Error,
    },

    /// The font manifest's fallback chain referenced an undeclared typeface.
    #[error(
        "Unknown typeface in fallback chain in {:?}: file name \'{}\', index {}",
        manifest_path,
        file_name,
        index
    )]
    UnknownFallbackEntry { file_name: String, index: u32, manifest_path: Option<PathBuf> },

    /// None of the loaded manifests contained a non-empty fallback chain.
    #[error("Need at least one fallback font family")]
    NoFallbackCollection,

    /// The manifest did not have defined code points for a particular typeface.
    #[error("Missing code points for \"{}\"[{}] in {:?}", asset_name, typeface_idx, manifest_path)]
    NoCodePoints { asset_name: String, typeface_idx: u32, manifest_path: Option<PathBuf> },
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::font_service::{
            family::{FamilyOrAlias, FontFamily},
            typeface::Collection as TypefaceCollection,
            AssetId,
        },
        char_set::CharSet,
        fidl_fuchsia_fonts::{GenericFontFamily, Slant, Width, WEIGHT_BOLD, WEIGHT_NORMAL},
        manifest::{serde_ext::StyleOptions, v2},
        maplit::{btreemap, btreeset},
        pretty_assertions::assert_eq,
        std::sync::Arc,
        unicase::UniCase,
    };

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_multiple_overlapping_manifests() -> Result<(), Error> {
        let inspector = finspect::Inspector::new();
        let mut builder = FontServiceBuilder::with_default_asset_loader(5000, inspector.root());
        builder
            .add_manifest(FontManifestWrapper::Version2(v2::FontsManifest {
                families: vec![v2::Family {
                    name: "Alpha".to_string(),
                    aliases: vec![v2::FontFamilyAliasSet::without_overrides(vec![
                        "A", "Aleph", "Alif",
                    ])?],
                    generic_family: Some(GenericFontFamily::SansSerif),
                    assets: vec![
                        v2::Asset {
                            file_name: "Alpha-Regular.ttf".to_string(),
                            location: v2::AssetLocation::LocalFile(v2::LocalFileLocator {
                                directory: PathBuf::from("/pkg/config/data/assets"),
                            }),
                            typefaces: vec![v2::Typeface {
                                index: 0,
                                languages: vec!["en".to_string()],
                                style: v2::Style {
                                    slant: Slant::Upright,
                                    weight: WEIGHT_NORMAL,
                                    width: Width::Normal,
                                },
                                code_points: CharSet::new(vec![0x1, 0x2, 0x3]),
                            }],
                        },
                        v2::Asset {
                            file_name: "Alpha-Bold.ttf".to_string(),
                            location: v2::AssetLocation::LocalFile(v2::LocalFileLocator {
                                directory: PathBuf::from("/pkg/config/data/assets"),
                            }),
                            typefaces: vec![v2::Typeface {
                                index: 0,
                                languages: vec!["en".to_string()],
                                style: v2::Style {
                                    slant: Slant::Upright,
                                    weight: WEIGHT_BOLD,
                                    width: Width::Normal,
                                },
                                code_points: CharSet::new(vec![0x1, 0x2, 0x3]),
                            }],
                        },
                    ],
                }],
                fallback_chain: vec![
                    v2::TypefaceId { file_name: "Alpha-Bold.ttf".to_string(), index: 0 },
                    v2::TypefaceId { file_name: "Alpha-Regular.ttf".to_string(), index: 0 },
                ],
            }))
            .add_manifest(FontManifestWrapper::Version2(v2::FontsManifest {
                families: vec![v2::Family {
                    name: "Alpha".to_string(),
                    aliases: vec![
                        v2::FontFamilyAliasSet::without_overrides(vec![
                            "A",
                            "Aleph",
                            "Alpha Ordinary",
                        ])?,
                        // Note different languages in second manifest's "Alif" alias
                        v2::FontFamilyAliasSet::new(
                            vec!["Alif"],
                            StyleOptions::default(),
                            vec!["en", "ar"],
                        )?,
                    ],
                    generic_family: Some(GenericFontFamily::SansSerif),
                    assets: vec![v2::Asset {
                        file_name: "Alpha-Regular.ttf".to_string(),
                        location: v2::AssetLocation::LocalFile(v2::LocalFileLocator {
                            directory: PathBuf::from("/pkg/config/data/assets"),
                        }),
                        typefaces: vec![v2::Typeface {
                            index: 0,
                            languages: vec!["en".to_string()],
                            style: v2::Style {
                                slant: Slant::Upright,
                                weight: WEIGHT_NORMAL,
                                width: Width::Expanded, // Note difference
                            },
                            code_points: CharSet::new(vec![0x1, 0x2, 0x3]),
                        }],
                    }],
                }],
                fallback_chain: vec![v2::TypefaceId {
                    file_name: "Alpha-Regular.ttf".to_string(),
                    index: 0,
                }],
            }));

        let service = builder.build().await?;

        let expected_typeface_regular = Arc::new(Typeface {
            asset_id: AssetId(0),
            font_index: 0,
            slant: Slant::Upright,
            weight: WEIGHT_NORMAL,
            width: Width::Normal, // First version wins
            languages: btreeset!["en".to_string()],
            char_set: CharSet::new(vec![0x1, 0x2, 0x3]),
            generic_family: Some(GenericFontFamily::SansSerif),
        });

        let expected_typeface_bold = Arc::new(Typeface {
            asset_id: AssetId(1),
            font_index: 0,
            slant: Slant::Upright,
            weight: WEIGHT_BOLD,
            width: Width::Normal,
            languages: btreeset!["en".to_string()],
            char_set: CharSet::new(vec![0x1, 0x2, 0x3]),
            generic_family: Some(GenericFontFamily::SansSerif),
        });

        assert_eq!(
            service.families,
            btreemap!(
            UniCase::new("Alpha".to_string()) =>
                FamilyOrAlias::Family(FontFamily {
                    name: "Alpha".to_string(),
                    faces: TypefaceCollection {
                        faces: vec![
                            expected_typeface_regular.clone(),
                            expected_typeface_bold.clone()
                        ]
                    },
                    generic_family: Some(GenericFontFamily::SansSerif)
            }),
            UniCase::new("A".to_string()) =>
                FamilyOrAlias::Alias(UniCase::new("Alpha".to_string()), None),
            UniCase::new("Aleph".to_string()) =>
                FamilyOrAlias::Alias(UniCase::new("Alpha".to_string()), None),
            // First version of "Alif" wins
            UniCase::new("Alif".to_string()) =>
                FamilyOrAlias::Alias(UniCase::new("Alpha".to_string()), None),
            UniCase::new("Alpha Ordinary".to_string()) =>
                FamilyOrAlias::Alias(UniCase::new("Alpha".to_string()), None),
            UniCase::new("AlphaOrdinary".to_string()) =>
                FamilyOrAlias::Alias(UniCase::new("Alpha".to_string()), None),)
        );

        assert_eq!(service.assets.len(), 2);

        assert_eq!(
            service.fallback_collection,
            TypefaceCollection {
                faces: vec![expected_typeface_bold.clone(), expected_typeface_regular.clone()]
            }
        );

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_empty_manifest() -> Result<(), Error> {
        let inspector = finspect::Inspector::new();
        let manifest = FontManifestWrapper::Version2(v2::FontsManifest {
            families: vec![],
            fallback_chain: vec![],
        });
        let mut builder = FontServiceBuilder::with_default_asset_loader(5000, inspector.root());
        builder.add_manifest(manifest);
        builder.build().await?; // Should succeed
        Ok(())
    }
}
