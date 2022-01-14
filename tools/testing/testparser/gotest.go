// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package testparser

import (
	"fmt"
	"regexp"
	"time"
)

var (
	goTestPreamblePattern = regexp.MustCompile(`^=== RUN\s+(.[^/]*)(?:/?)(.*?)$`)
	goTestCasePattern     = regexp.MustCompile(`^\s*--- (\w*?): (.[^/]*)(?:/?)(.*?) \((.*?)\)$`)
	goTestPanicPattern    = regexp.MustCompile(`^panic: test timed out after (\S+)$`)
)

func parseGoTest(lines [][]byte) []TestCaseResult {
	var res []TestCaseResult
	var preambleName string
	for _, line := range lines {
		var matched bool
		line := string(line)
		m := goTestPreamblePattern.FindStringSubmatch(line)
		if m != nil {
			preambleName = m[1]
			continue
		}
		var status TestCaseStatus
		var displayName string
		var suiteName string
		var caseName string
		var duration time.Duration
		m = goTestPanicPattern.FindStringSubmatch(line)
		if m != nil {
			status = Fail
			caseName = preambleName
			displayName = preambleName
			duration, _ = time.ParseDuration(m[1])
			matched = true
		}
		m = goTestCasePattern.FindStringSubmatch(line)
		if m != nil {
			switch m[1] {
			case "PASS":
				status = Pass
			case "FAIL":
				status = Fail
			case "SKIP":
				status = Skip
			}
			if m[3] == "" {
				caseName = m[2]
				displayName = caseName
			} else {
				suiteName = m[2]
				caseName = m[3]
				displayName = fmt.Sprintf("%s/%s", suiteName, caseName)
			}
			duration, _ = time.ParseDuration(m[4])
			matched = true
		}
		if matched {
			res = append(res, TestCaseResult{
				DisplayName: displayName,
				SuiteName:   suiteName,
				CaseName:    caseName,
				Status:      status,
				Duration:    duration,
				Format:      "Go",
			})
		}
	}
	return res
}
