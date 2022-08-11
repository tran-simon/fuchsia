// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

import (
	"embed"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

//go:embed *.tmpl
var templates embed.FS

// All currently active allowlists
const (
	// AllowlistTestdataGoldens allows golden testing of allowlist support by
	// permitting the //tools/fidl/fidlc/testdata/allowlist.fidl file.
	AllowlistTestdataGoldens AllowlistName = "testdata_goldens"
)

// al is a global map of all currently enabled allowlists.
var al = AllowlistMap{
	AllowlistName(AllowlistTestdataGoldens): []EncodedLibraryIdentifier{
		EncodedLibraryIdentifier("test.allowlist"),
	},
}

type Generator struct{ *fidlgen.Generator }

func NewGenerator(rustfmtPath, rustfmtConfigPath string) *Generator {
	var args []string
	if rustfmtConfigPath != "" {
		args = append(args, "--config-path", rustfmtConfigPath)
	}
	formatter := fidlgen.NewFormatter(rustfmtPath, args...)

	return &Generator{fidlgen.NewGenerator("RustTemplates", templates, formatter, template.FuncMap{})}
}

func (gen *Generator) GenerateFidl(ir fidlgen.Root, outputFilename string) error {
	tree := Compile(ir, al)
	return gen.GenerateFile(outputFilename, "GenerateSourceFile", tree)
}
