// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <asm.h>
#include <zircon/boot/image.h>


// This file lays out the final kernel image seen by the boot loader.
// It concatenates:
//     1. the boot loader headers
//     2. the actual kernel image (converted from the kernel ELF file)
//     3. the fixup code to relocate the kernel image
// The headers must tell the boot loader to load the whole combined image,
// and leave enough space in memory after it for the bss.
//
// The label arithmetic to define the header fields only works because this
// whole file is all in the same section (.text).  Because it's all just
// one big section and there are no relocs to absolute locations within
// this section, it really doesn't matter what memory layout the linker
// thinks it's doing, but nonetheless image.ld produces an ELF segment
// layout faithful to the physical memory picture (except that it's
// actually position-independent).  The addresses in the ELF headers of the
// final image.elf file are completely ignored because boot loaders don't
// actually use that file.  It only exists to have the contents extracted
// with objcopy -O binary.

.text

// ZBI file header (zbi_header_t)
ZBI_CONTAINER_HEADER(_zbi_file_header, boot_load_end - _zbi_kernel_header)

// ZBI kernel header (zbi_header_t)
DATA(_zbi_kernel_header)
    .int ZBI_TYPE_KERNEL_ARM64
    .int boot_load_end - _zbi_kernel_payload
    .int 0
    .int ZBI_FLAGS_VERSION
    .int 0
    .int 0
    .int ZBI_ITEM_MAGIC
    .int ZBI_ITEM_NO_CRC32
END_DATA(_zbi_kernel_header)

// ZBI_TYPE_KERNEL payload (zbi_kernel_t)
DATA(_zbi_kernel_payload)
    // The boot-shim code expects this to be an offset from the beginning
    // of the load image, whatever the kernel's virtual address.
    .quad IMAGE_ELF_ENTRY - _zbi_file_header
    .quad IMAGE_MEMORY_END - boot_load_end
END_DATA(_zbi_kernel_payload)

// Include the kernel image itself, skipping the padding left for the headers.
#include "kernel-image.inc"

// Immediately after the kernel image comes the fixup code.
// The start.S code sees this address as __data_end.

#define FIXUP_LOCATION(addr) (addr - KERNEL_BASE + IMAGE_LOAD_START)

// This code must be purely position-independent and have no relocs.
// This is called with the desired runtime address of __code_start in x0.
FUNCTION(apply_fixups)
    // This is the constant address the kernel was linked for.
    movlit x9, KERNEL_BASE
    sub x0, x0, x9

// The generated kernel-fixups.inc invokes this macro for each run of fixups.
.macro fixup addr, n, stride
    adr_global x9, FIXUP_LOCATION(\addr)
.if \n >= 4 && \stride == 8
    // Do a loop handling adjacent pairs.
    mov x16, #(\n / 2)
0:  fixup_pair
    subs x16, x16, #1
    b.ne 0b
 .if \n % 2
    // Handle the odd remainder after those pairs.
    fixup_single 8
 .endif
.elseif \n >= 2 && \stride == 8
    // Do a single adjacent pair.
    fixup_pair
 .if \n == 3
    // Do the third adjacent one.
    fixup_single 8
 .endif
.elseif \n > 1
    // Do a strided loop.
    mov x16, #\n
0:  fixup_single \stride
    subs x16, x16, #1
    b.ne 0b
.else
    // Do a singleton.
    fixup_single 8
.endif
.endm

.macro fixup_pair
    ldp x10, x11, [x9]
    add x10, x10, x0
    add x11, x11, x0
    stp x10, x11, [x9], #16
.endm

.macro fixup_single stride
    ldr x10, [x9]
    add x10, x10, x0
    str x10, [x9], #\stride
.endm

#include "kernel-fixups.inc"

    ret

DATA(apply_fixups_end)
END_FUNCTION(apply_fixups)

.balign 8
DATA(boot_load_end)

// We don't use any scratch memory after the kernel's bss.
.globl IMAGE_RESERVE_SIZE
IMAGE_RESERVE_SIZE = 0
