// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package paver

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/lib/logger"
	"golang.org/x/crypto/ssh"
)

const ImageManifest = "images.json"

type BuildPaver struct {
	BootserverPath  string
	ImageDir        string
	sshPublicKey    ssh.PublicKey
	overrideVBMetaA *string
	overrideZirconA *string
	stdout          io.Writer
}

type Mode int

const (
	Default Mode = iota
	ZedbootOnly
	SkipZedboot
)

type Options struct {
	Mode          Mode
	TftpBlockSize uint64
}

type Paver interface {
	PaveWithOptions(ctx context.Context, deviceName string, options Options) error
	Pave(ctx context.Context, deviceName string) error
}

// NewBuildPaver constructs a new paver that uses `bootserverPath` as the path
// to the tool used to pave Zedboot and Fuchsia with the image manifest located
// in `imageDir`. Also accepts a number of optional parameters.
func NewBuildPaver(bootserverPath, imageDir string, options ...BuildPaverOption) (*BuildPaver, error) {
	p := &BuildPaver{
		BootserverPath: bootserverPath,
		ImageDir:       imageDir,
	}

	for _, opt := range options {
		if err := opt(p); err != nil {
			return nil, err
		}
	}

	return p, nil
}

type BuildPaverOption func(p *BuildPaver) error

// Sets the SSH public key that the Paver will bake into the device as an
// authorized key.
func SSHPublicKey(publicKey ssh.PublicKey) BuildPaverOption {
	return func(p *BuildPaver) error {
		p.sshPublicKey = publicKey
		return nil
	}
}

// Sets a path to an image that the Paver will use to override the ZIRCON-A ZBI.
func OverrideSlotA(imgPath string) BuildPaverOption {
	return func(p *BuildPaver) error {
		if _, err := os.Stat(imgPath); err != nil {
			return err
		}
		p.overrideZirconA = &imgPath
		return nil
	}
}

// Sets the paths to the images that the Paver will use to override vbmeta_a.
func OverrideVBMetaA(vbmetaPath string) BuildPaverOption {
	return func(p *BuildPaver) error {
		if _, err := os.Stat(vbmetaPath); err != nil {
			return err
		}
		p.overrideVBMetaA = &vbmetaPath
		return nil
	}
}

// Send stdout from the paver scripts to `writer`. Defaults to the parent
// stdout.
func Stdout(writer io.Writer) BuildPaverOption {
	return func(p *BuildPaver) error {
		p.stdout = writer
		return nil
	}
}

// Pave runs a paver service for one pave. If `deviceName` is not empty, the
// pave will only be applied to the specified device.
func (p *BuildPaver) PaveWithOptions(ctx context.Context, deviceName string, options Options) error {
	paverArgs := []string{"--fail-fast-if-version-mismatch"}

	if options.TftpBlockSize != 0 {
		paverArgs = append(paverArgs, "-b", strconv.FormatUint(options.TftpBlockSize, 10))
	}

	// Write out the public key's authorized keys.
	if p.sshPublicKey != nil && options.Mode != ZedbootOnly {
		authorizedKeys, err := os.CreateTemp("", "")
		if err != nil {
			return err
		}
		defer os.Remove(authorizedKeys.Name())

		if _, err := authorizedKeys.Write(ssh.MarshalAuthorizedKey(p.sshPublicKey)); err != nil {
			return err
		}

		if err := authorizedKeys.Close(); err != nil {
			return err
		}

		paverArgs = append(paverArgs, "--authorized-keys", authorizedKeys.Name())
	}

	if p.overrideZirconA != nil {
		paverArgs = append(paverArgs, "--zircona", *p.overrideZirconA)
	}

	if p.overrideVBMetaA != nil {
		paverArgs = append(paverArgs, "--vbmetaa", *p.overrideVBMetaA)
	}

	if options.Mode != SkipZedboot {
		// Run bootserver with pave-zedboot mode to bootstrap the new bootloader and zedboot.
		if err := p.runPave(ctx, deviceName, "--mode", "pave-zedboot", "--allow-zedboot-version-mismatch"); err != nil {
			return err
		}
	}

	if options.Mode != ZedbootOnly {
		// Run bootserver with pave mode to install Fuchsia.
		paverArgs = append([]string{"--mode", "pave"}, paverArgs...)
		return p.runPave(ctx, deviceName, paverArgs...)
	}

	return nil
}

// Pave runs a paver service for one pave and includes Zedboot. If `deviceName` is not empty, the
// pave will only be applied to the specified device.
func (p *BuildPaver) Pave(ctx context.Context, deviceName string) error {
	return p.PaveWithOptions(ctx, deviceName, Options{Mode: Default})
}

func (p *BuildPaver) runPave(ctx context.Context, deviceName string, args ...string) error {
	args = append([]string{"--images", filepath.Join(p.ImageDir, ImageManifest)}, args...)

	logger.Infof(ctx, "paving device %q", deviceName)
	path, err := exec.LookPath(p.BootserverPath)
	if err != nil {
		return err
	}

	args = append(args, "-1")
	if deviceName != "" {
		args = append(args, "-n", deviceName)
	}

	supportsLogLevel, err := supportsLogLevel(ctx, path)
	if err != nil {
		return err
	}

	if supportsLogLevel {
		args = append(args, "-log-level", "debug")
	}

	logger.Infof(ctx, "running: %s %q", path, args)
	cmd := exec.CommandContext(ctx, path, args...)
	if p.stdout != nil {
		cmd.Stdout = p.stdout
	} else {
		cmd.Stdout = os.Stdout
	}
	cmd.Stderr = os.Stderr
	cmdRet := cmd.Run()
	logger.Infof(ctx, "finished running %s %q: %q", path, args, cmdRet)
	return cmdRet
}

// Check if bootserver supports `-log-level` by running `bootserver -log-level
// debug`. The bootserver supports the flag if:
//
//   - the process provides an exit code of 1.
//   - the process's stderr ends with:
//   - "no images provided!\n"
//   - "cannot specify a bootserver mode without an image manifest [--images]\n"
//
// If the bootserver does not support the flag if:
//
// * the process provides an exit code of 2.
// * the stderr starts with "flag provide but not defined: -log-level".
//
// Anything else will be treated as the bootserver does not support
// `-log-level`, in case any of the output changes.
func supportsLogLevel(ctx context.Context, bootserverPath string) (bool, error) {
	args := []string{"-log-level", "debug"}
	cmd := exec.CommandContext(ctx, bootserverPath, args...)

	var stderr bytes.Buffer
	cmd.Stdout = nil
	cmd.Stderr = &stderr

	err := cmd.Run()
	if err == nil {
		logger.Warningf(ctx, "unexpected success running %v, assuming does not support -log-level", args)
		return false, nil
	}

	if exitErr, ok := err.(*exec.ExitError); ok {
		if exitErr.ExitCode() == 1 {
			for _, suffix := range []string{
				"bootserver FATAL: no images provided!\n",
				"cannot specify a bootserver mode without an image manifest [--images]\n",
			} {
				if strings.HasSuffix(stderr.String(), suffix) {
					return true, nil
				}
			}

			logger.Warningf(ctx, "was unable to parse stderr, assuming bootserver does not support -log-level: %s", stderr.String())
			return false, nil
		}

		if exitErr.ExitCode() == 2 {
			if !strings.HasPrefix(stderr.String(), "flag provided but not defined: -log-level") {
				logger.Warningf(ctx, "was unable to parse stderr, assuming bootserver does not support -log-level: %s", stderr.String())
			}

			return false, nil
		}
	}

	if len(stderr.String()) == 0 {
		logger.Warningf(ctx, "unexpected result running %v, assuming does not support -log-level: %v", args, err)
	} else {
		logger.Warningf(ctx, "unexpected result running %v, assuming does not support -log-level: %v: stderr: %v", args, err, stderr.String())
	}
	return false, nil
}
