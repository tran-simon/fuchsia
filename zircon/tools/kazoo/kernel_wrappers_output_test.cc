// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/kazoo/outputs.h"
#include "tools/kazoo/syscall_library.h"
#include "tools/kazoo/test.h"
#include "tools/kazoo/test_ir_test_kernelwrappers.test.h"

namespace {

TEST(KernelWrappersOutput, Various) {
  SyscallLibrary library;
  ASSERT_TRUE(SyscallLibraryLoader::FromJson(k_test_kernelwrappers, &library));

  Writer writer;
  ASSERT_TRUE(KernelWrappersOutput(library, &writer));

  EXPECT_EQ(writer.Out(),
            R"(// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED BY //tools/kazoo. DO NOT EDIT.

extern "C" {

syscall_result wrapper_kwrap_simple_case(uint64_t pc);
syscall_result wrapper_kwrap_simple_case(uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_simple_case, pc, &VDso::ValidSyscallPC::kwrap_simple_case, [&](ProcessDispatcher* current_process) -> uint64_t {
        auto result = sys_kwrap_simple_case();
        return result;
    });
}

syscall_result wrapper_kwrap_multiple_in_handles(SafeSyscallArgument<const zx_handle_t*>::RawType handles, SafeSyscallArgument<size_t>::RawType num_handles, uint64_t pc);
syscall_result wrapper_kwrap_multiple_in_handles(SafeSyscallArgument<const zx_handle_t*>::RawType handles, SafeSyscallArgument<size_t>::RawType num_handles, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_multiple_in_handles, pc, &VDso::ValidSyscallPC::kwrap_multiple_in_handles, [&](ProcessDispatcher* current_process) -> uint64_t {
        auto result = sys_kwrap_multiple_in_handles(make_user_in_ptr(SafeSyscallArgument<const zx_handle_t*>::Sanitize(handles)), SafeSyscallArgument<size_t>::Sanitize(num_handles));
        return result;
    });
}

syscall_result wrapper_kwrap_ano_ret_func(uint64_t pc);
syscall_result wrapper_kwrap_ano_ret_func(uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_ano_ret_func, pc, &VDso::ValidSyscallPC::kwrap_ano_ret_func, [&](ProcessDispatcher* current_process) -> uint64_t {
        /*noreturn*/ sys_kwrap_ano_ret_func();
        /* NOTREACHED */
        return ZX_ERR_BAD_STATE;
    });
}

syscall_result wrapper_kwrap_inout_args(SafeSyscallArgument<zx_handle_t>::RawType handle, SafeSyscallArgument<uint32_t>::RawType op, SafeSyscallArgument<uint64_t>::RawType offset, SafeSyscallArgument<uint64_t>::RawType size, SafeSyscallArgument<void*>::RawType buffer, SafeSyscallArgument<size_t>::RawType buffer_size, uint64_t pc);
syscall_result wrapper_kwrap_inout_args(SafeSyscallArgument<zx_handle_t>::RawType handle, SafeSyscallArgument<uint32_t>::RawType op, SafeSyscallArgument<uint64_t>::RawType offset, SafeSyscallArgument<uint64_t>::RawType size, SafeSyscallArgument<void*>::RawType buffer, SafeSyscallArgument<size_t>::RawType buffer_size, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_inout_args, pc, &VDso::ValidSyscallPC::kwrap_inout_args, [&](ProcessDispatcher* current_process) -> uint64_t {
        auto result = sys_kwrap_inout_args(SafeSyscallArgument<zx_handle_t>::Sanitize(handle), SafeSyscallArgument<uint32_t>::Sanitize(op), SafeSyscallArgument<uint64_t>::Sanitize(offset), SafeSyscallArgument<uint64_t>::Sanitize(size), make_user_inout_ptr(SafeSyscallArgument<void*>::Sanitize(buffer)), SafeSyscallArgument<size_t>::Sanitize(buffer_size));
        return result;
    });
}

syscall_result wrapper_kwrap_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType event, uint64_t pc);
syscall_result wrapper_kwrap_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType event, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_output_handle, pc, &VDso::ValidSyscallPC::kwrap_output_handle, [&](ProcessDispatcher* current_process) -> uint64_t {
        user_out_handle out_handle_event;
        auto result = sys_kwrap_output_handle(&out_handle_event);
        if (result != ZX_OK)
            return result;
        result = out_handle_event.begin_copyout(current_process, make_user_out_ptr(SafeSyscallArgument<zx_handle_t*>::Sanitize(event)));
        if (result != ZX_OK)
            return result;
        out_handle_event.finish_copyout(current_process);
        return result;
    });
}

syscall_result wrapper_kwrap_two_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType out0, SafeSyscallArgument<zx_handle_t*>::RawType out1, uint64_t pc);
syscall_result wrapper_kwrap_two_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType out0, SafeSyscallArgument<zx_handle_t*>::RawType out1, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_two_output_handle, pc, &VDso::ValidSyscallPC::kwrap_two_output_handle, [&](ProcessDispatcher* current_process) -> uint64_t {
        user_out_handle out_handle_out0;
        user_out_handle out_handle_out1;
        auto result = sys_kwrap_two_output_handle(&out_handle_out0, &out_handle_out1);
        if (result != ZX_OK)
            return result;
        result = out_handle_out0.begin_copyout(current_process, make_user_out_ptr(SafeSyscallArgument<zx_handle_t*>::Sanitize(out0)));
        if (result != ZX_OK)
            return result;
        result = out_handle_out1.begin_copyout(current_process, make_user_out_ptr(SafeSyscallArgument<zx_handle_t*>::Sanitize(out1)));
        if (result != ZX_OK)
            return result;
        out_handle_out0.finish_copyout(current_process);
        out_handle_out1.finish_copyout(current_process);
        return result;
    });
}

syscall_result wrapper_kwrap_compiled_out(uint64_t pc);
syscall_result wrapper_kwrap_compiled_out(uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_compiled_out, pc, &VDso::ValidSyscallPC::kwrap_compiled_out, [&](ProcessDispatcher* current_process) -> uint64_t {
        auto result = sys_kwrap_compiled_out();
        return result;
    });
}

}
)");
}

TEST(KernelWrappersOutput, DebugExcludedInRelease) {
  SyscallLibrary library;
  ASSERT_TRUE(SyscallLibraryLoader::FromJson(k_test_kernelwrappers, &library));

  std::set<std::string> exclude{"testonly"};
  library.FilterSyscalls(exclude);

  Writer writer;
  ASSERT_TRUE(KernelWrappersOutput(library, &writer));

  // kwrap_compiled_out does not appear in this output vs. above due to
  // BuildType::kRelease.
  EXPECT_EQ(writer.Out(),
            R"(// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED BY //tools/kazoo. DO NOT EDIT.

extern "C" {

syscall_result wrapper_kwrap_ano_ret_func(uint64_t pc);
syscall_result wrapper_kwrap_ano_ret_func(uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_ano_ret_func, pc, &VDso::ValidSyscallPC::kwrap_ano_ret_func, [&](ProcessDispatcher* current_process) -> uint64_t {
        /*noreturn*/ sys_kwrap_ano_ret_func();
        /* NOTREACHED */
        return ZX_ERR_BAD_STATE;
    });
}

syscall_result wrapper_kwrap_inout_args(SafeSyscallArgument<zx_handle_t>::RawType handle, SafeSyscallArgument<uint32_t>::RawType op, SafeSyscallArgument<uint64_t>::RawType offset, SafeSyscallArgument<uint64_t>::RawType size, SafeSyscallArgument<void*>::RawType buffer, SafeSyscallArgument<size_t>::RawType buffer_size, uint64_t pc);
syscall_result wrapper_kwrap_inout_args(SafeSyscallArgument<zx_handle_t>::RawType handle, SafeSyscallArgument<uint32_t>::RawType op, SafeSyscallArgument<uint64_t>::RawType offset, SafeSyscallArgument<uint64_t>::RawType size, SafeSyscallArgument<void*>::RawType buffer, SafeSyscallArgument<size_t>::RawType buffer_size, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_inout_args, pc, &VDso::ValidSyscallPC::kwrap_inout_args, [&](ProcessDispatcher* current_process) -> uint64_t {
        auto result = sys_kwrap_inout_args(SafeSyscallArgument<zx_handle_t>::Sanitize(handle), SafeSyscallArgument<uint32_t>::Sanitize(op), SafeSyscallArgument<uint64_t>::Sanitize(offset), SafeSyscallArgument<uint64_t>::Sanitize(size), make_user_inout_ptr(SafeSyscallArgument<void*>::Sanitize(buffer)), SafeSyscallArgument<size_t>::Sanitize(buffer_size));
        return result;
    });
}

syscall_result wrapper_kwrap_multiple_in_handles(SafeSyscallArgument<const zx_handle_t*>::RawType handles, SafeSyscallArgument<size_t>::RawType num_handles, uint64_t pc);
syscall_result wrapper_kwrap_multiple_in_handles(SafeSyscallArgument<const zx_handle_t*>::RawType handles, SafeSyscallArgument<size_t>::RawType num_handles, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_multiple_in_handles, pc, &VDso::ValidSyscallPC::kwrap_multiple_in_handles, [&](ProcessDispatcher* current_process) -> uint64_t {
        auto result = sys_kwrap_multiple_in_handles(make_user_in_ptr(SafeSyscallArgument<const zx_handle_t*>::Sanitize(handles)), SafeSyscallArgument<size_t>::Sanitize(num_handles));
        return result;
    });
}

syscall_result wrapper_kwrap_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType event, uint64_t pc);
syscall_result wrapper_kwrap_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType event, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_output_handle, pc, &VDso::ValidSyscallPC::kwrap_output_handle, [&](ProcessDispatcher* current_process) -> uint64_t {
        user_out_handle out_handle_event;
        auto result = sys_kwrap_output_handle(&out_handle_event);
        if (result != ZX_OK)
            return result;
        result = out_handle_event.begin_copyout(current_process, make_user_out_ptr(SafeSyscallArgument<zx_handle_t*>::Sanitize(event)));
        if (result != ZX_OK)
            return result;
        out_handle_event.finish_copyout(current_process);
        return result;
    });
}

syscall_result wrapper_kwrap_simple_case(uint64_t pc);
syscall_result wrapper_kwrap_simple_case(uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_simple_case, pc, &VDso::ValidSyscallPC::kwrap_simple_case, [&](ProcessDispatcher* current_process) -> uint64_t {
        auto result = sys_kwrap_simple_case();
        return result;
    });
}

syscall_result wrapper_kwrap_two_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType out0, SafeSyscallArgument<zx_handle_t*>::RawType out1, uint64_t pc);
syscall_result wrapper_kwrap_two_output_handle(SafeSyscallArgument<zx_handle_t*>::RawType out0, SafeSyscallArgument<zx_handle_t*>::RawType out1, uint64_t pc) {
    return do_syscall(ZX_SYS_kwrap_two_output_handle, pc, &VDso::ValidSyscallPC::kwrap_two_output_handle, [&](ProcessDispatcher* current_process) -> uint64_t {
        user_out_handle out_handle_out0;
        user_out_handle out_handle_out1;
        auto result = sys_kwrap_two_output_handle(&out_handle_out0, &out_handle_out1);
        if (result != ZX_OK)
            return result;
        result = out_handle_out0.begin_copyout(current_process, make_user_out_ptr(SafeSyscallArgument<zx_handle_t*>::Sanitize(out0)));
        if (result != ZX_OK)
            return result;
        result = out_handle_out1.begin_copyout(current_process, make_user_out_ptr(SafeSyscallArgument<zx_handle_t*>::Sanitize(out1)));
        if (result != ZX_OK)
            return result;
        out_handle_out0.finish_copyout(current_process);
        out_handle_out1.finish_copyout(current_process);
        return result;
    });
}

}
)");
}

}  // namespace
