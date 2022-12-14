// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package project

import (
	"fmt"
	"path/filepath"

	spdx_builder "github.com/spdx/tools-golang/builder/builder2v2"
	spdx_common "github.com/spdx/tools-golang/spdx/common"
	spdx "github.com/spdx/tools-golang/spdx/v2_2"
	spdx_utils "github.com/spdx/tools-golang/utils"
)

// Create an spdx.Package struct that matches the given project struct.
func (p *Project) setSPDXFields() error {
	// Use the SPDX builder function to generate an empty SPDX struct.
	// Skip all file and folder processing for now.
	ignore := []string{
		"**",  // Skip all files for now.
		"**/", // Skip all folders for now.
	}
	files := make([]*spdx.File, 0)
	pkg, err := spdx_builder.BuildPackageSection2_2(p.Name, p.Root, ignore)
	if err != nil {
		// If the builder fails to create a pkg object, create one manually.
		code, err2 := spdx_utils.GetVerificationCode2_2(files, "")
		if err2 != nil {
			return fmt.Errorf("failed to create SPDX pkg using builder [%v] and manually [%w]", err, err2)
		}

		pkg = &spdx.Package{
			PackageName:                 p.Name,
			PackageSPDXIdentifier:       spdx_common.ElementID(fmt.Sprintf("Package-%s", p.Name)),
			PackageDownloadLocation:     "NOASSERTION",
			FilesAnalyzed:               true,
			IsFilesAnalyzedTagPresent:   true,
			PackageVerificationCode:     code,
			PackageLicenseConcluded:     "NOASSERTION",
			PackageLicenseInfoFromFiles: []string{},
			PackageLicenseDeclared:      "NOASSERTION",
			PackageCopyrightText:        "NOASSERTION",
		}
	}

	// Initialize these fields to make the online validator happy.
	// https://tools.spdx.org/app/validate/
	pkg.PackageChecksums = make([]spdx_common.Checksum, 0)
	pkg.Files = files
	pkg.IsFilesAnalyzedTagPresent = false
	pkg.IsUnpackaged = false
	pkg.Annotations = make([]spdx.Annotation, 0)

	// Some projects in the Fuchsia tree provide multiple license files.
	// Others have a single large notice file with multiple license texts.
	// In these cases, the SPDX project should specify the SPDX IDs of each
	// license file in a boolean format, e.g.:
	//
	//     (LicenseRef-A AND LicenseRef-B) OR LicenseRef-C
	//
	// More info: https://spdx.github.io/spdx-spec/SPDX-license-expressions/#d4-composite-license-expressions
	//
	// This section generates that statement by simply concatenating all
	// entries together with AND statements. License texts in the same file
	// will appear within the same parenthesis group.
	pkg.PackageLicenseConcluded = ""

	// Multiple files for a given project.
	for i, l := range p.LicenseFile {
		statement := "("
		// Multiple license texts in a given license file.
		for j, data := range l.Data {
			statement = fmt.Sprintf("%s%s", statement, data.SPDXID)
			if j < len(l.Data)-1 {
				statement = fmt.Sprintf("%s AND ", statement)
			}
		}
		statement = fmt.Sprintf("%s)", statement)
		if len(l.Data) > 1 {
			plusVal("Multiple texts in a single file", fmt.Sprintf("%s,%s,%v", p.Root, filepath.Base(l.RelPath), len(l.Data)))
		}

		pkg.PackageLicenseConcluded = fmt.Sprintf("%s%s",
			pkg.PackageLicenseConcluded, statement)

		if i < len(p.LicenseFile)-1 {
			pkg.PackageLicenseConcluded = fmt.Sprintf("%s AND ",
				pkg.PackageLicenseConcluded)
		}
	}
	if len(p.LicenseFile) > 1 {
		filenames := ""
		for _, l := range p.LicenseFile {
			filenames = fmt.Sprintf("%s,%s", filenames, filepath.Base(l.RelPath))
		}
		plusVal("Multiple files in a single package", fmt.Sprintf("%s,%v", p.Root, filenames))
	}
	p.Package = pkg

	return nil
}
