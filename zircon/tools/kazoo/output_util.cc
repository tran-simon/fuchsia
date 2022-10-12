// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/kazoo/output_util.h"

#include <zircon/assert.h>

#include <algorithm>
#include <string_view>

#include "tools/kazoo/string_util.h"

void CopyrightHeaderWithCppComments(Writer* writer) {
  writer->Puts(R"(// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED BY //tools/kazoo. DO NOT EDIT.

)");
}

void CopyrightHeaderWithHashComments(Writer* writer) {
  writer->Puts(R"(# Copyright 2019 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# WARNING: THIS FILE IS MACHINE GENERATED BY //tools/kazoo. DO NOT EDIT.

)");
}

std::string ToLowerAscii(const std::string& input) {
  std::string ret = input;
  std::transform(ret.begin(), ret.end(), ret.begin(), ToLowerASCII);
  return ret;
}

std::string ToUpperAscii(const std::string& input) {
  std::string ret = input;
  std::transform(ret.begin(), ret.end(), ret.begin(), ToUpperASCII);
  return ret;
}

std::string CamelToSnake(const std::string& camel_fidl) {
  auto is_transition = [](char prev, char cur, char peek) {
    enum { Upper, Lower, Other };
    auto categorize = [](char c) {
      if (c == 0)
        return Other;
      if (c >= 'a' && c <= 'z')
        return Lower;
      if (c >= 'A' && c <= 'Z')
        return Upper;
      if ((c >= '0' && c <= '9') || c == '_')
        return Other;
      ZX_ASSERT_MSG(false, "%c is unexpected character", c);
      return Other;
    };
    auto prev_type = categorize(prev);
    auto cur_type = categorize(cur);
    auto peek_type = categorize(peek);

    bool lower_to_upper = prev_type != Upper && cur_type == Upper;
    bool multiple_caps_to_lower =
        peek && (prev_type == Upper && cur_type == Upper && peek_type == Lower);

    return lower_to_upper || multiple_caps_to_lower;
  };
  std::vector<std::string> parts;
  char prev = 0;
  std::string current_word;
  for (size_t i = 0; i < camel_fidl.size(); ++i) {
    char cur = camel_fidl[i];
    char peek = i + 1 < camel_fidl.size() ? camel_fidl[i + 1] : 0;
    if (current_word.size() > 1 && is_transition(prev, cur, peek)) {
      parts.push_back(ToLowerAscii(current_word));
      current_word = cur;
    } else {
      current_word += cur;
    }
    prev = cur;
  }

  if (!current_word.empty()) {
    parts.push_back(ToLowerAscii(current_word));
  }

  return JoinStrings(parts, "_");
}

namespace {

// Most of the implementation of GetCUserModeName() and GetCKernelModeName(), other than for
// pointers.
std::string CNameImpl(const Type& type) {
  struct {
   public:
    void operator()(const std::monostate&) { ret = "<TODO!>"; }
    void operator()(const TypeBool&) { ret = "uint32_t"; }
    void operator()(const TypeUchar&) { ret = "char"; }
    void operator()(const TypeInt8&) { ret = "int8_t"; }
    void operator()(const TypeInt16&) { ret = "int16_t"; }
    void operator()(const TypeInt32&) { ret = "int32_t"; }
    void operator()(const TypeInt64&) { ret = "int64_t"; }
    void operator()(const TypeUsize&) { ret = "size_t"; }
    void operator()(const TypeUint8&) { ret = "uint8_t"; }
    void operator()(const TypeUint16&) { ret = "uint16_t"; }
    void operator()(const TypeUint32&) { ret = "uint32_t"; }
    void operator()(const TypeUint64&) { ret = "uint64_t"; }
    void operator()(const TypeUintptr&) { ret = "uintptr_t"; }
    void operator()(const TypeVoid&) { ret = "void"; }
    void operator()(const TypeZxBasicAlias& zx_basic_alias) { ret = zx_basic_alias.c_name(); }

    void operator()(const TypeAlias& alias) { ret = "zx_" + alias.alias_data().base_name() + "_t"; }
    void operator()(const TypeEnum& enm) { ret = "zx_" + enm.enum_data().base_name() + "_t"; }
    void operator()(const TypeHandle& handle) {
      ret = "zx_handle_t";
      // TODO(syscall-fidl-transition): Once we're not trying to match abigen, it might be nice to
      // add the underlying handle type here like "zx_handle_t /*vmo*/ handle" or similar.
    }
    void operator()(const TypePointer& pointer) {
      ZX_ASSERT(false && "pointers should be handled by caller");
      ret = "<!>";
    }
    void operator()(const TypeStruct& strukt) {
      ret = "zx_" + strukt.struct_data().base_name() + "_t";
    }
    void operator()(const TypeVector&) {
      ZX_ASSERT(false && "can't convert vector to C directly");
      ret = "<!>";
    }

    Constness constness;
    std::string ret;
  } name_visitor;
  name_visitor.constness = type.constness();
  std::visit(name_visitor, type.type_data());
  return name_visitor.ret;
}

}  // namespace

std::string GetCUserModeName(const Type& type) {
  if (type.IsPointer()) {
    ZX_ASSERT(type.constness() != Constness::kUnspecified &&
              "Pointer should be explicitly const or mutable by output time");
    return (type.constness() == Constness::kConst ? "const " : "") +
           GetCUserModeName(type.DataAsPointer().pointed_to_type()) + "*";
  }
  return CNameImpl(type);
}

std::string GetCKernelModeName(const Type& type) {
  if (type.IsPointer()) {
    ZX_ASSERT(type.constness() != Constness::kUnspecified &&
              "Pointer should be explictly const or mutable by output time");
    std::string pointed_to = GetCKernelModeName(type.DataAsPointer().pointed_to_type());
    if (type.constness() == Constness::kConst) {
      return StringPrintf("user_in_ptr<const %s>", pointed_to.c_str());
    } else if (type.constness() == Constness::kMutable) {
      if (pointed_to == "zx_handle_t" && !type.DataAsPointer().was_vector()) {
        return "user_out_handle*";
      }
      if (type.optionality() == Optionality::kInputArgument) {
        return StringPrintf("user_inout_ptr<%s>", pointed_to.c_str());
      } else {
        return StringPrintf("user_out_ptr<%s>", pointed_to.c_str());
      }
    }
  }
  return CNameImpl(type);
}

JsonTypeNameData GetJsonName(const Type& type) {
  JsonTypeNameData ret;
  if (type.IsPointer()) {
    ret.name = GetCUserModeName(type.DataAsPointer().pointed_to_type());
    ret.is_pointer = true;
    if (type.constness() == Constness::kConst) {
      ret.attribute = "IN";
    } else if (type.constness() == Constness::kMutable) {
      if (type.optionality() == Optionality::kInputArgument) {
        ret.attribute = "INOUT";
      } else if (type.optionality() == Optionality::kOutputNonOptional) {
        ret.attribute = "OUT";
      } else if (type.optionality() == Optionality::kOutputOptional) {
        ret.attribute = "optional";
      }
    }

    if (ret.name == "void") {
      ret.name = "any";
    }
  } else {
    ret.name = GetCUserModeName(type);
  }
  return ret;
}

namespace {

std::string GetGoNameImpl(const Type& type) {
  struct {
   public:
    void operator()(const std::monostate&) { ret = "<TODO!>"; }
    void operator()(const TypeBool&) { ret = "uint32"; }
    void operator()(const TypeUchar&) { ret = "uint8"; }
    void operator()(const TypeInt8&) { ret = "int8"; }
    void operator()(const TypeInt16&) { ret = "int16"; }
    void operator()(const TypeInt32&) { ret = "int32"; }
    void operator()(const TypeInt64&) { ret = "int64"; }
    void operator()(const TypeUsize&) { ret = "uint"; }
    void operator()(const TypeUint8&) { ret = "uint8"; }
    void operator()(const TypeUint16&) { ret = "uint16"; }
    void operator()(const TypeUint32&) { ret = "uint32"; }
    void operator()(const TypeUint64&) { ret = "uint64"; }
    void operator()(const TypeUintptr&) { ret = "uintptr"; }
    void operator()(const TypeVoid&) { ret = "void"; }
    void operator()(const TypeZxBasicAlias& zx_basic_alias) { ret = zx_basic_alias.name(); }

    void operator()(const TypeAlias& alias) { ret = "zx_" + alias.alias_data().base_name() + "_t"; }
    void operator()(const TypeEnum& enm) { ret = "zx_" + enm.enum_data().base_name() + "_t"; }
    void operator()(const TypeHandle& handle) { ret = "Handle"; }
    void operator()(const TypePointer& pointer) {
      if (pointer.pointed_to_type().IsVoid()) {
        ret = "unsafe.Pointer";
      } else {
        ret = "*" + GetGoName(pointer.pointed_to_type());
      }
    }
    void operator()(const TypeStruct& strukt) {
      ret = "zx_" + strukt.struct_data().base_name() + "_t";
    }
    void operator()(const TypeVector&) {
      ZX_ASSERT(false && "can't convert vector directly");
      ret = "<!>";
    }

    Constness constness;
    std::string ret;
  } name_visitor;
  name_visitor.constness = type.constness();
  std::visit(name_visitor, type.type_data());
  return name_visitor.ret;
}

}  // namespace

std::string GetGoName(const Type& type) {
  // Most of the "normal" implementation is in GetGoNameImpl(), and then we do
  // a few hacks here to make it match the previous output of the Go-written
  // tool that parsed abigen directly.
  // TODO(scottmg|dhobsd): Remove or rationalize these over time.
  std::string name = GetGoNameImpl(type);
  if (name == "Futex")
    return "int32";
  if (name == "Koid")
    return "uint64";
  if (name == "Ticks")
    return "int64";
  if (name == "VmOption")
    return "VMFlag";
  if (name == "zx_channel_call_args_t")
    return "ChannelCallArgs";
  if (name == "zx_channel_call_etc_args_t")
    return "ChannelCallEtcArgs";
  if (name == "zx_clock_t")
    return "uint32";
  if (name == "zx_handle_disposition_t")
    return "HandleDisposition";
  if (name == "zx_handle_info_t")
    return "HandleInfo";
  if (name == "Off" || name == "zx_off_t")
    return "uint64";
  if (name == "Iovec")
    return "uintptr";
  if (name == "zx_pci_bar_t")
    return "uintptr";
  if (name == "zx_pci_init_arg_t")
    return "uintptr";
  if (name == "zx_pcie_device_info_t")
    return "uintptr";
  if (name == "zx_port_packet_t")
    return "int";
  if (name == "zx_profile_info_t")
    return "int";
  if (name == "zx_rights_t")
    return "Rights";
  if (name == "zx_stream_seek_origin_t")
    return "uint32";
  if (name == "zx_smc_parameters_t")
    return "SMCParameters";
  if (name == "zx_smc_result_t")
    return "SMCResult";
  if (name == "zx_system_powerctl_arg_t")
    return "int";
  if (name == "zx_wait_item_t")
    return "WaitItem";
  if (name == "StringView")
    return "unsafe.Pointer";
  return name;
}

// Returns a size-compatible Go native type.
std::string GetNativeGoName(const Type& type) {
  if (type.IsPointer()) {
    return "unsafe.Pointer";
  }
  std::string name = GetGoNameImpl(type);

  // Most of the "normal" implementation is in GetGoNameImpl(), and then we do
  // a few hacks here to make it match the previous output of the Go-written
  // tool that parsed abigen directly.
  // TODO(scottmg|dhobsd): Remove or rationalize these over time.
  if (name == "Duration")
    return "int64";
  if (name == "Futex")
    return "int32";
  if (name == "Handle")
    return "uint32";
  if (name == "Koid")
    return "uint64";
  if (name == "Paddr")
    return "uintptr";
  if (name == "Signals")
    return "uint32";
  if (name == "Status")
    return "int32";
  if (name == "Ticks")
    return "int64";
  if (name == "Time")
    return "int64";
  if (name == "Vaddr")
    return "uintptr";
  if (name == "VmOption")
    return "uint32";
  if (name == "StringView")
    return "unsafe.Pointer";
  if (name == "zx_channel_call_args_t")
    return "uintptr";
  if (name == "zx_clock_t")
    return "uint32";
  if (name == "zx_handle_disposition_t")
    return "uintptr";
  if (name == "zx_handle_info_t")
    return "int";
  if (name == "zx_off_t")
    return "uint64";
  if (name == "zx_pci_bar_t")
    return "uintptr";
  if (name == "zx_pci_init_arg_t")
    return "uintptr";
  if (name == "zx_pcie_device_info_t")
    return "uintptr";
  if (name == "zx_port_packet_t")
    return "int";
  if (name == "zx_profile_info_t")
    return "int";
  if (name == "zx_rights_t")
    return "uint32";
  if (name == "zx_stream_seek_origin_t")
    return "uint32";
  if (name == "zx_smc_parameters_t")
    return "uintptr";
  if (name == "zx_smc_result_t")
    return "uintptr";
  if (name == "zx_system_powerctl_arg_t")
    return "int";
  if (name == "zx_wait_item_t")
    return "uintptr";
  return name;
}

std::string RemapReservedGoName(const std::string& name) {
  // Probably more of these and/or a better way to do this, but this handles current syscalls.
  if (name == "type")
    return "typ";
  if (name == "func")
    return "funk";
  if (name == "g")
    return "g_";
  return name;
}

// http://www.cse.yorku.ca/~oz/hash.html
// Used by Go output, taken from
// https://fuchsia.googlesource.com/third_party/go/+/ae00e6ff359966a496acacc3aa386c79e972279d/src/runtime/mkfuchsiavdso.go#36
// TODO(scottmg): Not clear on whether this must match something in Go internals.
uint32_t DJBHash(const std::string& str) {
  uint32_t h = 5381;
  for (const auto& c : str) {
    h = (h << 5) + h + static_cast<uint32_t>(c);
  }
  return h;
}
