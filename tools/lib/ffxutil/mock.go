// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package ffxutil

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
)

type MockFFXInstance struct {
	CmdsCalled  []string
	TestOutcome string
}

func (f *MockFFXInstance) SetStdoutStderr(_, _ io.Writer) {
}

func (f *MockFFXInstance) run(cmd string) error {
	f.CmdsCalled = append(f.CmdsCalled, cmd)
	return nil
}

func (f *MockFFXInstance) Test(_ context.Context, tests []TestDef, outDir string, _ ...string) (*TestRunResult, error) {
	f.run("test")
	outcome := TestPassed
	if f.TestOutcome != "" {
		outcome = f.TestOutcome
	}
	var suites []suiteEntry
	for i, test := range tests {
		suite := suiteEntry{fmt.Sprintf("summary%d.json", i)}
		summaryFile := filepath.Join(outDir, suite.Summary)
		fmt.Println("writing to ", summaryFile)
		summaryBytes, err := json.Marshal(SuiteResult{
			Outcome: outcome,
			Name:    test.TestUrl,
		})
		if err != nil {
			return nil, err
		}
		if err := ioutil.WriteFile(summaryFile, summaryBytes, os.ModePerm); err != nil {
			return nil, err
		}
		suites = append(suites, suite)
	}
	return &TestRunResult{
		Outcome:   outcome,
		Suites:    suites,
		outputDir: outDir,
	}, nil
}

func (f *MockFFXInstance) Snapshot(_ context.Context, _, _ string) error {
	return f.run("snapshot")
}

func (f *MockFFXInstance) Stop() error {
	return f.run("stop")
}

func (f *MockFFXInstance) ContainsCmd(cmd string) bool {
	for _, c := range f.CmdsCalled {
		if c == cmd {
			return true
		}
	}
	return false
}
