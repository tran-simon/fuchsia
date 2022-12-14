#!/usr/bin/env python3.8
# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import json
import os
import platform
import shlex
import subprocess
import sys


class BootTest(object):

    def __init__(self, images_by_label, test_json):
        test = test_json["test"]

        # The "name" JSON field is a path, so just use the extensionless
        # basename as a shorthand.
        name, ext = os.path.splitext(os.path.basename(test["name"]))
        while ext:
            name, ext = os.path.splitext(name)
        self.name = name

        self.label = test["label"]
        self.path = test["path"]

        # Each of the environments should specify the same images.
        images = test_json["environments"][0]["image_overrides"]
        self.zbi = images_by_label[images["zbi"]] if "zbi" in images else None
        self.qemu_kernel = (
            images_by_label[images["qemu_kernel"]]
            if "qemu_kernel" in images else None)
        self.efi = images_by_label[images["efi"]] if "efi" in images else None

    @staticmethod
    def is_boot_test(test_json):
        envs = test_json.get("environments", [])
        return "image_overrides" in envs[0] if len(envs) else False

    def print(self, command=None):
        kinds = []
        if self.qemu_kernel:
            kinds.append("QEMU kernel")
        if self.zbi:
            kinds.append("ZBI")
        if self.efi:
            kinds.append("EFI")
        print("* %s (%s)" % (self.name, ", ".join(kinds)))
        print("    label: %s" % self.label)
        if command:
            print("    command: %s" % " ".join(map(shlex.quote, command)))


def error(str):
    RED = "\033[91m"
    END = "\033[0m"
    print(RED + "ERROR: " + str + END)


def find_bootserver(build_dir):
    host_os = {"Linux": "linux", "Darwin": "mac"}[platform.system()]
    host_cpu = {"x86_64": "x64", "arm64": "arm64"}[platform.machine()]
    with open(os.path.join(build_dir, "tool_paths.json")) as file:
        tool_paths = json.load(file)
    bootservers = [
        os.path.join(build_dir, tool["path"]) for tool in tool_paths if (
            tool["name"] == "bootserver" and tool["cpu"] == host_cpu and
            tool["os"] == host_os)
    ]
    if bootservers:
        return bootservers[0]
    print("Cannot find bootserver for %s-%s" % (host_os, host_cpu))
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        prog="fx run-boot-test", description="Run a boot test.")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument(
        "--boot", "-b", action="store_true", help="Run via bootserver")
    parser.add_argument(
        "--args",
        "-a",
        metavar="RUNNER-ARG",
        action="append",
        default=[],
        help="Pass RUNNER-ARG to bootserver/fx qemu",
    )
    parser.add_argument(
        "--cmdline",
        "-c",
        metavar="KERNEL-ARGS",
        action="append",
        default=[],
        help="Add kernel command-line arguments.",
    )
    parser.add_argument(
        "name",
        help=
        "Name of the zbi_test()/qemu_kernel_test()/efi_test() target to run",
        nargs="?",
    )
    args = parser.parse_args()

    build_dir = os.path.relpath(os.getenv("FUCHSIA_BUILD_DIR"))
    if build_dir is None:
        print("FUCHSIA_BUILD_DIR not set")
        return 1
    test_cpu = os.getenv("FUCHSIA_ARCH")
    if test_cpu is None:
        print("FUCHSIA_ARCH not set")
        return 1

    # Construct a map of images by GN label. Boot test metadata records its
    # desired images that way.
    with open(os.path.join(build_dir, "images.json")) as file:
        images = {}
        for image in json.load(file):
            images[image["label"]] = image

    # There can be multiple versions of the same boot test for different host
    # architectures. These will otherwise only differ in metadata name, a
    # difference that `BootTest()` normalizes away.
    with open(os.path.join(build_dir, "tests.json")) as file:
        boot_tests = {}
        for test in json.load(file):
            if BootTest.is_boot_test(test):
                boot_test = BootTest(images, test)
                boot_tests[boot_test.name] = boot_test

    if not args.name:
        for test in boot_tests.values():
            test.print()
        return 0

    matches = []
    for test in boot_tests.values():
        if args.name in test.name:
            matches.append(test)
    if len(matches) == 0:
        error("no boot test with '%s' in name found" % args.name)
        return 1
    elif len(matches) > 1:
        error("more than one boot test with '%s' in the name" % args.name)
        for test in matches:
            test.print()
        return 1

    test = matches[0]
    if args.boot:
        if test.qemu_kernel:
            error("cannot use --boot with QEMU-only test %s" % test.name)
            return 1
        assert test.zbi
        bootserver = find_bootserver(build_dir)
        cmd = [bootserver, "--boot"] + test.zbi["path"] + args.args
    else:
        cmd = ["fx", "qemu"] + args.args
        if test.qemu_kernel:
            cmd += ["-t", test.qemu_kernel["path"]]
        if test.zbi:
            cmd += ["-z", test.zbi["path"]]
        if test.efi:
            cmd += ["--uefi", test.efi["path"]]

    for arg in args.cmdline:
        cmd += ["-c", arg]

    if not args.boot:
        # Prevents QEMU from boot-looping, as most boot tests do not have a
        # means of gracefully shutting down.
        cmd += ["--", "-no-reboot"]

    test.print(command=cmd)
    return subprocess.run(cmd, cwd=build_dir).returncode


if __name__ == "__main__":
    sys.exit(main())
