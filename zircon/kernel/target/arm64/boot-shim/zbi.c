// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zbi.h"

#include <stdbool.h>
#include <string.h>

struct check_state {
  zbi_header_t** err;
};

static bool is_zbi_container(const zbi_header_t* hdr) {
  return (hdr->type == ZBI_TYPE_CONTAINER) && (hdr->magic == ZBI_ITEM_MAGIC) &&
         (hdr->extra == ZBI_CONTAINER_MAGIC);
}

static zbi_result_t for_each_check_entry(zbi_header_t* hdr, void* payload, void* cookie) {
  struct check_state* const state = cookie;

  zbi_result_t result = ZBI_RESULT_OK;

  if (hdr->magic != ZBI_ITEM_MAGIC) {
    result = ZBI_RESULT_BAD_MAGIC;
  } else if ((hdr->flags & ZBI_FLAGS_VERSION) == 0) {
    result = ZBI_RESULT_BAD_VERSION;
  } else if ((hdr->flags & ZBI_FLAGS_CRC32) == 0 && hdr->crc32 != ZBI_ITEM_NO_CRC32) {
    result = ZBI_RESULT_BAD_CRC;
  }

  // If we found a problem, try to report it back up to the caller.
  if (state->err != NULL && result != ZBI_RESULT_OK) {
    *state->err = hdr;
  }

  return result;
}

zbi_result_t zbi_check(const void* base, zbi_header_t** err) {
  if (!base) {
    return ZBI_RESULT_ERROR;
  }

  zbi_result_t res = ZBI_RESULT_OK;
  const zbi_header_t* header = base;

  if (header->type != ZBI_TYPE_CONTAINER) {
    res = ZBI_RESULT_BAD_TYPE;
  } else if (header->extra != ZBI_CONTAINER_MAGIC || header->magic != ZBI_ITEM_MAGIC) {
    res = ZBI_RESULT_BAD_MAGIC;
  } else if ((header->flags & ZBI_FLAGS_VERSION) == 0) {
    res = ZBI_RESULT_BAD_VERSION;
  } else if ((header->flags & ZBI_FLAGS_CRC32) == 0 && header->crc32 != ZBI_ITEM_NO_CRC32) {
    res = ZBI_RESULT_BAD_CRC;
  }

  // Something was wrong with the container.  Don't even attempt to process
  // the rest of the image.  Return diagnostic information back to the caller
  // if they requested it.
  if (res != ZBI_RESULT_OK) {
    if (err) {
      *err = (zbi_header_t*)header;
    }
    return res;
  }

  struct check_state state = {.err = err};
  res = zbi_for_each(base, for_each_check_entry, &state);

  if (err && res == ZBI_RESULT_ERR_TRUNCATED) {
    // A truncated image perhaps indicates a problem with the container?
    *err = (zbi_header_t*)header;
  }

  return res;
}

zbi_result_t zbi_for_each(const void* base, const zbi_foreach_cb_t callback, void* cookie) {
  if (!base || !callback) {
    return ZBI_RESULT_ERROR;
  }

  zbi_header_t* header = (zbi_header_t*)(base);

  // Skip container header.
  const uint32_t totalSize = (uint32_t)sizeof(zbi_header_t) + header->length;
  uint32_t offset = sizeof(zbi_header_t);
  while (offset < totalSize) {
    zbi_header_t* entryHeader = (zbi_header_t*)(base + offset);

    zbi_result_t result = callback(entryHeader, entryHeader + 1, cookie);

    if (result != ZBI_RESULT_OK) {
      return result;
    }

    if ((offset + entryHeader->length + sizeof(zbi_header_t)) > totalSize) {
      return ZBI_RESULT_ERR_TRUNCATED;
    }

    offset = ZBI_ALIGN(offset + entryHeader->length + sizeof(zbi_header_t));
  }

  return ZBI_RESULT_OK;
}

zbi_result_t zbi_create_entry_with_payload(void* base, const size_t capacity, uint32_t type,
                                           uint32_t extra, uint32_t flags, const void* payload,
                                           uint32_t payload_length) {
  if (!base || !payload) {
    return ZBI_RESULT_ERROR;
  }

  uint8_t* new_section;
  zbi_result_t result =
      zbi_create_entry(base, capacity, type, extra, flags, payload_length, (void**)&new_section);

  if (result != ZBI_RESULT_OK) {
    return result;
  }

  // Copy in the payload.
  memcpy(new_section, payload, payload_length);
  return ZBI_RESULT_OK;
}

zbi_result_t zbi_create_entry(void* base, size_t capacity, uint32_t type, uint32_t extra,
                              uint32_t flags, uint32_t payload_length, void** payload) {
  if (!base || !payload) {
    return ZBI_RESULT_ERROR;
  }

  // We don't support CRC computation (yet?)
  if (flags & ZBI_FLAGS_CRC32) {
    return ZBI_RESULT_ERROR;
  }

  zbi_header_t* hdr = (zbi_header_t*)base;

  // Make sure we were actually passed a bootdata container.
  if (!is_zbi_container(hdr)) {
    return ZBI_RESULT_BAD_TYPE;
  }

  // Make sure we have enough space in the buffer to append the new section.
  if (capacity < sizeof(*hdr) || capacity - sizeof(*hdr) < hdr->length) {
    return ZBI_RESULT_TOO_BIG;
  }
  const size_t available = capacity - sizeof(*hdr) - hdr->length;
  if (available < sizeof(*hdr) || available - sizeof(*hdr) < ZBI_ALIGN(payload_length)) {
    return ZBI_RESULT_TOO_BIG;
  }

  // Fill in the new section header.
  zbi_header_t* new_header = (void*)((uint8_t*)(hdr + 1) + hdr->length);
  *new_header = (zbi_header_t){
      .type = type,
      .length = payload_length,
      .extra = extra,
      .flags = flags | ZBI_FLAGS_VERSION,
      .magic = ZBI_ITEM_MAGIC,
      .crc32 = ZBI_ITEM_NO_CRC32,
  };

  // Tell the caller where to fill in the payload.
  *payload = new_header + 1;

  // Update the container header, always keeping the length aligned.
  hdr->length += (uint32_t)sizeof(*new_header) + new_header->length;
  if (hdr->length % ZBI_ALIGNMENT != 0) {
    // It was already verified that the capacity can fit the aligned length.
    uint32_t aligned_length = ZBI_ALIGN(hdr->length);
    memset((uint8_t*)(hdr + 1) + hdr->length, 0, aligned_length - hdr->length);
    hdr->length = aligned_length;
  }

  return ZBI_RESULT_OK;
}
