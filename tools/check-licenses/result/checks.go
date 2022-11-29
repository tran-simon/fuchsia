// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package result

import (
	"fmt"
	"net/http"
	"regexp"
	"sort"
	"strings"
	"time"

	"go.fuchsia.dev/fuchsia/tools/check-licenses/directory"
	"go.fuchsia.dev/fuchsia/tools/check-licenses/license"
	"go.fuchsia.dev/fuchsia/tools/check-licenses/project"
)

type Check struct {
	Name      string          `json: "name"`
	Allowlist map[string]bool `json:"allowlist"`
}

func RunChecks() error {
	if err := AllFuchsiaAuthorSourceFilesMustHaveCopyrightHeaders(); err != nil {
		return err
	}
	if err := AllLicenseTextsMustBeRecognized(); err != nil {
		return err
	}
	if err := AllLicensePatternUsagesMustBeApproved(); err != nil {
		return err
	}
	// TODO: Enable this when running outside of CQ
	//if err := AllComplianceWorksheetLinksAreGood(); err != nil {
	//	return err
	//}
	if err := AllProjectsMustHaveALicense(); err != nil {
		return err
	}
	if err := AllFilesAndFoldersMustBeIncludedInAProject(); err != nil {
		// TODO: Enable this check after all projects have been addressed.
		return nil
	}
	if err := AllReadmeFuchsiaFilesMustBeFormattedCorrectly(); err != nil {
		// TODO: Enable this check after all projects have been addressed.
		return nil
	}
	return nil
}

// =============================================================================

func AllFuchsiaAuthorSourceFilesMustHaveCopyrightHeaders() error {
	var b strings.Builder
	b.WriteString("All source files owned by The Fuchsia Authors must contain a copyright header.\n")
	b.WriteString("The following files have missing or incorrectly worded copyright header information:\n\n")

	var fuchsia *project.Project
	for _, p := range project.FilteredProjects {
		if p.Root == "." {
			fuchsia = p
			break
		}
	}

	if fuchsia == nil {
		return fmt.Errorf("Couldn't find Fuchsia project to verify this check!!\n")
	}
	count := 0
	for _, f := range fuchsia.SearchableFiles {
		if len(f.Data) == 0 {
			return fmt.Errorf("Found a file that hasn't been parsed yet?? %v\n", f.AbsPath)
		}
	OUTER:
		for _, fd := range f.Data {
			for _, p := range license.AllCopyrightPatterns {
				if _, ok := p.PreviousMatches[fd.Hash()]; ok {
					continue OUTER
				}
			}
			b.WriteString(fmt.Sprintf("-> %v\n", fd.FilePath))
			count = count + 1
		}
	}
	b.WriteString(fmt.Sprintf("\nPlease add the standard Fuchsia copyright header info to the above %v files.\n", count))
	if count > 0 {
		return fmt.Errorf(b.String())
	}
	return nil
}

func AllLicenseTextsMustBeRecognized() error {
	if len(license.Unrecognized.Matches) > 0 {
		var b strings.Builder
		b.WriteString("Found unrecognized license texts - please add the relevant license pattern(s) to //tools/check-licenses/license/patterns/* and have it(them) reviewed by the OSRB team:\n\n")
		for _, m := range license.Unrecognized.Matches {
			b.WriteString(fmt.Sprintf("-> Line %v of %v\n", m.LineNumber, m.FilePath))
			b.WriteString(fmt.Sprintf("\n%v\n\n", string(m.Data)))
		}
		return fmt.Errorf(b.String())
	}
	return nil
}

func AllLicensePatternUsagesMustBeApproved() error {
	var b strings.Builder
OUTER:
	for _, sr := range license.AllSearchResults {
		if sr.Pattern.Category == "approved_production" {
			continue
		}
		if sr.Pattern.Category == "approved_development" {
			if !(strings.Contains(Config.BuildInfoProduct, "user") || strings.Contains(Config.BuildInfoBoard, "user")) {
				continue
			}
		}

		filepath := sr.LicenseData.FilePath
		allowlist := sr.Pattern.AllowList
		for _, entry := range allowlist {
			re, err := regexp.Compile("(" + entry + ")")
			if err != nil {
				return err
			}
			if m := re.Find([]byte(filepath)); m != nil {
				continue OUTER
			}
		}
		for _, exceptions := range sr.Pattern.Exceptions {
			for _, entry := range exceptions.Entries {
				for _, project := range entry.Projects {
					if sr.ProjectRoot == project {
						continue OUTER
					}
				}
			}
		}
		b.WriteString(fmt.Sprintf("File %v was not approved to use license pattern %v\n", filepath, sr.Pattern.RelPath))
	}

	result := b.String()
	if len(result) > 0 {
		return fmt.Errorf("Encountered license texts that were not approved for usage:\n%v", result)
	}
	return nil
}

func AllComplianceWorksheetLinksAreGood() error {
	for _, sr := range license.AllSearchResults {
		if strings.HasPrefix(sr.Pattern.Name, "_") {
			continue
		}
		url := sr.LicenseData.URL
		if url != "" {
			resp, err := http.Get(url)
			if err != nil {
				fmt.Printf("%v-%v %v: %v \n", sr.ProjectRoot, sr.Pattern.Name, url, err)
			} else if resp.Status != "200 OK" {
				fmt.Printf("%v-%v %v: %v\n", sr.ProjectRoot, sr.Pattern.Name, url, resp.Status)
			}
			time.Sleep(200 * time.Millisecond)
		}
	}
	return nil

}

func AllProjectsMustHaveALicense() error {
	name := "AllProjectsMustHaveALicense"

	var b strings.Builder
	b.WriteString("All projects should include relevant license information, and a README.fuchsia file pointing to the file.\n")
	b.WriteString("The following projects were found without any license information:\n\n")

	// Retrieve allowlists from config files
	allowlist := make(map[string]bool, 0)
	for _, c := range Config.Checks {
		if c.Name == name {
			for k, v := range c.Allowlist {
				allowlist[k] = v
			}
		}
	}

	badReadmes := make([]string, 0)
	for _, p := range project.FilteredProjects {
		if _, ok := allowlist[p.Root]; ok {
			continue
		}

		if len(p.LicenseFile) == 0 {
			badReadmes = append(badReadmes, fmt.Sprintf("-> %v (README.fuchsia file: %v)\n", p.Root, p.ReadmePath))
		}
	}
	sort.Strings(badReadmes)

	if len(badReadmes) > 0 {
		for _, s := range badReadmes {
			b.WriteString(s)
		}
	}
	b.WriteString("\nPlease add a LICENSE file to the above projects, and point to them in the associated README.fuchsia file.\n")
	if len(badReadmes) > 0 {
		return fmt.Errorf(b.String())
	}
	return nil
}

func AllFilesAndFoldersMustBeIncludedInAProject() error {
	var b strings.Builder
	var recurse func(*directory.Directory)
	count := 0

	b.WriteString("All files and folders must have proper license attribution.\n")
	b.WriteString("This means a license file needs to accompany all first and third party projects,\n")
	b.WriteString("and a README.fuchsia file must exist and specify where the license file lives.\n")
	b.WriteString("The following directories are not included in a project:\n\n")
	recurse = func(d *directory.Directory) {
		if d.Project == nil {
			b.WriteString(fmt.Sprintf("-> %v\n", d.Path))
			count = count + 1
			return
		}
		for _, child := range d.Children {
			recurse(child)
		}
	}
	b.WriteString("\nPlease add a LICENSE file to the above projects, and point to them in an associated README.fuchsia file.\n")

	recurse(directory.RootDirectory)
	if count > 0 {
		return fmt.Errorf(b.String())
	}
	return nil
}

func AllReadmeFuchsiaFilesMustBeFormattedCorrectly() error {
	//TODO
	return nil
}
