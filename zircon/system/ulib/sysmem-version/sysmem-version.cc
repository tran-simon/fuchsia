// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/sysmem-version/sysmem-version.h"

#include <fidl/fuchsia.sysmem/cpp/common_types.h>
#include <fidl/fuchsia.sysmem/cpp/wire.h>
#include <fidl/fuchsia.sysmem2/cpp/wire.h>
#include <inttypes.h>
#include <lib/fidl/cpp/wire/traits.h>
#include <zircon/assert.h>

#include <map>
#include <set>

#include "log.h"

namespace sysmem {
namespace {

// Can be replaced with std::remove_cvref<> when C++20.
template <typename T>
struct RemoveCVRef : internal::TypeIdentity<std::remove_cv_t<std::remove_reference_t<T>>> {};
template <typename T>
using RemoveCVRef_t = typename RemoveCVRef<T>::type;

// The meaning of "fidl scalar" here includes flexible enums, which are actually just final classes
// with a single private scalar field after codegen, but the have an operator uint32_t() or
// operator uint64_t() (the ones we care about here) so we detect that way (at least for now).
template <typename T, typename Enable = void>
struct IsFidlScalar : std::false_type {};
template <typename T>
struct IsFidlScalar<
    T, typename std::enable_if<fidl::IsFidlType<T>::value &&
                               (std::is_arithmetic<T>::value || std::is_enum<T>::value)>::type>
    : std::true_type {};
template <typename T>
struct IsFidlScalar<T, typename std::enable_if<fidl::IsFidlType<T>::value &&
                                               (internal::HasOperatorUInt32<T>::value ||
                                                internal::HasOperatorUInt64<T>::value)>::type>
    : std::true_type {};

template <typename V2Type, typename V1Type, typename Enable = void>
struct IsCompatibleFidlScalarTypes : std::false_type {};
template <typename V2Type, typename V1Type>
struct IsCompatibleFidlScalarTypes<
    V2Type, V1Type,
    typename std::enable_if<
        // must be able to write to v2
        !std::is_const<typename std::remove_reference_t<V2Type>>::value &&
        IsFidlScalar<RemoveCVRef_t<V2Type>>::value && IsFidlScalar<RemoveCVRef_t<V1Type>>::value &&
        std::is_same<FidlUnderlyingTypeOrType_t<RemoveCVRef_t<V2Type>>,
                     FidlUnderlyingTypeOrType_t<RemoveCVRef_t<V1Type>>>::value>::type>
    : std::true_type {};
template <typename V2, typename V1>
inline constexpr bool IsCompatibleFidlScalarTypes_v = IsCompatibleFidlScalarTypes<V2, V1>::value;

// The C++ style guide discourages macros, but does not prohibit them.  To operate on a bunch of
// separate fields with different names, it's a choice among tons of error-prone repetitive
// verbosity, macros, or more abstraction than I think anyone would want.  Macros are the least-bad
// option (among those options considred so far).  Feel free to propose another option.

// This macro is needed to cut down on the noise from the exact same error check occurring every
// place we might early return a failure.
#define OK_OR_RET_ERROR(foo)    \
  do {                          \
    if (!(foo).is_ok()) {       \
      LOG(ERROR, "!is_ok()");   \
      return fpromise::error(); \
    }                           \
  } while (false)

// This macro is needed to ensure that we don't cross-wire fields as we're converting from V1 to V2
// and to cut down on the noise from the exact same code structure for most fields.  Also, this way
// we can include a static check that the type of the v2 field exactly matches the type of the v1
// field, which doesn't generate any actual code, yet needs to be repeated for each field being
// converted.
//
// This handles scalar fields, including enum fields.  It doesn't handle vector fields, tensor
// fields, struct fields, or table fields.
//
// All bool fields are set regardless of false or true.  Other scalar fields are only set if not
// equal to zero.
#define PROCESS_SCALAR_FIELD_V1(field_name)                                                \
  do {                                                                                     \
    using V2FieldType = std::remove_reference_t<decltype(v2b.field_name().value())>;       \
    /* double parens are significant here */                                               \
    using V1FieldType = std::remove_reference_t<decltype((v1.field_name()))>;              \
    static_assert(IsCompatibleFidlScalarTypes_v<V2FieldType, V1FieldType>);                \
    using V2UnderlyingType = FidlUnderlyingTypeOrType_t<V2FieldType>;                      \
    using V1UnderlyingType = FidlUnderlyingTypeOrType_t<V1FieldType>;                      \
    if (std::is_same<bool, RemoveCVRef_t<V1FieldType>>::value ||                           \
        static_cast<bool>(v1.field_name())) {                                              \
      v2b.field_name().emplace(static_cast<V2FieldType>(                                   \
          static_cast<V2UnderlyingType>(static_cast<V1UnderlyingType>(v1.field_name())))); \
    }                                                                                      \
  } while (false)

#define PROCESS_WIRE_SCALAR_FIELD_V1(field_name)                                         \
  do {                                                                                   \
    using V2FieldType = std::remove_reference_t<decltype(v2b.field_name())>;             \
    /* double parens are significant here */                                             \
    using V1FieldType = std::remove_reference_t<decltype((v1.field_name))>;              \
    static_assert(IsCompatibleFidlScalarTypes_v<V2FieldType, V1FieldType>);              \
    using V2UnderlyingType = FidlUnderlyingTypeOrType_t<V2FieldType>;                    \
    using V1UnderlyingType = FidlUnderlyingTypeOrType_t<V1FieldType>;                    \
    if (std::is_same<bool, RemoveCVRef_t<V1FieldType>>::value ||                         \
        static_cast<bool>(v1.field_name)) {                                              \
      /* This intentionally allows for implicit conversions for flexible enums */        \
      v2b.set_##field_name(static_cast<V2FieldType>(                                     \
          static_cast<V2UnderlyingType>(static_cast<V1UnderlyingType>(v1.field_name)))); \
    }                                                                                    \
  } while (false)

#define PROCESS_WIRE_SCALAR_FIELD_V1_WITH_ALLOCATOR(field_name)                               \
  do {                                                                                        \
    using V2FieldType = std::remove_reference_t<decltype(v2b.field_name())>;                  \
    /* double parens are significant here */                                                  \
    using V1FieldType = std::remove_reference_t<decltype((v1.field_name))>;                   \
    static_assert(IsCompatibleFidlScalarTypes_v<V2FieldType, V1FieldType>);                   \
    using V2UnderlyingType = FidlUnderlyingTypeOrType_t<V2FieldType>;                         \
    using V1UnderlyingType = FidlUnderlyingTypeOrType_t<V1FieldType>;                         \
    if (std::is_same<bool, RemoveCVRef_t<V1FieldType>>::value ||                              \
        static_cast<bool>(v1.field_name)) {                                                   \
      /* This intentionally allows for implicit conversions for flexible enums */             \
      v2b.set_##field_name(allocator, static_cast<V2FieldType>(static_cast<V2UnderlyingType>( \
                                          static_cast<V1UnderlyingType>(v1.field_name))));    \
    }                                                                                         \
  } while (false)

#define PROCESS_SCALAR_FIELD_V2(field_name)                                                       \
  do {                                                                                            \
    using V1FieldType = std::remove_reference_t<decltype(v1.field_name())>;                       \
    using V2FieldType = std::remove_reference_t<decltype(v2.field_name().value())>;               \
    static_assert(IsCompatibleFidlScalarTypes<V1FieldType, V2FieldType>::value);                  \
    using V1UnderlyingType = FidlUnderlyingTypeOrType_t<V1FieldType>;                             \
    using V2UnderlyingType = FidlUnderlyingTypeOrType_t<V2FieldType>;                             \
    if (v2.field_name().has_value()) {                                                            \
      v1.field_name() = static_cast<V1FieldType>(                                                 \
          static_cast<V1UnderlyingType>(static_cast<V2UnderlyingType>(v2.field_name().value()))); \
    } else {                                                                                      \
      v1.field_name() = static_cast<V1FieldType>(0);                                              \
    }                                                                                             \
  } while (false)
#define PROCESS_WIRE_SCALAR_FIELD_V2(field_name)                                          \
  do {                                                                                    \
    using V1FieldType = decltype(v1.field_name);                                          \
    using V2FieldType = std::remove_reference_t<decltype(v2.field_name())>;               \
    static_assert(IsCompatibleFidlScalarTypes<V1FieldType, V2FieldType>::value);          \
    using V1UnderlyingType = FidlUnderlyingTypeOrType_t<V1FieldType>;                     \
    using V2UnderlyingType = FidlUnderlyingTypeOrType_t<V2FieldType>;                     \
    if (v2.has_##field_name()) {                                                          \
      /* This intentionally allows for implicit conversions for flexible enums */         \
      v1.field_name = static_cast<V1FieldType>(                                           \
          static_cast<V1UnderlyingType>(static_cast<V2UnderlyingType>(v2.field_name()))); \
    } else {                                                                              \
      v1.field_name = static_cast<V1FieldType>(0);                                        \
    }                                                                                     \
  } while (false)

#define ASSIGN_SCALAR(dst, src)                                                                    \
  do {                                                                                             \
    using DstType = typename std::remove_reference_t<decltype((dst))>;                             \
    using SrcType = typename std::remove_reference_t<decltype((src))>;                             \
    static_assert(IsCompatibleFidlScalarTypes_v<DstType, SrcType>);                                \
    using DstUnderlyingType = FidlUnderlyingTypeOrType_t<DstType>;                                 \
    using SrcUnderlyingType = FidlUnderlyingTypeOrType_t<SrcType>;                                 \
    /* This intentionally allows for implicit conversions for flexible enums */                    \
    (dst) =                                                                                        \
        static_cast<DstType>(static_cast<DstUnderlyingType>(static_cast<SrcUnderlyingType>(src))); \
  } while (false)

template <size_t N>
fpromise::result<std::vector<fuchsia_sysmem2::HeapType>> V2CopyFromV1HeapPermittedArrayNatural(
    const std::array<fuchsia_sysmem::HeapType, N>& v1a, const uint32_t v1_count) {
  ZX_DEBUG_ASSERT(v1_count);
  if (v1_count > v1a.size()) {
    LOG(ERROR, "v1_count > v1a.size() - v1_count: %u v1a.size(): %zu", v1_count, v1a.size());
    return fpromise::error();
  }
  std::vector<fuchsia_sysmem2::HeapType> v2a(v1_count);
  for (uint32_t i = 0; i < v1_count; i++) {
    ASSIGN_SCALAR(v2a[i], v1a[i]);
  }
  return fpromise::ok(std::move(v2a));
}

template <size_t N>
fpromise::result<fidl::VectorView<fuchsia_sysmem2::wire::HeapType>> V2CopyFromV1HeapPermittedArray(
    fidl::AnyArena& allocator, const fidl::Array<fuchsia_sysmem::wire::HeapType, N>& v1a,
    const uint32_t v1_count) {
  ZX_DEBUG_ASSERT(v1_count);
  if (v1_count > v1a.size()) {
    LOG(ERROR, "v1_count > v1a.size() - v1_count: %u v1a.size(): %zu", v1_count, v1a.size());
    return fpromise::error();
  }
  fidl::VectorView<fuchsia_sysmem2::wire::HeapType> v2a(allocator, v1_count);
  for (uint32_t i = 0; i < v1_count; i++) {
    ASSIGN_SCALAR(v2a[i], v1a[i]);
  }
  return fpromise::ok(v2a);
}

template <size_t N>
fpromise::result<std::vector<fuchsia_sysmem2::ColorSpace>> V2CopyFromV1ColorSpaceArrayNatural(
    const std::array<fuchsia_sysmem::ColorSpace, N>& v1a, uint32_t v1_count) {
  ZX_DEBUG_ASSERT(v1_count);
  if (v1_count > v1a.size()) {
    LOG(ERROR, "v1_count > v1a.size() - v1_count: %u v1a.size(): %zu", v1_count, v1a.size());
    return fpromise::error();
  }
  std::vector<fuchsia_sysmem2::ColorSpace> v2a(v1_count);
  for (uint32_t i = 0; i < v1_count; i++) {
    v2a[i] = V2CopyFromV1ColorSpace(v1a[i]);
  }
  return fpromise::ok(std::move(v2a));
}

template <size_t N>
fpromise::result<fidl::VectorView<fuchsia_sysmem2::wire::ColorSpace>> V2CopyFromV1ColorSpaceArray(
    fidl::AnyArena& allocator, const fidl::Array<fuchsia_sysmem::wire::ColorSpace, N>& v1a,
    uint32_t v1_count) {
  ZX_DEBUG_ASSERT(v1_count);
  if (v1_count > v1a.size()) {
    LOG(ERROR, "v1_count > v1a.size() - v1_count: %u v1a.size(): %zu", v1_count, v1a.size());
    return fpromise::error();
  }
  fidl::VectorView<fuchsia_sysmem2::wire::ColorSpace> v2a(allocator, v1_count);
  for (uint32_t i = 0; i < v1_count; i++) {
    v2a[i] = V2CopyFromV1ColorSpace(allocator, v1a[i]);
  }
  return fpromise::ok(v2a);
}

template <size_t N>
fpromise::result<std::vector<fuchsia_sysmem2::ImageFormatConstraints>>
V2CopyFromV1ImageFormatConstraintsArrayNatural(
    const std::array<fuchsia_sysmem::ImageFormatConstraints, N>& v1a, const uint32_t v1_count) {
  ZX_DEBUG_ASSERT(v1_count);
  if (v1_count > v1a.size()) {
    LOG(ERROR, "v1_count > v1a.size() - v1_count: %u v1a.size(): %zu", v1_count, v1a.size());
    return fpromise::error();
  }
  std::vector<fuchsia_sysmem2::ImageFormatConstraints> v2a(v1_count);
  for (uint32_t i = 0; i < v1_count; i++) {
    auto result = V2CopyFromV1ImageFormatConstraints(v1a[i]);
    OK_OR_RET_ERROR(result);
    v2a[i] = result.take_value();
  }
  return fpromise::ok(std::move(v2a));
}

template <size_t N>
fpromise::result<fidl::VectorView<fuchsia_sysmem2::wire::ImageFormatConstraints>>
V2CopyFromV1ImageFormatConstraintsArray(
    fidl::AnyArena& allocator,
    const fidl::Array<fuchsia_sysmem::wire::ImageFormatConstraints, N>& v1a,
    const uint32_t v1_count) {
  ZX_DEBUG_ASSERT(v1_count);
  if (v1_count > v1a.size()) {
    LOG(ERROR, "v1_count > v1a.size() - v1_count: %u v1a.size(): %zu", v1_count, v1a.size());
    return fpromise::error();
  }
  fidl::VectorView<fuchsia_sysmem2::wire::ImageFormatConstraints> v2a(allocator, v1_count);
  for (uint32_t i = 0; i < v1_count; i++) {
    auto result = V2CopyFromV1ImageFormatConstraints(allocator, v1a[i]);
    OK_OR_RET_ERROR(result);
    v2a[i] = result.take_value();
  }
  return fpromise::ok(v2a);
}

fpromise::result<> V2CopyFromV1BufferCollectionConstraintsMain(
    fuchsia_sysmem2::BufferCollectionConstraints* v2b_param,
    const fuchsia_sysmem::BufferCollectionConstraints& v1) {
  ZX_DEBUG_ASSERT(v2b_param);
  fuchsia_sysmem2::BufferCollectionConstraints& v2b = *v2b_param;

  // This sets usage regardless of whether the client set any usage bits within usage.  That's
  // checked later (regardless of v1 or v2 client).  If a v1 client said !has_constraints, we
  // won't call the current method and usage field will remain un-set so that
  // Constraints2.IsEmpty() overall.
  {
    auto result = V2CopyFromV1BufferUsage(v1.usage());
    OK_OR_RET_ERROR(result);
    v2b.usage().emplace(result.take_value());
  }

  PROCESS_SCALAR_FIELD_V1(min_buffer_count_for_camping);
  PROCESS_SCALAR_FIELD_V1(min_buffer_count_for_dedicated_slack);
  PROCESS_SCALAR_FIELD_V1(min_buffer_count_for_shared_slack);
  PROCESS_SCALAR_FIELD_V1(min_buffer_count);
  PROCESS_SCALAR_FIELD_V1(max_buffer_count);
  if (v1.has_buffer_memory_constraints()) {
    auto result = V2CopyFromV1BufferMemoryConstraints(v1.buffer_memory_constraints());
    OK_OR_RET_ERROR(result);
    v2b.buffer_memory_constraints().emplace(result.take_value());
  }
  if (v1.image_format_constraints_count()) {
    auto result = V2CopyFromV1ImageFormatConstraintsArrayNatural(
        v1.image_format_constraints(), v1.image_format_constraints_count());
    OK_OR_RET_ERROR(result);
    v2b.image_format_constraints().emplace(result.take_value());
  }
  return fpromise::ok();
}

fpromise::result<> V2CopyFromV1BufferCollectionConstraintsAuxBuffers(
    fuchsia_sysmem2::BufferCollectionConstraints* v2b_param,
    const fuchsia_sysmem::BufferCollectionConstraintsAuxBuffers& v1) {
  ZX_DEBUG_ASSERT(v2b_param);
  fuchsia_sysmem2::BufferCollectionConstraints& v2b = *v2b_param;
  PROCESS_SCALAR_FIELD_V1(need_clear_aux_buffers_for_secure);
  PROCESS_SCALAR_FIELD_V1(allow_clear_aux_buffers_for_secure);
  return fpromise::ok();
}

fpromise::result<> V2CopyFromV1BufferCollectionConstraintsMain(
    fidl::AnyArena& allocator, fuchsia_sysmem2::wire::BufferCollectionConstraints* v2b_param,
    const fuchsia_sysmem::wire::BufferCollectionConstraints& v1) {
  ZX_DEBUG_ASSERT(v2b_param);
  fuchsia_sysmem2::wire::BufferCollectionConstraints& v2b = *v2b_param;

  // This sets usage regardless of whether the client set any usage bits within usage.  That's
  // checked later (regardless of v1 or v2 client).  If a v1 client said !has_constraints, we
  // won't call the current method and usage field will remain un-set so that
  // Constraints2.IsEmpty() overall.
  {
    auto result = V2CopyFromV1BufferUsage(allocator, v1.usage);
    OK_OR_RET_ERROR(result);
    v2b.set_usage(allocator, result.take_value());
  }

  PROCESS_WIRE_SCALAR_FIELD_V1(min_buffer_count_for_camping);
  PROCESS_WIRE_SCALAR_FIELD_V1(min_buffer_count_for_dedicated_slack);
  PROCESS_WIRE_SCALAR_FIELD_V1(min_buffer_count_for_shared_slack);
  PROCESS_WIRE_SCALAR_FIELD_V1(min_buffer_count);
  PROCESS_WIRE_SCALAR_FIELD_V1(max_buffer_count);
  if (v1.has_buffer_memory_constraints) {
    auto result = V2CopyFromV1BufferMemoryConstraints(allocator, v1.buffer_memory_constraints);
    OK_OR_RET_ERROR(result);
    v2b.set_buffer_memory_constraints(allocator, result.take_value());
  }
  if (v1.image_format_constraints_count) {
    auto result = V2CopyFromV1ImageFormatConstraintsArray(allocator, v1.image_format_constraints,
                                                          v1.image_format_constraints_count);
    OK_OR_RET_ERROR(result);
    v2b.set_image_format_constraints(allocator, result.take_value());
  }
  return fpromise::ok();
}

fpromise::result<> V2CopyFromV1BufferCollectionConstraintsAuxBuffers(
    fidl::AnyArena& allocator, fuchsia_sysmem2::wire::BufferCollectionConstraints* v2b_param,
    const fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers& v1) {
  ZX_DEBUG_ASSERT(v2b_param);
  fuchsia_sysmem2::wire::BufferCollectionConstraints& v2b = *v2b_param;
  PROCESS_WIRE_SCALAR_FIELD_V1(need_clear_aux_buffers_for_secure);
  PROCESS_WIRE_SCALAR_FIELD_V1(allow_clear_aux_buffers_for_secure);
  return fpromise::ok();
}

}  // namespace

fuchsia_sysmem2::PixelFormat V2CopyFromV1PixelFormat(const fuchsia_sysmem::PixelFormat& v1) {
  fuchsia_sysmem2::PixelFormat v2b;
  PROCESS_SCALAR_FIELD_V1(type);
  if (v1.has_format_modifier()) {
    v2b.format_modifier_value().emplace(v1.format_modifier().value());
  }
  return v2b;
}

fuchsia_sysmem2::wire::PixelFormat V2CopyFromV1PixelFormat(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::PixelFormat& v1) {
  fuchsia_sysmem2::wire::PixelFormat v2b(allocator);
  PROCESS_WIRE_SCALAR_FIELD_V1(type);
  if (v1.has_format_modifier) {
    v2b.set_format_modifier_value(allocator, v1.format_modifier.value);
  }
  return v2b;
}

fuchsia_sysmem2::ColorSpace V2CopyFromV1ColorSpace(const fuchsia_sysmem::ColorSpace& v1) {
  fuchsia_sysmem2::ColorSpace v2b;
  PROCESS_SCALAR_FIELD_V1(type);
  return v2b;
}

fuchsia_sysmem2::wire::ColorSpace V2CopyFromV1ColorSpace(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::ColorSpace& v1) {
  fuchsia_sysmem2::wire::ColorSpace v2b(allocator);
  PROCESS_WIRE_SCALAR_FIELD_V1(type);
  return v2b;
}

fpromise::result<fuchsia_sysmem2::ImageFormatConstraints> V2CopyFromV1ImageFormatConstraints(
    const fuchsia_sysmem::ImageFormatConstraints& v1) {
  fuchsia_sysmem2::ImageFormatConstraints v2b;
  v2b.pixel_format().emplace(V2CopyFromV1PixelFormat(v1.pixel_format()));
  if (v1.color_spaces_count()) {
    auto result = V2CopyFromV1ColorSpaceArrayNatural(v1.color_space(), v1.color_spaces_count());
    OK_OR_RET_ERROR(result);
    v2b.color_spaces().emplace(result.take_value());
  }
  PROCESS_SCALAR_FIELD_V1(min_coded_width);
  PROCESS_SCALAR_FIELD_V1(max_coded_width);
  PROCESS_SCALAR_FIELD_V1(min_coded_height);
  PROCESS_SCALAR_FIELD_V1(max_coded_height);
  PROCESS_SCALAR_FIELD_V1(min_bytes_per_row);
  PROCESS_SCALAR_FIELD_V1(max_bytes_per_row);
  PROCESS_SCALAR_FIELD_V1(max_coded_width_times_coded_height);
  if (v1.layers()) {
    if (v1.layers() > 1) {
      LOG(ERROR, "v1.layers > 1");
      return fpromise::error();
    }
    // v2 ImageFormatConstraints doesn't have layers field (at least not yet), on purpose.  If it
    // ever gains a layers field, most likely we won't translate the v1 layers field to any v2
    // layers field.
  }
  PROCESS_SCALAR_FIELD_V1(coded_width_divisor);
  PROCESS_SCALAR_FIELD_V1(coded_height_divisor);
  PROCESS_SCALAR_FIELD_V1(bytes_per_row_divisor);
  PROCESS_SCALAR_FIELD_V1(start_offset_divisor);
  PROCESS_SCALAR_FIELD_V1(display_width_divisor);
  PROCESS_SCALAR_FIELD_V1(display_height_divisor);
  PROCESS_SCALAR_FIELD_V1(required_min_coded_width);
  PROCESS_SCALAR_FIELD_V1(required_max_coded_width);
  PROCESS_SCALAR_FIELD_V1(required_min_coded_height);
  PROCESS_SCALAR_FIELD_V1(required_max_coded_height);
  PROCESS_SCALAR_FIELD_V1(required_min_bytes_per_row);
  PROCESS_SCALAR_FIELD_V1(required_max_bytes_per_row);
  return fpromise::ok(std::move(v2b));
}

fpromise::result<fuchsia_sysmem2::wire::ImageFormatConstraints> V2CopyFromV1ImageFormatConstraints(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::ImageFormatConstraints& v1) {
  fuchsia_sysmem2::wire::ImageFormatConstraints v2b(allocator);
  v2b.set_pixel_format(allocator, V2CopyFromV1PixelFormat(allocator, v1.pixel_format));
  if (v1.color_spaces_count) {
    auto result = V2CopyFromV1ColorSpaceArray(allocator, v1.color_space, v1.color_spaces_count);
    OK_OR_RET_ERROR(result);
    v2b.set_color_spaces(allocator, result.take_value());
  }
  PROCESS_WIRE_SCALAR_FIELD_V1(min_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V1(max_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V1(min_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V1(max_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V1(min_bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V1(max_bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V1(max_coded_width_times_coded_height);
  if (v1.layers) {
    if (v1.layers > 1) {
      LOG(ERROR, "v1.layers > 1");
      return fpromise::error();
    }
    // v2 ImageFormatConstraints doesn't have layers field (at least not yet), on purpose.  If it
    // ever gains a layers field, most likely we won't translate the v1 layers field to any v2
    // layers field.
  }
  PROCESS_WIRE_SCALAR_FIELD_V1(coded_width_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V1(coded_height_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V1(bytes_per_row_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V1(start_offset_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V1(display_width_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V1(display_height_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V1(required_min_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V1(required_max_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V1(required_min_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V1(required_max_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V1(required_min_bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V1(required_max_bytes_per_row);
  return fpromise::ok(v2b);
}

fpromise::result<fuchsia_sysmem2::BufferUsage> V2CopyFromV1BufferUsage(
    const fuchsia_sysmem::BufferUsage& v1) {
  fuchsia_sysmem2::BufferUsage v2b;
  PROCESS_SCALAR_FIELD_V1(none);
  PROCESS_SCALAR_FIELD_V1(cpu);
  PROCESS_SCALAR_FIELD_V1(vulkan);
  PROCESS_SCALAR_FIELD_V1(display);
  PROCESS_SCALAR_FIELD_V1(video);
  return fpromise::ok(std::move(v2b));
}

fpromise::result<fuchsia_sysmem2::wire::BufferUsage> V2CopyFromV1BufferUsage(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::BufferUsage& v1) {
  fuchsia_sysmem2::wire::BufferUsage v2b(allocator);
  using foo = std::remove_reference_t<decltype((v1.none))>;
  static_assert(std::is_const<foo>::value);
  PROCESS_WIRE_SCALAR_FIELD_V1(none);
  PROCESS_WIRE_SCALAR_FIELD_V1(cpu);
  PROCESS_WIRE_SCALAR_FIELD_V1(vulkan);
  PROCESS_WIRE_SCALAR_FIELD_V1(display);
  PROCESS_WIRE_SCALAR_FIELD_V1(video);
  return fpromise::ok(v2b);
}

fpromise::result<fuchsia_sysmem2::BufferMemoryConstraints> V2CopyFromV1BufferMemoryConstraints(
    const fuchsia_sysmem::BufferMemoryConstraints& v1) {
  fuchsia_sysmem2::BufferMemoryConstraints v2b;
  PROCESS_SCALAR_FIELD_V1(min_size_bytes);
  PROCESS_SCALAR_FIELD_V1(max_size_bytes);
  PROCESS_SCALAR_FIELD_V1(physically_contiguous_required);
  PROCESS_SCALAR_FIELD_V1(secure_required);
  PROCESS_SCALAR_FIELD_V1(ram_domain_supported);
  PROCESS_SCALAR_FIELD_V1(cpu_domain_supported);
  PROCESS_SCALAR_FIELD_V1(inaccessible_domain_supported);
  if (v1.heap_permitted_count()) {
    auto result =
        V2CopyFromV1HeapPermittedArrayNatural(v1.heap_permitted(), v1.heap_permitted_count());
    OK_OR_RET_ERROR(result);
    v2b.heap_permitted().emplace(result.take_value());
  }
  return fpromise::ok(std::move(v2b));
}

fpromise::result<fuchsia_sysmem2::wire::BufferMemoryConstraints>
V2CopyFromV1BufferMemoryConstraints(fidl::AnyArena& allocator,
                                    const fuchsia_sysmem::wire::BufferMemoryConstraints& v1) {
  fuchsia_sysmem2::wire::BufferMemoryConstraints v2b(allocator);
  PROCESS_WIRE_SCALAR_FIELD_V1(min_size_bytes);
  PROCESS_WIRE_SCALAR_FIELD_V1(max_size_bytes);
  PROCESS_WIRE_SCALAR_FIELD_V1(physically_contiguous_required);
  PROCESS_WIRE_SCALAR_FIELD_V1(secure_required);
  PROCESS_WIRE_SCALAR_FIELD_V1(ram_domain_supported);
  PROCESS_WIRE_SCALAR_FIELD_V1(cpu_domain_supported);
  PROCESS_WIRE_SCALAR_FIELD_V1(inaccessible_domain_supported);
  if (v1.heap_permitted_count) {
    auto result =
        V2CopyFromV1HeapPermittedArray(allocator, v1.heap_permitted, v1.heap_permitted_count);
    OK_OR_RET_ERROR(result);
    v2b.set_heap_permitted(allocator, result.take_value());
  }
  return fpromise::ok(v2b);
}

// If !v1 && !aux_buffers_v1, the result will be fit::is_ok(), but result.value().IsEmpty().
fpromise::result<fuchsia_sysmem2::BufferCollectionConstraints>
V2CopyFromV1BufferCollectionConstraints(
    const fuchsia_sysmem::BufferCollectionConstraints* v1,
    const fuchsia_sysmem::BufferCollectionConstraintsAuxBuffers* aux_buffers_v1) {
  // Should be enforced by the caller.
  ZX_DEBUG_ASSERT(v1 || !aux_buffers_v1);
  fuchsia_sysmem2::BufferCollectionConstraints v2b;

  if (v1) {
    auto result = V2CopyFromV1BufferCollectionConstraintsMain(&v2b, *v1);
    OK_OR_RET_ERROR(result);
  }

  if (aux_buffers_v1) {
    auto result = V2CopyFromV1BufferCollectionConstraintsAuxBuffers(&v2b, *aux_buffers_v1);
    OK_OR_RET_ERROR(result);
  }

  return fpromise::ok(std::move(v2b));
}

// If !v1 && !aux_buffers_v1, the result will be fit::is_ok(), but result.value().IsEmpty().
fpromise::result<fuchsia_sysmem2::wire::BufferCollectionConstraints>
V2CopyFromV1BufferCollectionConstraints(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::BufferCollectionConstraints* v1,
    const fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers* aux_buffers_v1) {
  // Should be enforced by the caller.
  ZX_DEBUG_ASSERT(v1 || !aux_buffers_v1);
  fuchsia_sysmem2::wire::BufferCollectionConstraints v2b(allocator);

  if (v1) {
    auto result = V2CopyFromV1BufferCollectionConstraintsMain(allocator, &v2b, *v1);
    OK_OR_RET_ERROR(result);
  }

  if (aux_buffers_v1) {
    auto result =
        V2CopyFromV1BufferCollectionConstraintsAuxBuffers(allocator, &v2b, *aux_buffers_v1);
    OK_OR_RET_ERROR(result);
  }

  return fpromise::ok(v2b);
}

fpromise::result<fuchsia_sysmem2::ImageFormat> V2CopyFromV1ImageFormat(
    const fuchsia_sysmem::ImageFormat2& v1) {
  fuchsia_sysmem2::ImageFormat v2b;
  v2b.pixel_format().emplace(V2CopyFromV1PixelFormat(v1.pixel_format()));
  PROCESS_SCALAR_FIELD_V1(coded_width);
  PROCESS_SCALAR_FIELD_V1(coded_height);
  PROCESS_SCALAR_FIELD_V1(bytes_per_row);
  PROCESS_SCALAR_FIELD_V1(display_width);
  PROCESS_SCALAR_FIELD_V1(display_height);
  if (v1.layers() > 1) {
    LOG(ERROR, "v1.layers > 1");
    return fpromise::error();
  }
  v2b.color_space().emplace(V2CopyFromV1ColorSpace(v1.color_space()));
  if (v1.has_pixel_aspect_ratio()) {
    v2b.pixel_aspect_ratio_width().emplace(v1.pixel_aspect_ratio_width());
    v2b.pixel_aspect_ratio_height().emplace(v1.pixel_aspect_ratio_height());
  } else {
    ZX_DEBUG_ASSERT(!v2b.pixel_aspect_ratio_width().has_value());
    ZX_DEBUG_ASSERT(!v2b.pixel_aspect_ratio_height().has_value());
  }
  return fpromise::ok(std::move(v2b));
}

fpromise::result<fuchsia_sysmem2::wire::ImageFormat> V2CopyFromV1ImageFormat(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::ImageFormat2& v1) {
  fuchsia_sysmem2::wire::ImageFormat v2b(allocator);
  v2b.set_pixel_format(allocator, V2CopyFromV1PixelFormat(allocator, v1.pixel_format));
  PROCESS_WIRE_SCALAR_FIELD_V1(coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V1(coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V1(bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V1(display_width);
  PROCESS_WIRE_SCALAR_FIELD_V1(display_height);
  if (v1.layers > 1) {
    LOG(ERROR, "v1.layers > 1");
    return fpromise::error();
  }
  v2b.set_color_space(allocator, V2CopyFromV1ColorSpace(allocator, v1.color_space));
  if (v1.has_pixel_aspect_ratio) {
    v2b.set_pixel_aspect_ratio_width(v1.pixel_aspect_ratio_width);
    v2b.set_pixel_aspect_ratio_height(v1.pixel_aspect_ratio_height);
  } else {
    ZX_DEBUG_ASSERT(!v2b.has_pixel_aspect_ratio_width());
    ZX_DEBUG_ASSERT(!v2b.has_pixel_aspect_ratio_height());
  }
  return fpromise::ok(v2b);
}

fuchsia_sysmem2::BufferMemorySettings V2CopyFromV1BufferMemorySettings(
    const fuchsia_sysmem::BufferMemorySettings& v1) {
  fuchsia_sysmem2::BufferMemorySettings v2b;
  PROCESS_SCALAR_FIELD_V1(size_bytes);
  PROCESS_SCALAR_FIELD_V1(is_physically_contiguous);
  PROCESS_SCALAR_FIELD_V1(is_secure);
  PROCESS_SCALAR_FIELD_V1(coherency_domain);
  PROCESS_SCALAR_FIELD_V1(heap);
  return v2b;
}

fuchsia_sysmem2::wire::BufferMemorySettings V2CopyFromV1BufferMemorySettings(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::BufferMemorySettings& v1) {
  fuchsia_sysmem2::wire::BufferMemorySettings v2b(allocator);
  PROCESS_WIRE_SCALAR_FIELD_V1(size_bytes);
  PROCESS_WIRE_SCALAR_FIELD_V1(is_physically_contiguous);
  PROCESS_WIRE_SCALAR_FIELD_V1(is_secure);
  PROCESS_WIRE_SCALAR_FIELD_V1(coherency_domain);
  PROCESS_WIRE_SCALAR_FIELD_V1_WITH_ALLOCATOR(heap);
  return v2b;
}

fpromise::result<fuchsia_sysmem2::SingleBufferSettings> V2CopyFromV1SingleBufferSettings(
    const fuchsia_sysmem::SingleBufferSettings& v1) {
  fuchsia_sysmem2::SingleBufferSettings v2b;
  v2b.buffer_settings().emplace(V2CopyFromV1BufferMemorySettings(v1.buffer_settings()));
  if (v1.has_image_format_constraints()) {
    auto image_format_constraints_result =
        V2CopyFromV1ImageFormatConstraints(v1.image_format_constraints());
    if (!image_format_constraints_result.is_ok()) {
      LOG(ERROR, "!image_format_constraints_result.is_ok()");
      return fpromise::error();
    }
    v2b.image_format_constraints().emplace(image_format_constraints_result.take_value());
  }
  return fpromise::ok(std::move(v2b));
}

fpromise::result<fuchsia_sysmem2::wire::SingleBufferSettings> V2CopyFromV1SingleBufferSettings(
    fidl::AnyArena& allocator, const fuchsia_sysmem::wire::SingleBufferSettings& v1) {
  fuchsia_sysmem2::wire::SingleBufferSettings v2b(allocator);
  v2b.set_buffer_settings(allocator,
                          V2CopyFromV1BufferMemorySettings(allocator, v1.buffer_settings));
  if (v1.has_image_format_constraints) {
    auto image_format_constraints_result =
        V2CopyFromV1ImageFormatConstraints(allocator, v1.image_format_constraints);
    if (!image_format_constraints_result.is_ok()) {
      LOG(ERROR, "!image_format_constraints_result.is_ok()");
      return fpromise::error();
    }
    v2b.set_image_format_constraints(allocator, image_format_constraints_result.take_value());
  }
  return fpromise::ok(v2b);
}

fuchsia_sysmem2::VmoBuffer V2MoveFromV1VmoBuffer(fuchsia_sysmem::VmoBuffer v1) {
  fuchsia_sysmem2::VmoBuffer v2b;
  if (v1.vmo().is_valid()) {
    v2b.vmo().emplace(std::move(v1.vmo()));
  }
  PROCESS_SCALAR_FIELD_V1(vmo_usable_start);
  ZX_DEBUG_ASSERT(!v2b.aux_vmo().has_value());
  return v2b;
}

fuchsia_sysmem2::wire::VmoBuffer V2MoveFromV1VmoBuffer(fidl::AnyArena& allocator,
                                                       fuchsia_sysmem::wire::VmoBuffer v1) {
  fuchsia_sysmem2::wire::VmoBuffer v2b(allocator);
  if (v1.vmo) {
    v2b.set_vmo(std::move(v1.vmo));
  }
  PROCESS_WIRE_SCALAR_FIELD_V1_WITH_ALLOCATOR(vmo_usable_start);
  ZX_DEBUG_ASSERT(!v2b.has_aux_vmo());
  return v2b;
}

fpromise::result<fuchsia_sysmem2::BufferCollectionInfo> V2MoveFromV1BufferCollectionInfo(
    fuchsia_sysmem::BufferCollectionInfo2 v1) {
  fuchsia_sysmem2::BufferCollectionInfo v2b;
  auto settings_result = V2CopyFromV1SingleBufferSettings(v1.settings());
  if (!settings_result.is_ok()) {
    LOG(ERROR, "!settings_result.is_ok()");
    return fpromise::error();
  }
  v2b.settings().emplace(settings_result.take_value());
  if (v1.buffer_count()) {
    v2b.buffers().emplace(v1.buffer_count());
    for (uint32_t i = 0; i < v1.buffer_count(); ++i) {
      v2b.buffers()->at(i) = V2MoveFromV1VmoBuffer(std::move(v1.buffers()[i]));
    }
  }
  return fpromise::ok(std::move(v2b));
}

fpromise::result<fuchsia_sysmem2::wire::BufferCollectionInfo> V2MoveFromV1BufferCollectionInfo(
    fidl::AnyArena& allocator, fuchsia_sysmem::wire::BufferCollectionInfo2 v1) {
  fuchsia_sysmem2::wire::BufferCollectionInfo v2b(allocator);
  auto settings_result = V2CopyFromV1SingleBufferSettings(allocator, v1.settings);
  if (!settings_result.is_ok()) {
    LOG(ERROR, "!settings_result.is_ok()");
    return fpromise::error();
  }
  v2b.set_settings(allocator, settings_result.take_value());
  if (v1.buffer_count) {
    v2b.set_buffers(allocator, allocator, v1.buffer_count);
    for (uint32_t i = 0; i < v1.buffer_count; ++i) {
      v2b.buffers()[i] = V2MoveFromV1VmoBuffer(allocator, std::move(v1.buffers[i]));
    }
  }
  return fpromise::ok(v2b);
}

fuchsia_sysmem::wire::HeapType V1CopyFromV2HeapType(fuchsia_sysmem2::wire::HeapType heap_type) {
  return static_cast<fuchsia_sysmem::wire::HeapType>(fidl_underlying_cast(heap_type));
}

fuchsia_sysmem2::wire::HeapType V2CopyFromV1HeapType(fuchsia_sysmem::wire::HeapType heap_type) {
  return static_cast<fuchsia_sysmem2::wire::HeapType>(fidl_underlying_cast(heap_type));
}

fpromise::result<std::pair<std::optional<fuchsia_sysmem::BufferCollectionConstraints>,
                           std::optional<fuchsia_sysmem::BufferCollectionConstraintsAuxBuffers>>>
V1CopyFromV2BufferCollectionConstraints(const fuchsia_sysmem2::BufferCollectionConstraints& v2) {
  fuchsia_sysmem::BufferCollectionConstraints v1;
  if (v2.IsEmpty()) {
    return fpromise::ok(
        std::pair<std::optional<fuchsia_sysmem::BufferCollectionConstraints>,
                  std::optional<fuchsia_sysmem::BufferCollectionConstraintsAuxBuffers>>());
  }
  if (v2.usage().has_value()) {
    v1.usage() = V1CopyFromV2BufferUsage(v2.usage().value());
  }
  PROCESS_SCALAR_FIELD_V2(min_buffer_count_for_camping);
  PROCESS_SCALAR_FIELD_V2(min_buffer_count_for_dedicated_slack);
  PROCESS_SCALAR_FIELD_V2(min_buffer_count_for_shared_slack);
  PROCESS_SCALAR_FIELD_V2(min_buffer_count);
  PROCESS_SCALAR_FIELD_V2(max_buffer_count);
  ZX_DEBUG_ASSERT(!v1.has_buffer_memory_constraints());
  if (v2.buffer_memory_constraints().has_value()) {
    v1.has_buffer_memory_constraints() = true;
    auto buffer_memory_constraints_result =
        V1CopyFromV2BufferMemoryConstraints(v2.buffer_memory_constraints().value());
    if (!buffer_memory_constraints_result.is_ok()) {
      LOG(ERROR, "!buffer_memory_constraints_result.is_ok()");
      return fpromise::error();
    }
    v1.buffer_memory_constraints() = buffer_memory_constraints_result.take_value();
  }
  ZX_DEBUG_ASSERT(!v1.image_format_constraints_count());
  if (v2.image_format_constraints().has_value()) {
    if (v2.image_format_constraints()->size() >
        fuchsia_sysmem::wire::kMaxCountBufferCollectionConstraintsImageFormatConstraints) {
      LOG(ERROR,
          "v2 image_format_constraints count > v1 "
          "MAX_COUNT_BUFFER_COLLECTION_CONSTRAINTS_IMAGE_FORMAT_CONSTRAINTS");
      return fpromise::error();
    }
    v1.image_format_constraints_count() =
        static_cast<uint32_t>(v2.image_format_constraints()->size());
    for (uint32_t i = 0; i < v2.image_format_constraints()->size(); ++i) {
      auto image_format_constraints_result =
          V1CopyFromV2ImageFormatConstraints(v2.image_format_constraints()->at(i));
      if (!image_format_constraints_result.is_ok()) {
        LOG(ERROR, "!image_format_constraints_result.is_ok()");
        return fpromise::error();
      }
      v1.image_format_constraints()[i] = image_format_constraints_result.take_value();
    }
  }

  std::optional<fuchsia_sysmem::BufferCollectionConstraintsAuxBuffers> v1_aux_buffers;
  if (v2.need_clear_aux_buffers_for_secure().has_value() ||
      v2.allow_clear_aux_buffers_for_secure().has_value()) {
    fuchsia_sysmem::BufferCollectionConstraintsAuxBuffers v1;
    PROCESS_SCALAR_FIELD_V2(need_clear_aux_buffers_for_secure);
    PROCESS_SCALAR_FIELD_V2(allow_clear_aux_buffers_for_secure);
    v1_aux_buffers.emplace(v1);
  }

  return fpromise::ok(std::make_pair(std::move(v1), std::move(v1_aux_buffers)));
}

fpromise::result<
    std::pair<std::optional<fuchsia_sysmem::wire::BufferCollectionConstraints>,
              std::optional<fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers>>>
V1CopyFromV2BufferCollectionConstraints(
    const fuchsia_sysmem2::wire::BufferCollectionConstraints& v2) {
  fuchsia_sysmem::wire::BufferCollectionConstraints v1{};
  if (v2.IsEmpty()) {
    return fpromise::ok(
        std::pair<std::optional<fuchsia_sysmem::wire::BufferCollectionConstraints>,
                  std::optional<fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers>>());
  }
  if (v2.has_usage()) {
    v1.usage = V1CopyFromV2BufferUsage(v2.usage());
  }
  PROCESS_WIRE_SCALAR_FIELD_V2(min_buffer_count_for_camping);
  PROCESS_WIRE_SCALAR_FIELD_V2(min_buffer_count_for_dedicated_slack);
  PROCESS_WIRE_SCALAR_FIELD_V2(min_buffer_count_for_shared_slack);
  PROCESS_WIRE_SCALAR_FIELD_V2(min_buffer_count);
  PROCESS_WIRE_SCALAR_FIELD_V2(max_buffer_count);
  ZX_DEBUG_ASSERT(!v1.has_buffer_memory_constraints);
  if (v2.has_buffer_memory_constraints()) {
    v1.has_buffer_memory_constraints = true;
    auto buffer_memory_constraints_result =
        V1CopyFromV2BufferMemoryConstraints(v2.buffer_memory_constraints());
    if (!buffer_memory_constraints_result.is_ok()) {
      LOG(ERROR, "!buffer_memory_constraints_result.is_ok()");
      return fpromise::error();
    }
    v1.buffer_memory_constraints = buffer_memory_constraints_result.take_value();
  }
  ZX_DEBUG_ASSERT(!v1.image_format_constraints_count);
  if (v2.has_image_format_constraints()) {
    if (v2.image_format_constraints().count() >
        fuchsia_sysmem::wire::kMaxCountBufferCollectionConstraintsImageFormatConstraints) {
      LOG(ERROR,
          "v2 image_format_constraints count > v1 "
          "MAX_COUNT_BUFFER_COLLECTION_CONSTRAINTS_IMAGE_FORMAT_CONSTRAINTS");
      return fpromise::error();
    }
    v1.image_format_constraints_count =
        static_cast<uint32_t>(v2.image_format_constraints().count());
    for (uint32_t i = 0; i < v2.image_format_constraints().count(); ++i) {
      auto image_format_constraints_result =
          V1CopyFromV2ImageFormatConstraints(v2.image_format_constraints()[i]);
      if (!image_format_constraints_result.is_ok()) {
        LOG(ERROR, "!image_format_constraints_result.is_ok()");
        return fpromise::error();
      }
      v1.image_format_constraints[i] = image_format_constraints_result.take_value();
    }
  }

  std::optional<fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers> v1_aux_buffers;
  if (v2.has_need_clear_aux_buffers_for_secure() || v2.has_allow_clear_aux_buffers_for_secure()) {
    fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers v1{};
    PROCESS_WIRE_SCALAR_FIELD_V2(need_clear_aux_buffers_for_secure);
    PROCESS_WIRE_SCALAR_FIELD_V2(allow_clear_aux_buffers_for_secure);
    v1_aux_buffers.emplace(v1);
  }

  return fpromise::ok(std::make_pair(v1, v1_aux_buffers));
}

fpromise::result<fuchsia_sysmem::BufferMemoryConstraints> V1CopyFromV2BufferMemoryConstraints(
    const fuchsia_sysmem2::BufferMemoryConstraints& v2) {
  fuchsia_sysmem::BufferMemoryConstraints v1;
  PROCESS_SCALAR_FIELD_V2(min_size_bytes);
  PROCESS_SCALAR_FIELD_V2(max_size_bytes);
  PROCESS_SCALAR_FIELD_V2(physically_contiguous_required);
  PROCESS_SCALAR_FIELD_V2(secure_required);
  PROCESS_SCALAR_FIELD_V2(ram_domain_supported);
  PROCESS_SCALAR_FIELD_V2(cpu_domain_supported);
  PROCESS_SCALAR_FIELD_V2(inaccessible_domain_supported);
  ZX_DEBUG_ASSERT(!v1.heap_permitted_count());
  if (v2.heap_permitted().has_value()) {
    if (v2.heap_permitted()->size() >
        fuchsia_sysmem::wire::kMaxCountBufferMemoryConstraintsHeapPermitted) {
      LOG(ERROR, "v2 heap_permitted count > v1 MAX_COUNT_BUFFER_MEMORY_CONSTRAINTS_HEAP_PERMITTED");
      return fpromise::error();
    }
    v1.heap_permitted_count() = static_cast<uint32_t>(v2.heap_permitted()->size());
    for (uint32_t i = 0; i < v2.heap_permitted()->size(); ++i) {
      ASSIGN_SCALAR(v1.heap_permitted()[i], v2.heap_permitted()->at(i));
    }
  }
  return fpromise::ok(std::move(v1));
}

fpromise::result<fuchsia_sysmem::wire::BufferMemoryConstraints> V1CopyFromV2BufferMemoryConstraints(
    const fuchsia_sysmem2::wire::BufferMemoryConstraints& v2) {
  fuchsia_sysmem::wire::BufferMemoryConstraints v1{};
  PROCESS_WIRE_SCALAR_FIELD_V2(min_size_bytes);
  PROCESS_WIRE_SCALAR_FIELD_V2(max_size_bytes);
  PROCESS_WIRE_SCALAR_FIELD_V2(physically_contiguous_required);
  PROCESS_WIRE_SCALAR_FIELD_V2(secure_required);
  PROCESS_WIRE_SCALAR_FIELD_V2(ram_domain_supported);
  PROCESS_WIRE_SCALAR_FIELD_V2(cpu_domain_supported);
  PROCESS_WIRE_SCALAR_FIELD_V2(inaccessible_domain_supported);
  ZX_DEBUG_ASSERT(!v1.heap_permitted_count);
  if (v2.has_heap_permitted()) {
    if (v2.heap_permitted().count() >
        fuchsia_sysmem::wire::kMaxCountBufferMemoryConstraintsHeapPermitted) {
      LOG(ERROR, "v2 heap_permitted count > v1 MAX_COUNT_BUFFER_MEMORY_CONSTRAINTS_HEAP_PERMITTED");
      return fpromise::error();
    }
    v1.heap_permitted_count = static_cast<uint32_t>(v2.heap_permitted().count());
    for (uint32_t i = 0; i < v2.heap_permitted().count(); ++i) {
      ASSIGN_SCALAR(v1.heap_permitted[i], v2.heap_permitted()[i]);
    }
  }
  return fpromise::ok(v1);
}

fuchsia_sysmem::BufferUsage V1CopyFromV2BufferUsage(const fuchsia_sysmem2::BufferUsage& v2) {
  fuchsia_sysmem::BufferUsage v1;
  PROCESS_SCALAR_FIELD_V2(none);
  PROCESS_SCALAR_FIELD_V2(cpu);
  PROCESS_SCALAR_FIELD_V2(vulkan);
  PROCESS_SCALAR_FIELD_V2(display);
  PROCESS_SCALAR_FIELD_V2(video);
  return v1;
}

fuchsia_sysmem::wire::BufferUsage V1CopyFromV2BufferUsage(
    const fuchsia_sysmem2::wire::BufferUsage& v2) {
  fuchsia_sysmem::wire::BufferUsage v1{};
  PROCESS_WIRE_SCALAR_FIELD_V2(none);
  PROCESS_WIRE_SCALAR_FIELD_V2(cpu);
  PROCESS_WIRE_SCALAR_FIELD_V2(vulkan);
  PROCESS_WIRE_SCALAR_FIELD_V2(display);
  PROCESS_WIRE_SCALAR_FIELD_V2(video);
  return v1;
}

// v2 must have all fields set.
fuchsia_sysmem::BufferMemorySettings V1CopyFromV2BufferMemorySettings(
    const fuchsia_sysmem2::BufferMemorySettings& v2) {
  fuchsia_sysmem::BufferMemorySettings v1;
  PROCESS_SCALAR_FIELD_V2(size_bytes);
  PROCESS_SCALAR_FIELD_V2(is_physically_contiguous);
  PROCESS_SCALAR_FIELD_V2(is_secure);
  PROCESS_SCALAR_FIELD_V2(coherency_domain);
  PROCESS_SCALAR_FIELD_V2(heap);
  return v1;
}

// v2 must have all fields set.
fuchsia_sysmem::wire::BufferMemorySettings V1CopyFromV2BufferMemorySettings(
    const fuchsia_sysmem2::wire::BufferMemorySettings& v2) {
  fuchsia_sysmem::wire::BufferMemorySettings v1{};
  PROCESS_WIRE_SCALAR_FIELD_V2(size_bytes);
  PROCESS_WIRE_SCALAR_FIELD_V2(is_physically_contiguous);
  PROCESS_WIRE_SCALAR_FIELD_V2(is_secure);
  PROCESS_WIRE_SCALAR_FIELD_V2(coherency_domain);
  PROCESS_WIRE_SCALAR_FIELD_V2(heap);
  return v1;
}

fuchsia_sysmem::PixelFormat V1CopyFromV2PixelFormat(const fuchsia_sysmem2::PixelFormat& v2) {
  fuchsia_sysmem::PixelFormat v1;
  PROCESS_SCALAR_FIELD_V2(type);
  v1.has_format_modifier() = v2.format_modifier_value().has_value();
  if (v2.format_modifier_value().has_value()) {
    v1.format_modifier().value() = v2.format_modifier_value().value();
  }
  return v1;
}

fuchsia_sysmem::wire::PixelFormat V1CopyFromV2PixelFormat(
    const fuchsia_sysmem2::wire::PixelFormat& v2) {
  fuchsia_sysmem::wire::PixelFormat v1;
  PROCESS_WIRE_SCALAR_FIELD_V2(type);
  v1.has_format_modifier = v2.has_format_modifier_value();
  if (v2.has_format_modifier_value()) {
    v1.format_modifier.value = v2.format_modifier_value();
  }
  return v1;
}

[[nodiscard]] fuchsia_sysmem::ColorSpace V1CopyFromV2ColorSpace(
    const fuchsia_sysmem2::ColorSpace& v2) {
  fuchsia_sysmem::ColorSpace v1;
  PROCESS_SCALAR_FIELD_V2(type);
  return v1;
}

fuchsia_sysmem::wire::ColorSpace V1CopyFromV2ColorSpace(
    const fuchsia_sysmem2::wire::ColorSpace& v2) {
  fuchsia_sysmem::wire::ColorSpace v1{};
  PROCESS_WIRE_SCALAR_FIELD_V2(type);
  return v1;
}

fpromise::result<fuchsia_sysmem::ImageFormatConstraints> V1CopyFromV2ImageFormatConstraints(
    const fuchsia_sysmem2::ImageFormatConstraints& v2) {
  fuchsia_sysmem::ImageFormatConstraints v1;
  v1.pixel_format() = V1CopyFromV2PixelFormat(v2.pixel_format().value());
  ZX_DEBUG_ASSERT(!v1.color_spaces_count());
  if (v2.color_spaces().has_value()) {
    if (v2.color_spaces()->size() >
        fuchsia_sysmem::wire::kMaxCountImageFormatConstraintsColorSpaces) {
      LOG(ERROR,
          "v2.color_spaces().count() > "
          "fuchsia_sysmem::wire::kMaxCountImageFormatConstraintsColorSpaces");
      return fpromise::error();
    }
    v1.color_spaces_count() = static_cast<uint32_t>(v2.color_spaces()->size());
    for (uint32_t i = 0; i < v2.color_spaces()->size(); ++i) {
      v1.color_space()[i] = V1CopyFromV2ColorSpace(v2.color_spaces()->at(i));
    }
  }
  PROCESS_SCALAR_FIELD_V2(min_coded_width);
  PROCESS_SCALAR_FIELD_V2(max_coded_width);
  PROCESS_SCALAR_FIELD_V2(min_coded_height);
  PROCESS_SCALAR_FIELD_V2(max_coded_height);
  PROCESS_SCALAR_FIELD_V2(min_bytes_per_row);
  PROCESS_SCALAR_FIELD_V2(max_bytes_per_row);
  PROCESS_SCALAR_FIELD_V2(max_coded_width_times_coded_height);
  v1.layers() = 1;
  PROCESS_SCALAR_FIELD_V2(coded_width_divisor);
  PROCESS_SCALAR_FIELD_V2(coded_height_divisor);
  PROCESS_SCALAR_FIELD_V2(bytes_per_row_divisor);
  PROCESS_SCALAR_FIELD_V2(start_offset_divisor);
  PROCESS_SCALAR_FIELD_V2(display_width_divisor);
  PROCESS_SCALAR_FIELD_V2(display_height_divisor);
  PROCESS_SCALAR_FIELD_V2(required_min_coded_width);
  PROCESS_SCALAR_FIELD_V2(required_max_coded_width);
  PROCESS_SCALAR_FIELD_V2(required_min_coded_height);
  PROCESS_SCALAR_FIELD_V2(required_max_coded_height);
  PROCESS_SCALAR_FIELD_V2(required_min_bytes_per_row);
  PROCESS_SCALAR_FIELD_V2(required_max_bytes_per_row);
  return fpromise::ok(std::move(v1));
}

fpromise::result<fuchsia_sysmem::wire::ImageFormatConstraints> V1CopyFromV2ImageFormatConstraints(
    const fuchsia_sysmem2::wire::ImageFormatConstraints& v2) {
  fuchsia_sysmem::wire::ImageFormatConstraints v1{};
  v1.pixel_format = V1CopyFromV2PixelFormat(v2.pixel_format());
  ZX_DEBUG_ASSERT(!v1.color_spaces_count);
  if (v2.has_color_spaces()) {
    if (v2.color_spaces().count() >
        fuchsia_sysmem::wire::kMaxCountImageFormatConstraintsColorSpaces) {
      LOG(ERROR,
          "v2.color_spaces().count() > "
          "fuchsia_sysmem::wire::kMaxCountImageFormatConstraintsColorSpaces");
      return fpromise::error();
    }
    v1.color_spaces_count = static_cast<uint32_t>(v2.color_spaces().count());
    for (uint32_t i = 0; i < v2.color_spaces().count(); ++i) {
      v1.color_space[i] = V1CopyFromV2ColorSpace(v2.color_spaces()[i]);
    }
  }
  PROCESS_WIRE_SCALAR_FIELD_V2(min_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V2(max_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V2(min_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V2(max_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V2(min_bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V2(max_bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V2(max_coded_width_times_coded_height);
  v1.layers = 1;
  PROCESS_WIRE_SCALAR_FIELD_V2(coded_width_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V2(coded_height_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V2(bytes_per_row_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V2(start_offset_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V2(display_width_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V2(display_height_divisor);
  PROCESS_WIRE_SCALAR_FIELD_V2(required_min_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V2(required_max_coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V2(required_min_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V2(required_max_coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V2(required_min_bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V2(required_max_bytes_per_row);
  return fpromise::ok(v1);
}

[[nodiscard]] fpromise::result<fuchsia_sysmem::ImageFormat2> V1CopyFromV2ImageFormat(
    fuchsia_sysmem2::ImageFormat& v2) {
  fuchsia_sysmem::ImageFormat2 v1;
  if (v2.pixel_format().has_value()) {
    v1.pixel_format() = V1CopyFromV2PixelFormat(v2.pixel_format().value());
  }
  PROCESS_SCALAR_FIELD_V2(coded_width);
  PROCESS_SCALAR_FIELD_V2(coded_height);
  PROCESS_SCALAR_FIELD_V2(bytes_per_row);
  PROCESS_SCALAR_FIELD_V2(display_width);
  PROCESS_SCALAR_FIELD_V2(display_height);
  v1.layers() = 1;
  if (v2.color_space().has_value()) {
    v1.color_space() = V1CopyFromV2ColorSpace(v2.color_space().value());
  }
  v1.has_pixel_aspect_ratio() =
      v2.pixel_aspect_ratio_width().has_value() && v2.pixel_aspect_ratio_height().has_value();
  if (v1.has_pixel_aspect_ratio()) {
    v1.pixel_aspect_ratio_width() = v2.pixel_aspect_ratio_width().value();
    v1.pixel_aspect_ratio_height() = v2.pixel_aspect_ratio_height().value();
  }
  return fpromise::ok(std::move(v1));
}

fpromise::result<fuchsia_sysmem::wire::ImageFormat2> V1CopyFromV2ImageFormat(
    fuchsia_sysmem2::wire::ImageFormat& v2) {
  fuchsia_sysmem::wire::ImageFormat2 v1{};
  if (v2.has_pixel_format()) {
    v1.pixel_format = V1CopyFromV2PixelFormat(v2.pixel_format());
  }
  PROCESS_WIRE_SCALAR_FIELD_V2(coded_width);
  PROCESS_WIRE_SCALAR_FIELD_V2(coded_height);
  PROCESS_WIRE_SCALAR_FIELD_V2(bytes_per_row);
  PROCESS_WIRE_SCALAR_FIELD_V2(display_width);
  PROCESS_WIRE_SCALAR_FIELD_V2(display_height);
  v1.layers = 1;
  if (v2.has_color_space()) {
    v1.color_space = V1CopyFromV2ColorSpace(v2.color_space());
  }
  v1.has_pixel_aspect_ratio =
      v2.has_pixel_aspect_ratio_width() && v2.has_pixel_aspect_ratio_height();
  if (v1.has_pixel_aspect_ratio) {
    v1.pixel_aspect_ratio_width = v2.pixel_aspect_ratio_width();
    v1.pixel_aspect_ratio_height = v2.pixel_aspect_ratio_height();
  }
  return fpromise::ok(v1);
}

fpromise::result<fuchsia_sysmem::SingleBufferSettings> V1CopyFromV2SingleBufferSettings(
    const fuchsia_sysmem2::SingleBufferSettings& v2) {
  fuchsia_sysmem::SingleBufferSettings v1;
  v1.buffer_settings() = V1CopyFromV2BufferMemorySettings(v2.buffer_settings().value());
  v1.has_image_format_constraints() = v2.image_format_constraints().has_value();
  if (v2.image_format_constraints().has_value()) {
    auto image_format_constraints_result =
        V1CopyFromV2ImageFormatConstraints(v2.image_format_constraints().value());
    if (!image_format_constraints_result.is_ok()) {
      LOG(ERROR, "!image_format_constraints_result.is_ok()");
      return fpromise::error();
    }
    v1.image_format_constraints() = image_format_constraints_result.take_value();
  }
  return fpromise::ok(std::move(v1));
}

fpromise::result<fuchsia_sysmem::wire::SingleBufferSettings> V1CopyFromV2SingleBufferSettings(
    const fuchsia_sysmem2::wire::SingleBufferSettings& v2) {
  fuchsia_sysmem::wire::SingleBufferSettings v1{};
  v1.buffer_settings = V1CopyFromV2BufferMemorySettings(v2.buffer_settings());
  v1.has_image_format_constraints = v2.has_image_format_constraints();
  if (v2.has_image_format_constraints()) {
    auto image_format_constraints_result =
        V1CopyFromV2ImageFormatConstraints(v2.image_format_constraints());
    if (!image_format_constraints_result.is_ok()) {
      LOG(ERROR, "!image_format_constraints_result.is_ok()");
      return fpromise::error();
    }
    v1.image_format_constraints = image_format_constraints_result.take_value();
  }
  return fpromise::ok(v1);
}

// Intentionally just consumes aux_vmo.  The implied extra handle duplications from this behavior go
// away when all participants speak V2.  For V1 participants that need the aux_vmo, there's a
// completely different method and struct for getting that.
fuchsia_sysmem::VmoBuffer V1MoveFromV2VmoBuffer(fuchsia_sysmem2::VmoBuffer v2) {
  fuchsia_sysmem::VmoBuffer v1;
  if (v2.vmo().has_value()) {
    v1.vmo() = std::move(v2.vmo().value());
  }
  PROCESS_SCALAR_FIELD_V2(vmo_usable_start);
  // ~v2 will ~aux_vmo, intentionally - see function comment above.
  return v1;
}

// Intentionally just consumes aux_vmo.  The implied extra handle duplications from this behavior go
// away when all participants speak V2.  For V1 participants that need the aux_vmo, there's a
// completely different method and struct for getting that.
fuchsia_sysmem::wire::VmoBuffer V1MoveFromV2VmoBuffer(fuchsia_sysmem2::wire::VmoBuffer v2) {
  fuchsia_sysmem::wire::VmoBuffer v1;
  if (v2.has_vmo()) {
    v1.vmo = std::move(v2.vmo());
  }
  PROCESS_WIRE_SCALAR_FIELD_V2(vmo_usable_start);
  // ~v2 will ~aux_vmo, intentionally - see function comment above.
  return v1;
}

// Intentionally just consumes vmo (but not aux_vmo).  The implied extra handle duplications from
// this behavior go away when all participants speak V2.
fuchsia_sysmem::VmoBuffer V1AuxBuffersMoveFromV2VmoBuffer(fuchsia_sysmem2::VmoBuffer v2) {
  fuchsia_sysmem::VmoBuffer v1;
  if (v2.aux_vmo().has_value()) {
    v1.vmo() = std::move(v2.aux_vmo().value());
  }
  PROCESS_SCALAR_FIELD_V2(vmo_usable_start);
  // ~v2 will ~vmo, intentionally - see function comment above.
  return v1;
}

// Intentionally just consumes vmo (but not aux_vmo).  The implied extra handle duplications from
// this behavior go away when all participants speak V2.
fuchsia_sysmem::wire::VmoBuffer V1AuxBuffersMoveFromV2VmoBuffer(
    fuchsia_sysmem2::wire::VmoBuffer v2) {
  fuchsia_sysmem::wire::VmoBuffer v1;
  if (v2.has_aux_vmo()) {
    v1.vmo = std::move(v2.aux_vmo());
  }
  PROCESS_WIRE_SCALAR_FIELD_V2(vmo_usable_start);
  // ~v2 will ~vmo, intentionally - see function comment above.
  return v1;
}

fpromise::result<fuchsia_sysmem::BufferCollectionInfo2> V1MoveFromV2BufferCollectionInfo(
    fuchsia_sysmem2::BufferCollectionInfo v2) {
  fuchsia_sysmem::BufferCollectionInfo2 v1;
  if (v2.buffers().has_value()) {
    if (v2.buffers()->size() > fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers) {
      LOG(ERROR,
          "v2.buffers().count() > "
          "fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers");
      return fpromise::error();
    }
    v1.buffer_count() = static_cast<uint32_t>(v2.buffers()->size());
    for (uint32_t i = 0; i < v2.buffers()->size(); ++i) {
      v1.buffers()[i] = V1MoveFromV2VmoBuffer(std::move(v2.buffers()->at(i)));
    }
  }
  auto settings_result = V1CopyFromV2SingleBufferSettings(v2.settings().value());
  if (!settings_result.is_ok()) {
    LOG(ERROR, "!settings_result.is_ok()");
    return fpromise::error();
  }
  v1.settings() = settings_result.take_value();
  return fpromise::ok(std::move(v1));
}

fpromise::result<fuchsia_sysmem::wire::BufferCollectionInfo2> V1MoveFromV2BufferCollectionInfo(
    fuchsia_sysmem2::wire::BufferCollectionInfo v2) {
  fuchsia_sysmem::wire::BufferCollectionInfo2 v1;
  if (v2.has_buffers()) {
    if (v2.buffers().count() > fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers) {
      LOG(ERROR,
          "v2.buffers().count() > "
          "fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers");
      return fpromise::error();
    }
    v1.buffer_count = static_cast<uint32_t>(v2.buffers().count());
    for (uint32_t i = 0; i < v2.buffers().count(); ++i) {
      v1.buffers[i] = V1MoveFromV2VmoBuffer(v2.buffers()[i]);
    }
  }
  auto settings_result = V1CopyFromV2SingleBufferSettings(v2.settings());
  if (!settings_result.is_ok()) {
    LOG(ERROR, "!settings_result.is_ok()");
    return fpromise::error();
  }
  v1.settings = settings_result.take_value();
  return fpromise::ok(std::move(v1));
}

fpromise::result<fuchsia_sysmem::BufferCollectionInfo2> V1AuxBuffersMoveFromV2BufferCollectionInfo(
    fuchsia_sysmem2::BufferCollectionInfo v2) {
  fuchsia_sysmem::BufferCollectionInfo2 v1;
  if (v2.buffers().has_value()) {
    if (v2.buffers()->size() > fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers) {
      LOG(ERROR,
          "v2.buffers().count() > "
          "fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers");
      return fpromise::error();
    }
    v1.buffer_count() = static_cast<uint32_t>(v2.buffers()->size());
    for (uint32_t i = 0; i < v2.buffers()->size(); ++i) {
      v1.buffers()[i] = V1AuxBuffersMoveFromV2VmoBuffer(std::move(v2.buffers()->at(i)));
    }
  }
  auto settings_result = V1CopyFromV2SingleBufferSettings(v2.settings().value());
  if (!settings_result.is_ok()) {
    LOG(ERROR, "!settings_result.is_ok()");
    return fpromise::error();
  }
  v1.settings() = settings_result.take_value();
  return fpromise::ok(std::move(v1));
}

fpromise::result<fuchsia_sysmem::wire::BufferCollectionInfo2>
V1AuxBuffersMoveFromV2BufferCollectionInfo(fuchsia_sysmem2::wire::BufferCollectionInfo v2) {
  fuchsia_sysmem::wire::BufferCollectionInfo2 v1;
  if (v2.has_buffers()) {
    if (v2.buffers().count() > fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers) {
      LOG(ERROR,
          "v2.buffers().count() > "
          "fuchsia_sysmem::wire::kMaxCountBufferCollectionInfoBuffers");
      return fpromise::error();
    }
    v1.buffer_count = static_cast<uint32_t>(v2.buffers().count());
    for (uint32_t i = 0; i < v2.buffers().count(); ++i) {
      v1.buffers[i] = V1AuxBuffersMoveFromV2VmoBuffer(v2.buffers()[i]);
    }
  }
  auto settings_result = V1CopyFromV2SingleBufferSettings(v2.settings());
  if (!settings_result.is_ok()) {
    LOG(ERROR, "!settings_result.is_ok()");
    return fpromise::error();
  }
  v1.settings = settings_result.take_value();
  return fpromise::ok(std::move(v1));
}

fuchsia_sysmem2::wire::PixelFormat V2ClonePixelFormat(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::PixelFormat& src) {
  fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
  if (src.has_type()) {
    pixel_format.set_type(src.type());
  }
  if (src.has_format_modifier_value()) {
    pixel_format.set_format_modifier_value(allocator, src.format_modifier_value());
  }
  return pixel_format;
}

fuchsia_sysmem2::wire::ColorSpace V2CloneColorSpace(fidl::AnyArena& allocator,
                                                    const fuchsia_sysmem2::wire::ColorSpace& src) {
  fuchsia_sysmem2::wire::ColorSpace color_space(allocator);
  if (src.has_type()) {
    color_space.set_type(src.type());
  }
  return color_space;
}

fuchsia_sysmem2::wire::BufferMemorySettings V2CloneBufferMemorySettings(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::BufferMemorySettings& src) {
  fuchsia_sysmem2::wire::BufferMemorySettings buffer_memory_settings(allocator);
  if (src.has_size_bytes()) {
    buffer_memory_settings.set_size_bytes(src.size_bytes());
  }
  if (src.has_is_physically_contiguous()) {
    buffer_memory_settings.set_is_physically_contiguous(src.is_physically_contiguous());
  }
  if (src.has_is_secure()) {
    buffer_memory_settings.set_is_secure(src.is_secure());
  }
  if (src.has_coherency_domain()) {
    buffer_memory_settings.set_coherency_domain(src.coherency_domain());
  }
  if (src.has_heap()) {
    buffer_memory_settings.set_heap(allocator, src.heap());
  }
  return buffer_memory_settings;
}

fuchsia_sysmem2::wire::ImageFormatConstraints V2CloneImageFormatConstraints(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::ImageFormatConstraints& src) {
  fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
  if (src.has_pixel_format()) {
    image_format_constraints.set_pixel_format(allocator,
                                              V2ClonePixelFormat(allocator, src.pixel_format()));
  }
  if (src.has_color_spaces()) {
    image_format_constraints.set_color_spaces(allocator, allocator, src.color_spaces().count());
    for (uint32_t i = 0; i < src.color_spaces().count(); ++i) {
      image_format_constraints.color_spaces()[i] =
          V2CloneColorSpace(allocator, src.color_spaces()[i]);
    }
  }
  if (src.has_min_coded_width()) {
    image_format_constraints.set_min_coded_width(src.min_coded_width());
  }
  if (src.has_max_coded_width()) {
    image_format_constraints.set_max_coded_width(src.max_coded_width());
  }
  if (src.has_min_coded_height()) {
    image_format_constraints.set_min_coded_height(src.min_coded_height());
  }
  if (src.has_max_coded_height()) {
    image_format_constraints.set_max_coded_height(src.max_coded_height());
  }
  if (src.has_min_bytes_per_row()) {
    image_format_constraints.set_min_bytes_per_row(src.min_bytes_per_row());
  }
  if (src.has_max_bytes_per_row()) {
    image_format_constraints.set_max_bytes_per_row(src.max_bytes_per_row());
  }
  if (src.has_max_coded_width_times_coded_height()) {
    image_format_constraints.set_max_coded_width_times_coded_height(
        src.max_coded_width_times_coded_height());
  }
  if (src.has_coded_width_divisor()) {
    image_format_constraints.set_coded_width_divisor(src.coded_width_divisor());
  }
  if (src.has_coded_height_divisor()) {
    image_format_constraints.set_coded_height_divisor(src.coded_height_divisor());
  }
  if (src.has_bytes_per_row_divisor()) {
    image_format_constraints.set_bytes_per_row_divisor(src.bytes_per_row_divisor());
  }
  if (src.has_start_offset_divisor()) {
    image_format_constraints.set_start_offset_divisor(src.start_offset_divisor());
  }
  if (src.has_display_width_divisor()) {
    image_format_constraints.set_display_width_divisor(src.display_width_divisor());
  }
  if (src.has_display_height_divisor()) {
    image_format_constraints.set_display_height_divisor(src.display_height_divisor());
  }
  if (src.has_required_min_coded_width()) {
    image_format_constraints.set_required_min_coded_width(src.required_min_coded_width());
  }
  if (src.has_required_max_coded_width()) {
    image_format_constraints.set_required_max_coded_width(src.required_max_coded_width());
  }
  if (src.has_required_min_coded_height()) {
    image_format_constraints.set_required_min_coded_height(src.required_min_coded_height());
  }
  if (src.has_required_max_coded_height()) {
    image_format_constraints.set_required_max_coded_height(src.required_max_coded_height());
  }
  if (src.has_required_min_bytes_per_row()) {
    image_format_constraints.set_required_min_bytes_per_row(src.required_min_bytes_per_row());
  }
  if (src.has_required_max_bytes_per_row()) {
    image_format_constraints.set_required_max_bytes_per_row(src.required_max_bytes_per_row());
  }
  return image_format_constraints;
}

fuchsia_sysmem2::wire::SingleBufferSettings V2CloneSingleBufferSettings(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::SingleBufferSettings& src) {
  fuchsia_sysmem2::wire::SingleBufferSettings single_buffer_settings(allocator);
  if (src.has_buffer_settings()) {
    single_buffer_settings.set_buffer_settings(
        allocator, V2CloneBufferMemorySettings(allocator, src.buffer_settings()));
  }
  if (src.has_image_format_constraints()) {
    single_buffer_settings.set_image_format_constraints(
        allocator, V2CloneImageFormatConstraints(allocator, src.image_format_constraints()));
  }
  return single_buffer_settings;
}

fpromise::result<fuchsia_sysmem2::VmoBuffer, zx_status_t> V2CloneVmoBuffer(
    const fuchsia_sysmem2::VmoBuffer& src, uint32_t vmo_rights_mask, uint32_t aux_vmo_rights_mask) {
  fuchsia_sysmem2::VmoBuffer vmo_buffer;
  if (src.vmo().has_value()) {
    zx::vmo clone_vmo;
    if (src.vmo().value().get() != ZX_HANDLE_INVALID) {
      zx_info_handle_basic_t info{};
      zx_status_t get_info_status =
          src.vmo().value().get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
      if (get_info_status != ZX_OK) {
        LOG(ERROR, "get_info_status: %d", get_info_status);
        return fpromise::error(get_info_status);
      }
      zx_status_t duplicate_status =
          src.vmo().value().duplicate(info.rights & vmo_rights_mask, &clone_vmo);
      if (duplicate_status != ZX_OK) {
        LOG(ERROR, "duplicate_status: %d", duplicate_status);
        return fpromise::error(duplicate_status);
      }
    } else {
      ZX_DEBUG_ASSERT(clone_vmo.get() == ZX_HANDLE_INVALID);
    }
    vmo_buffer.vmo().emplace(std::move(clone_vmo));
  }
  if (src.vmo_usable_start().has_value()) {
    vmo_buffer.vmo_usable_start().emplace(src.vmo_usable_start().value());
  }
  if (src.aux_vmo().has_value()) {
    zx::vmo clone_vmo;
    if (src.aux_vmo().value().get() != ZX_HANDLE_INVALID) {
      zx_info_handle_basic_t info{};
      zx_status_t get_info_status = src.aux_vmo().value().get_info(ZX_INFO_HANDLE_BASIC, &info,
                                                                   sizeof(info), nullptr, nullptr);
      if (get_info_status != ZX_OK) {
        LOG(ERROR, "get_info_status: %d", get_info_status);
        return fpromise::error(get_info_status);
      }
      zx_status_t duplicate_status =
          src.aux_vmo().value().duplicate(info.rights & aux_vmo_rights_mask, &clone_vmo);
      if (duplicate_status != ZX_OK) {
        LOG(ERROR, "duplicate_status: %d", duplicate_status);
        return fpromise::error(duplicate_status);
      }
    } else {
      ZX_DEBUG_ASSERT(clone_vmo.get() == ZX_HANDLE_INVALID);
    }
    vmo_buffer.aux_vmo().emplace(std::move(clone_vmo));
  }
  return fpromise::ok(std::move(vmo_buffer));
}

fpromise::result<fuchsia_sysmem2::wire::VmoBuffer, zx_status_t> V2CloneVmoBuffer(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::VmoBuffer& src,
    uint32_t vmo_rights_mask, uint32_t aux_vmo_rights_mask) {
  fuchsia_sysmem2::wire::VmoBuffer vmo_buffer(allocator);
  if (src.has_vmo()) {
    zx::vmo clone_vmo;
    if (src.vmo().get() != ZX_HANDLE_INVALID) {
      zx_info_handle_basic_t info{};
      zx_status_t get_info_status =
          src.vmo().get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
      if (get_info_status != ZX_OK) {
        LOG(ERROR, "get_info_status: %d", get_info_status);
        return fpromise::error(get_info_status);
      }
      zx_status_t duplicate_status = src.vmo().duplicate(info.rights & vmo_rights_mask, &clone_vmo);
      if (duplicate_status != ZX_OK) {
        LOG(ERROR, "duplicate_status: %d", duplicate_status);
        return fpromise::error(duplicate_status);
      }
    } else {
      ZX_DEBUG_ASSERT(clone_vmo.get() == ZX_HANDLE_INVALID);
    }
    vmo_buffer.set_vmo(std::move(clone_vmo));
  }
  if (src.has_vmo_usable_start()) {
    vmo_buffer.set_vmo_usable_start(allocator, src.vmo_usable_start());
  }
  if (src.has_aux_vmo()) {
    zx::vmo clone_vmo;
    if (src.aux_vmo().get() != ZX_HANDLE_INVALID) {
      zx_info_handle_basic_t info{};
      zx_status_t get_info_status =
          src.aux_vmo().get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
      if (get_info_status != ZX_OK) {
        LOG(ERROR, "get_info_status: %d", get_info_status);
        return fpromise::error(get_info_status);
      }
      zx_status_t duplicate_status =
          src.aux_vmo().duplicate(info.rights & aux_vmo_rights_mask, &clone_vmo);
      if (duplicate_status != ZX_OK) {
        LOG(ERROR, "duplicate_status: %d", duplicate_status);
        return fpromise::error(duplicate_status);
      }
    } else {
      ZX_DEBUG_ASSERT(clone_vmo.get() == ZX_HANDLE_INVALID);
    }
    vmo_buffer.set_aux_vmo(std::move(clone_vmo));
  }
  return fpromise::ok(vmo_buffer);
}

fpromise::result<fuchsia_sysmem2::BufferCollectionInfo, zx_status_t> V2CloneBufferCollectionInfo(
    const fuchsia_sysmem2::BufferCollectionInfo& src, uint32_t vmo_rights_mask,
    uint32_t aux_vmo_rights_mask) {
  fuchsia_sysmem2::BufferCollectionInfo buffer_collection_info;
  if (src.settings().has_value()) {
    // clone via generated copy
    buffer_collection_info.settings().emplace(src.settings().value());
  }
  if (src.buffers().has_value()) {
    buffer_collection_info.buffers().emplace(src.buffers()->size());
    for (uint32_t i = 0; i < src.buffers()->size(); ++i) {
      auto clone_result =
          V2CloneVmoBuffer(src.buffers()->at(i), vmo_rights_mask, aux_vmo_rights_mask);
      if (!clone_result.is_ok()) {
        return clone_result.take_error_result();
      }
      buffer_collection_info.buffers()->at(i) = clone_result.take_value();
    }
  }
  return fpromise::ok(std::move(buffer_collection_info));
}

fpromise::result<fuchsia_sysmem2::wire::BufferCollectionInfo, zx_status_t>
V2CloneBufferCollectionInfo(fidl::AnyArena& allocator,
                            const fuchsia_sysmem2::wire::BufferCollectionInfo& src,
                            uint32_t vmo_rights_mask, uint32_t aux_vmo_rights_mask) {
  fuchsia_sysmem2::wire::BufferCollectionInfo buffer_collection_info(allocator);
  if (src.has_settings()) {
    buffer_collection_info.set_settings(allocator,
                                        V2CloneSingleBufferSettings(allocator, src.settings()));
  }
  if (src.has_buffers()) {
    buffer_collection_info.set_buffers(allocator, allocator, src.buffers().count());
    for (uint32_t i = 0; i < src.buffers().count(); ++i) {
      auto clone_result =
          V2CloneVmoBuffer(allocator, src.buffers()[i], vmo_rights_mask, aux_vmo_rights_mask);
      if (!clone_result.is_ok()) {
        return clone_result.take_error_result();
      }
      buffer_collection_info.buffers()[i] = clone_result.take_value();
    }
  }
  return fpromise::ok(buffer_collection_info);
}

fuchsia_sysmem2::wire::CoherencyDomainSupport V2CloneCoherencyDomainSuppoort(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::CoherencyDomainSupport& src) {
  fuchsia_sysmem2::wire::CoherencyDomainSupport coherency_domain_support(allocator);
  if (src.has_cpu_supported()) {
    coherency_domain_support.set_cpu_supported(src.cpu_supported());
  }
  if (src.has_ram_supported()) {
    coherency_domain_support.set_ram_supported(src.ram_supported());
  }
  if (src.has_inaccessible_supported()) {
    coherency_domain_support.set_inaccessible_supported(src.inaccessible_supported());
  }
  return coherency_domain_support;
}

fuchsia_sysmem2::wire::HeapProperties V2CloneHeapProperties(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::HeapProperties& src) {
  fuchsia_sysmem2::wire::HeapProperties heap_properties(allocator);
  if (src.has_coherency_domain_support()) {
    heap_properties.set_coherency_domain_support(
        allocator, V2CloneCoherencyDomainSuppoort(allocator, src.coherency_domain_support()));
  }
  if (src.has_need_clear()) {
    heap_properties.set_need_clear(src.need_clear());
  }
  return heap_properties;
}

fuchsia_sysmem2::wire::BufferCollectionConstraints V2CloneBufferCollectionConstraints(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::BufferCollectionConstraints& src) {
  fuchsia_sysmem2::wire::BufferCollectionConstraints buffer_collection_constraints(allocator);
  if (src.has_usage()) {
    buffer_collection_constraints.set_usage(allocator, V2CloneBufferUsage(allocator, src.usage()));
  }
  if (src.has_min_buffer_count_for_camping()) {
    buffer_collection_constraints.set_min_buffer_count_for_camping(
        src.min_buffer_count_for_camping());
  }
  if (src.has_min_buffer_count_for_dedicated_slack()) {
    buffer_collection_constraints.set_min_buffer_count_for_dedicated_slack(
        src.min_buffer_count_for_dedicated_slack());
  }
  if (src.has_min_buffer_count_for_shared_slack()) {
    buffer_collection_constraints.set_min_buffer_count_for_shared_slack(
        src.min_buffer_count_for_shared_slack());
  }
  if (src.has_min_buffer_count()) {
    buffer_collection_constraints.set_min_buffer_count(src.min_buffer_count());
  }
  if (src.has_max_buffer_count()) {
    buffer_collection_constraints.set_max_buffer_count(src.max_buffer_count());
  }
  if (src.has_buffer_memory_constraints()) {
    buffer_collection_constraints.set_buffer_memory_constraints(
        allocator, V2CloneBufferMemoryConstraints(allocator, src.buffer_memory_constraints()));
  }
  if (src.has_image_format_constraints()) {
    buffer_collection_constraints.set_image_format_constraints(
        allocator, allocator, src.image_format_constraints().count());
    for (uint32_t i = 0; i < src.image_format_constraints().count(); ++i) {
      buffer_collection_constraints.image_format_constraints()[i] =
          V2CloneImageFormatConstraints(allocator, src.image_format_constraints()[i]);
    }
  }
  if (src.has_need_clear_aux_buffers_for_secure()) {
    buffer_collection_constraints.set_need_clear_aux_buffers_for_secure(
        src.need_clear_aux_buffers_for_secure());
  }
  if (src.has_allow_clear_aux_buffers_for_secure()) {
    buffer_collection_constraints.set_allow_clear_aux_buffers_for_secure(
        src.allow_clear_aux_buffers_for_secure());
  }
  return buffer_collection_constraints;
}

fuchsia_sysmem2::wire::BufferUsage V2CloneBufferUsage(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::BufferUsage& src) {
  fuchsia_sysmem2::wire::BufferUsage buffer_usage(allocator);
  if (src.has_none()) {
    buffer_usage.set_none(src.none());
  }
  if (src.has_cpu()) {
    buffer_usage.set_cpu(src.cpu());
  }
  if (src.has_vulkan()) {
    buffer_usage.set_vulkan(src.vulkan());
  }
  if (src.has_display()) {
    buffer_usage.set_display(src.display());
  }
  if (src.has_video()) {
    buffer_usage.set_video(src.video());
  }
  return buffer_usage;
}

fuchsia_sysmem2::wire::BufferMemoryConstraints V2CloneBufferMemoryConstraints(
    fidl::AnyArena& allocator, const fuchsia_sysmem2::wire::BufferMemoryConstraints& src) {
  fuchsia_sysmem2::wire::BufferMemoryConstraints buffer_memory_constraints(allocator);
  if (src.has_min_size_bytes()) {
    buffer_memory_constraints.set_min_size_bytes(src.min_size_bytes());
  }
  if (src.has_max_size_bytes()) {
    buffer_memory_constraints.set_max_size_bytes(src.max_size_bytes());
  }
  if (src.has_physically_contiguous_required()) {
    buffer_memory_constraints.set_physically_contiguous_required(
        src.physically_contiguous_required());
  }
  if (src.has_secure_required()) {
    buffer_memory_constraints.set_secure_required(src.secure_required());
  }
  if (src.has_cpu_domain_supported()) {
    buffer_memory_constraints.set_cpu_domain_supported(src.cpu_domain_supported());
  }
  if (src.has_ram_domain_supported()) {
    buffer_memory_constraints.set_ram_domain_supported(src.ram_domain_supported());
  }
  if (src.has_inaccessible_domain_supported()) {
    buffer_memory_constraints.set_inaccessible_domain_supported(
        src.inaccessible_domain_supported());
  }
  if (src.has_heap_permitted()) {
    buffer_memory_constraints.set_heap_permitted(allocator, allocator,
                                                 src.heap_permitted().count());
    ZX_DEBUG_ASSERT(buffer_memory_constraints.heap_permitted().count() ==
                    src.heap_permitted().count());
    for (uint32_t i = 0; i < src.heap_permitted().count(); ++i) {
      buffer_memory_constraints.heap_permitted()[i] = src.heap_permitted()[i];
    }
  }
  return buffer_memory_constraints;
}

}  // namespace sysmem
