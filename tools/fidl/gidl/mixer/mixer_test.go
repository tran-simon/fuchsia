// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package mixer

import (
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

var hostDir = map[string]string{"arm64": "host_arm64", "amd64": "host_x64"}[runtime.GOARCH]

func getTestDataDir() string {
	base := filepath.Join("..", "..", "..", "..")
	c, err := os.ReadFile(filepath.Join(base, ".fx-build-dir"))
	if err != nil {
		return ""
	}
	return filepath.Join(base, strings.TrimSpace(string(c)), hostDir, "test_data", "gidl")
}

var testDataDir = flag.String("test_data_dir", getTestDataDir(), "Path to golden files; only used in GN build")

func testSchema(t *testing.T) Schema {
	path := filepath.Join(*testDataDir, "mixer.test.fidl.json")
	bytes, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("please \"fx build %s/test_data/gidl/mixer.test.fidl.json\" first then \"go test\" again", hostDir)
	}
	root := fidlgen.Root{}
	if err = json.Unmarshal(bytes, &root); err != nil {
		t.Fatalf("failed to unmarshal %s: %s", path, err)
	}
	return BuildSchema(root)
}

// checkStruct is a helper function to test the Declaration for a struct.
func checkStruct(t *testing.T, decl *StructDecl, expectedName string, expectedNullable bool) {
	t.Helper()
	qualifiedName := decl.Name()
	expectedQualifiedName := fmt.Sprintf("test.mixer/%s", expectedName)
	if qualifiedName != expectedQualifiedName {
		t.Errorf("expected name to be %s, got %s\n\ndecl: %#v",
			expectedQualifiedName, qualifiedName, decl)
	}
	if decl.nullable != expectedNullable {
		t.Errorf("expected nullable to be %v, got %v\n\ndecl: %#v",
			expectedNullable, decl.nullable, decl)
	}
}

func defaultMetadataForHandle(h gidlir.Handle) gidlir.HandleWithRights {
	return gidlir.HandleWithRights{
		Handle: h,
		Type:   fidlgen.ObjectTypeNone,
		Rights: fidlgen.HandleRightsSameRights,
	}
}

func TestLookupDeclByNameNonNullable(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleStruct", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	checkStruct(t, decl.(*StructDecl), "ExampleStruct", false)
}

func TestLookupDeclByNameNullable(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleStruct", true)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	checkStruct(t, decl.(*StructDecl), "ExampleStruct", true)
}

func TestLookupDeclByNameFailure(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ThisIsNotAStruct", false)
	if ok {
		t.Fatalf("lookupDeclByName unexpectedly succeeded: %#v", decl)
	}
}

func TestLookupDeclByTypeSuccess(t *testing.T) {
	typ := fidlgen.Type{
		Kind:             fidlgen.PrimitiveType,
		PrimitiveSubtype: fidlgen.Bool,
	}
	decl, ok := testSchema(t).lookupDeclByType(typ)
	if !ok {
		t.Fatalf("lookupDeclByType failed")
	}
	if _, ok := decl.(*BoolDecl); !ok {
		t.Fatalf("expected BoolDecl, got %T\n\ndecl: %#v", decl, decl)
	}
}

func TestExtractDeclarationSuccess(t *testing.T) {
	value := gidlir.Record{
		Name: "ExampleStruct",
		Fields: []gidlir.Field{
			{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
		},
	}
	decl, err := testSchema(t).ExtractDeclaration(value, nil)
	if err != nil {
		t.Fatalf("ExtractDeclaration failed: %s", err)
	}
	checkStruct(t, decl, "ExampleStruct", false)
}

func TestExtractDeclarationNotDefined(t *testing.T) {
	value := gidlir.Record{
		Name:   "ThisIsNotAStruct",
		Fields: []gidlir.Field{},
	}
	decl, err := testSchema(t).ExtractDeclaration(value, nil)
	if err == nil {
		t.Fatalf("ExtractDeclaration unexpectedly succeeded: %#v", decl)
	}
	if !strings.Contains(err.Error(), "unknown") {
		t.Fatalf("expected err to contain 'unknown', got '%s'", err)
	}
}

func TestExtractDeclarationDoesNotConform(t *testing.T) {
	value := gidlir.Record{
		Name: "ExampleStruct",
		Fields: []gidlir.Field{
			{Key: gidlir.FieldKey{Name: "ThisIsNotAField"}, Value: "foo"},
		},
	}
	decl, err := testSchema(t).ExtractDeclaration(value, nil)
	if err == nil {
		t.Fatalf("ExtractDeclaration unexpectedly succeeded: %#v", decl)
	}
	if !strings.Contains(err.Error(), "conform") {
		t.Fatalf("expected err to contain 'conform', got '%s'", err)
	}
}

func TestExtractDeclarationWrongHandleTypeFailure(t *testing.T) {
	value := gidlir.Record{
		Name: "ExampleHandleStruct",
		Fields: []gidlir.Field{
			{
				Key: gidlir.FieldKey{
					Name:           "channel",
					UnknownOrdinal: 1,
				},
				Value: gidlir.HandleWithRights{
					Handle: 0,
					Type:   fidlgen.ObjectTypeChannel,
					Rights: fidlgen.HandleRightsDuplicate,
				},
			},
		},
	}
	handleDefs := []gidlir.HandleDef{
		{
			Subtype: fidlgen.HandleSubtypeFifo,
			Rights:  fidlgen.HandleRightsTransfer,
		},
	}
	decl, err := testSchema(t).ExtractDeclaration(value, handleDefs)
	if err == nil {
		t.Fatalf("ExtractDeclaration unexpectedly succeeded: %#v", decl)
	}
	if !strings.Contains(err.Error(), "expecting handle:channel") {
		t.Fatalf("expected err to contain 'failed to conform to declaration', got '%s'", err)
	}
}

func TestExtractDeclarationEncodeSuccessWrongHandleTypeSuccess(t *testing.T) {
	value := gidlir.Record{
		Name: "ExampleHandleStruct",
		Fields: []gidlir.Field{
			{
				Key: gidlir.FieldKey{
					Name:           "channel",
					UnknownOrdinal: 1,
				},
				Value: gidlir.HandleWithRights{
					Handle: 0,
					Type:   fidlgen.ObjectTypeChannel,
					Rights: fidlgen.HandleRightsDuplicate,
				},
			},
		},
	}
	handleDefs := []gidlir.HandleDef{
		{
			Subtype: fidlgen.HandleSubtypeFifo,
			Rights:  fidlgen.HandleRightsTransfer,
		},
	}
	decl, err := testSchema(t).ExtractDeclarationEncodeSuccess(value, handleDefs)
	if err != nil {
		t.Fatalf("ExtractDeclaration failed: %s", err)
	}
	checkStruct(t, decl, "ExampleHandleStruct", false)
}

func TestExtractDeclarationUnsafeSuccess(t *testing.T) {
	value := gidlir.Record{
		Name: "ExampleStruct",
		Fields: []gidlir.Field{
			{Key: gidlir.FieldKey{Name: "ThisIsNotAField"}, Value: "foo"},
		},
	}
	decl, err := testSchema(t).ExtractDeclarationUnsafe(value)
	if err != nil {
		t.Fatalf("ExtractDeclarationUnsafe failed: %s", err)
	}
	checkStruct(t, decl, "ExampleStruct", false)
}

func TestExtractDeclarationByNameSuccess(t *testing.T) {
	decl, err := testSchema(t).ExtractDeclarationByName("ExampleStruct")
	if err != nil {
		t.Fatalf("ExtractDeclarationUnsafe failed: %s", err)
	}
	checkStruct(t, decl, "ExampleStruct", false)
}

// conformTest describes a test case for the Declaration.conforms method.
type conformTest interface {
	value() gidlir.Value
}

type conformOk struct {
	val gidlir.Value
}
type conformFail struct {
	val          gidlir.Value
	errSubstring string
}

func (c conformOk) value() gidlir.Value   { return c.val }
func (c conformFail) value() gidlir.Value { return c.val }

// checkConforms is a helper function to test the Declaration.conforms method.
func checkConforms(t *testing.T, ctx context, decl Declaration, tests []conformTest) {
	t.Helper()
	for _, test := range tests {
		value := test.value()
		err := decl.conforms(value, ctx)
		switch test := test.(type) {
		case conformOk:
			if err != nil {
				t.Errorf(
					"value failed to conform to declaration\n\nvalue: %#v\n\nerr: %s\n\ndecl: %#v",
					value, err, decl)
			}
		case conformFail:
			if err == nil {
				t.Errorf(
					"value unexpectedly conformed to declaration\n\nvalue: %#v\n\ndecl: %#v",
					value, decl)
			} else if !strings.Contains(err.Error(), test.errSubstring) {
				t.Errorf("expected error containing %q, but got %q", test.errSubstring, err.Error())
			}
		default:
			panic("unreachable")
		}
	}
}

func TestBoolDeclConforms(t *testing.T) {
	checkConforms(t,
		context{},
		&BoolDecl{},
		[]conformTest{
			conformOk{false},
			conformOk{true},
			conformFail{nil, "expecting bool"},
			conformFail{"foo", "expecting bool"},
			conformFail{42, "expecting bool"},
			conformFail{int64(42), "expecting bool"},
		},
	)
}

func TestIntegerDeclConforms(t *testing.T) {
	checkConforms(t,
		context{},
		&IntegerDecl{subtype: fidlgen.Uint8, lower: 0, upper: 255},
		[]conformTest{
			conformOk{uint64(0)},
			conformOk{uint64(128)},
			conformOk{uint64(255)},
			conformFail{uint64(256), "out of range"},
			conformFail{int64(256), "out of range"},
			conformFail{int64(-1), "out of range"},
			conformFail{nil, "expecting int64 or uint64"},
			conformFail{0, "expecting int64 or uint64"},
			conformFail{uint(0), "expecting int64 or uint64"},
			conformFail{int8(0), "expecting int64 or uint64"},
			conformFail{uint8(0), "expecting int64 or uint64"},
			conformFail{"foo", "expecting int64 or uint64"},
			conformFail{1.5, "expecting int64 or uint64"},
		},
	)
	checkConforms(t,
		context{},
		&IntegerDecl{subtype: fidlgen.Int64, lower: -5, upper: 10},
		[]conformTest{
			conformOk{int64(-5)},
			conformOk{int64(10)},
			conformOk{uint64(10)},
			conformFail{int64(-6), "out of range"},
			conformFail{int64(11), "out of range"},
			conformFail{uint64(11), "out of range"},
		},
	)
}

func TestFloatDeclConforms(t *testing.T) {
	tests := []conformTest{
		conformOk{0.0},
		conformOk{1.5},
		conformOk{-1.0},
		conformOk{gidlir.RawFloat(0)},
		conformFail{nil, "expecting float64"},
		conformFail{float32(0.0), "expecting float64"},
		conformFail{0, "expecting float64"},
		conformFail{"foo", "expecting float64"},
		conformFail{math.Inf(1), "must use raw_float"},
		conformFail{math.Inf(-1), "must use raw_float"},
		conformFail{math.NaN(), "must use raw_float"},
	}
	tests32 := []conformTest{
		conformOk{math.MaxFloat32},
		conformOk{gidlir.RawFloat(math.Float32bits(float32(math.Inf(1))))},
		conformOk{gidlir.RawFloat(math.Float32bits(float32(math.NaN())))},
		conformFail{gidlir.RawFloat(0x1122334455), "out of range"},
	}
	tests64 := []conformTest{
		conformOk{math.MaxFloat64},
		conformOk{gidlir.RawFloat(math.Float64bits(math.Inf(1)))},
		conformOk{gidlir.RawFloat(math.Float64bits(math.NaN()))},
	}
	checkConforms(t, context{}, &FloatDecl{subtype: fidlgen.Float32}, append(tests, tests32...))
	checkConforms(t, context{}, &FloatDecl{subtype: fidlgen.Float64}, append(tests, tests64...))
}

func TestStringDeclConforms(t *testing.T) {
	checkConforms(t,
		context{},
		&StringDecl{bound: nil, nullable: false},
		[]conformTest{
			conformOk{""},
			conformOk{"the quick brown fox"},
			conformFail{nil, "expecting non-null string"},
			conformFail{0, "expecting string"},
		},
	)
	checkConforms(t,
		context{},
		&StringDecl{bound: nil, nullable: true},
		[]conformTest{
			conformOk{"foo"},
			conformOk{nil},
			conformFail{0, "expecting string"},
		},
	)
	two := 2
	checkConforms(t,
		context{},
		&StringDecl{bound: &two, nullable: false},
		[]conformTest{
			conformOk{""},
			conformOk{"1"},
			conformOk{"12"},
			conformFail{"123", "too long"},
			conformFail{"the quick brown fox", "too long"},
		},
	)
}

func TestHandleDeclConforms(t *testing.T) {
	// Cannot refer to any handles if there are no handle_defs.
	checkConforms(t,
		context{},
		&HandleDecl{subtype: fidlgen.HandleSubtypeEvent, nullable: false},
		[]conformTest{
			conformFail{
				defaultMetadataForHandle(-1),
				"out of range",
			},
			conformFail{
				defaultMetadataForHandle(0),
				"out of range",
			},
			conformFail{
				defaultMetadataForHandle(1),
				"out of range",
			},
			conformFail{
				defaultMetadataForHandle(2),
				"out of range",
			},
			conformFail{
				defaultMetadataForHandle(3),
				"out of range",
			},
			conformFail{nil, "expecting non-null handle"},
			conformFail{"foo", "expecting handle"},
			conformFail{0, "expecting handle"},
		},
	)
	// The FIDL type `handle` is compatible with all subtypes.
	checkConforms(t,
		context{
			handleDefs: []gidlir.HandleDef{
				{Subtype: fidlgen.HandleSubtypeEvent}, // #0
				{Subtype: fidlgen.HandleSubtypePort},  // #1
				{Subtype: fidlgen.HandleSubtypeEvent}, // #2
			},
		},
		&HandleDecl{subtype: fidlgen.HandleSubtypeNone, nullable: false},
		[]conformTest{
			conformOk{
				defaultMetadataForHandle(0),
			},
			conformOk{
				defaultMetadataForHandle(1),
			},
			conformOk{
				defaultMetadataForHandle(2),
			},
			conformFail{
				defaultMetadataForHandle(-1),
				"out of range",
			},
			conformFail{
				defaultMetadataForHandle(3),
				"out of range",
			},
			conformFail{nil, "expecting non-null handle"},
			conformFail{"foo", "expecting handle"},
			conformFail{0, "expecting handle"},
		},
	)
	// The FIDL type `handle<event>` requires an event.
	checkConforms(t,
		context{
			handleDefs: []gidlir.HandleDef{
				{Subtype: fidlgen.HandleSubtypeEvent}, // #0
				{Subtype: fidlgen.HandleSubtypePort},  // #1
				{Subtype: fidlgen.HandleSubtypeEvent}, // #2
			},
		},
		&HandleDecl{subtype: fidlgen.HandleSubtypeEvent, nullable: false},
		[]conformTest{
			conformOk{
				defaultMetadataForHandle(0),
			},
			conformOk{
				defaultMetadataForHandle(2),
			},
			conformFail{
				defaultMetadataForHandle(1),
				"expecting handle:event",
			},
			conformFail{
				defaultMetadataForHandle(-1),
				"out of range",
			},
			conformFail{
				defaultMetadataForHandle(3),
				"out of range",
			},
			conformFail{nil, "expecting non-null handle"},
			conformFail{"foo", "expecting handle"},
			conformFail{0, "expecting handle"},
		},
	)
	// The FIDL type `handle<port>?` requires an event or nil.
	checkConforms(t,
		context{
			handleDefs: []gidlir.HandleDef{
				{Subtype: fidlgen.HandleSubtypeEvent}, // #0
				{Subtype: fidlgen.HandleSubtypePort},  // #1
				{Subtype: fidlgen.HandleSubtypeEvent}, // #2
			},
		},
		&HandleDecl{subtype: fidlgen.HandleSubtypePort, nullable: true},
		[]conformTest{
			conformOk{
				defaultMetadataForHandle(1),
			},
			conformOk{nil},
			conformFail{
				defaultMetadataForHandle(0),
				"expecting handle:port",
			},
			conformFail{
				defaultMetadataForHandle(2),
				"expecting handle:port",
			},
			conformFail{
				defaultMetadataForHandle(-1),
				"out of range",
			},
			conformFail{
				defaultMetadataForHandle(3),
				"out of range",
			},
			conformFail{0, "expecting handle"},
		},
	)
}

func TestProtocolEndpointConforms(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleEndpointStruct", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	structDecl := decl.(*StructDecl)
	checkConforms(t,
		context{
			handleDefs: []gidlir.HandleDef{
				{Subtype: fidlgen.HandleSubtypeChannel}, // #0
				{Subtype: fidlgen.HandleSubtypeChannel}, // #1
				{Subtype: fidlgen.HandleSubtypeChannel}, // #2
				{Subtype: fidlgen.HandleSubtypeChannel}, // #3
			},
		},
		structDecl,
		[]conformTest{
			conformOk{gidlir.Record{
				Name: "ExampleEndpointStruct",
				Fields: []gidlir.Field{
					{
						Key: gidlir.FieldKey{
							Name: "client_end",
						},
						Value: defaultMetadataForHandle(0),
					},
					{
						Key: gidlir.FieldKey{
							Name: "optional_client_end",
						},
						Value: defaultMetadataForHandle(1),
					},
					{
						Key: gidlir.FieldKey{
							Name: "server_end",
						},
						Value: defaultMetadataForHandle(2),
					},
					{
						Key: gidlir.FieldKey{
							Name: "optional_server_end",
						},
						Value: defaultMetadataForHandle(3),
					},
				},
			}},
			conformOk{gidlir.Record{
				Name: "ExampleEndpointStruct",
				Fields: []gidlir.Field{
					{
						Key: gidlir.FieldKey{
							Name: "client_end",
						},
						Value: defaultMetadataForHandle(0),
					},
					{
						Key: gidlir.FieldKey{
							Name: "server_end",
						},
						Value: defaultMetadataForHandle(2),
					},
				},
			}},
			conformFail{
				gidlir.Record{
					Name: "ExampleEndpointStruct",
					Fields: []gidlir.Field{
						{
							Key: gidlir.FieldKey{
								Name: "client_end",
							},
							Value: defaultMetadataForHandle(0),
						},
					},
				}, "missing non-nullable field server_end",
			},
		},
	)
	checkConforms(t,
		context{
			handleDefs: []gidlir.HandleDef{
				{Subtype: fidlgen.HandleSubtypeChannel}, // #0
				{Subtype: fidlgen.HandleSubtypeChannel}, // #1
				{Subtype: fidlgen.HandleSubtypeEvent},   // #2
				{Subtype: fidlgen.HandleSubtypeChannel}, // #3
			},
		},
		structDecl,
		[]conformTest{
			conformFail{gidlir.Record{
				Name: "ExampleEndpointStruct",
				Fields: []gidlir.Field{
					{
						Key: gidlir.FieldKey{
							Name: "client_end",
						},
						Value: defaultMetadataForHandle(0),
					},
					{
						Key: gidlir.FieldKey{
							Name: "server_end",
						},
						Value: defaultMetadataForHandle(2),
					},
				},
			}, "expecting handle:channel, found handle:event"},
		},
	)
}

func TestStrictBitsConforms(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleStrictBits", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	bitsDecl := decl.(*BitsDecl)
	checkConforms(t,
		context{},
		bitsDecl,
		[]conformTest{
			// The only valid bit of ExampleStrictBits is `B = 1`.
			conformOk{uint64(0)},
			conformOk{uint64(1)},
			conformFail{uint64(2), "invalid for strict bits"},
			conformFail{uint64(255), "invalid for strict bits"},
			// Underlying type for ExampleStrictBits is uint8.
			conformFail{uint64(256), "out of range"},
			conformFail{int64(256), "out of range"},
			conformFail{int64(-1), "out of range"},
			conformFail{nil, "expecting int64 or uint64"},
			conformFail{0, "expecting int64 or uint64"},
			conformFail{uint(0), "expecting int64 or uint64"},
			conformFail{int8(0), "expecting int64 or uint64"},
			conformFail{uint8(0), "expecting int64 or uint64"},
			conformFail{"foo", "expecting int64 or uint64"},
			conformFail{1.5, "expecting int64 or uint64"},
		},
	)
}

func TestFlexibleBitsDeclConforms(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleFlexibleBits", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	bitsDecl := decl.(*BitsDecl)
	checkConforms(t,
		context{},
		bitsDecl,
		[]conformTest{
			// Underlying type for ExampleFlexibleBits is uint8.
			conformOk{uint64(0)},
			conformOk{uint64(1)},
			conformOk{uint64(2)},
			conformOk{uint64(255)},
			conformFail{uint64(256), "out of range"},
			conformFail{int64(256), "out of range"},
			conformFail{int64(-1), "out of range"},
			conformFail{nil, "expecting int64 or uint64"},
			conformFail{0, "expecting int64 or uint64"},
			conformFail{uint(0), "expecting int64 or uint64"},
			conformFail{int8(0), "expecting int64 or uint64"},
			conformFail{uint8(0), "expecting int64 or uint64"},
			conformFail{"foo", "expecting int64 or uint64"},
			conformFail{1.5, "expecting int64 or uint64"},
		},
	)
}

func TestStrictEnumDeclConforms(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleStrictEnum", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	enumDecl := decl.(*EnumDecl)
	checkConforms(t,
		context{},
		enumDecl,
		[]conformTest{
			// The only valid member of ExampleStrictEnum is `E = 1`.
			conformFail{uint64(0), "invalid for strict enum"},
			conformOk{uint64(1)},
			conformFail{uint64(2), "invalid for strict enum"},
			conformFail{uint64(255), "invalid for strict enum"},
			// Underlying type for ExampleStrictEnum is uint8.
			conformFail{uint64(256), "out of range"},
			conformFail{int64(256), "out of range"},
			conformFail{int64(-1), "out of range"},
			conformFail{nil, "expecting int64 or uint64"},
			conformFail{0, "expecting int64 or uint64"},
			conformFail{uint(0), "expecting int64 or uint64"},
			conformFail{int8(0), "expecting int64 or uint64"},
			conformFail{uint8(0), "expecting int64 or uint64"},
			conformFail{"foo", "expecting int64 or uint64"},
			conformFail{1.5, "expecting int64 or uint64"},
		},
	)
}

func TestFlexibleEnumDeclConforms(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleFlexibleEnum", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	enumDecl := decl.(*EnumDecl)
	checkConforms(t,
		context{},
		enumDecl,
		[]conformTest{
			// Underlying type for ExampleFlexibleEnum is uint8.
			conformOk{uint64(0)},
			conformOk{uint64(1)},
			conformOk{uint64(2)},
			conformOk{uint64(255)},
			conformFail{uint64(256), "out of range"},
			conformFail{int64(256), "out of range"},
			conformFail{int64(-1), "out of range"},
			conformFail{nil, "expecting int64 or uint64"},
			conformFail{0, "expecting int64 or uint64"},
			conformFail{uint(0), "expecting int64 or uint64"},
			conformFail{int8(0), "expecting int64 or uint64"},
			conformFail{uint8(0), "expecting int64 or uint64"},
			conformFail{"foo", "expecting int64 or uint64"},
			conformFail{1.5, "expecting int64 or uint64"},
		},
	)
}

func TestStructDeclConformsNonNullable(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleStruct", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	structDecl := decl.(*StructDecl)
	checkConforms(t,
		context{},
		structDecl,
		[]conformTest{
			conformOk{gidlir.Record{
				Name: "ExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformFail{gidlir.Record{
				Name: "ExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "DefinitelyNotS"}, Value: "foo"},
				},
			}, "field DefinitelyNotS: unknown"},
			conformFail{gidlir.Record{
				Name: "DefinitelyNotExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}, "expecting struct test.mixer/ExampleStruct"},
			conformFail{nil, "expecting non-null struct"},
			conformFail{"foo", "expecting struct"},
			conformFail{0, "expecting struct"},
		},
	)
}

func TestStructDeclConformsNullable(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleStruct", true)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	structDecl := decl.(*StructDecl)
	checkConforms(t,
		context{},
		structDecl,
		[]conformTest{
			conformOk{gidlir.Record{
				Name: "ExampleStruct",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformOk{nil},
		},
	)
}

func TestTableDeclConforms(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleTable", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	tableDecl := decl.(*TableDecl)
	checkConforms(t,
		context{},
		tableDecl,
		[]conformTest{
			conformOk{gidlir.Record{
				Name: "ExampleTable",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformOk{gidlir.Record{
				Name: "ExampleTable",
				Fields: []gidlir.Field{
					{
						// 2 is a reserved field
						Key:   gidlir.FieldKey{UnknownOrdinal: 2},
						Value: gidlir.UnknownData{},
					},
				},
			}},
			conformOk{gidlir.Record{
				Name: "ExampleTable",
				Fields: []gidlir.Field{
					{
						// 3 is an unknown field
						Key:   gidlir.FieldKey{UnknownOrdinal: 3},
						Value: gidlir.UnknownData{},
					},
				},
			}},
			conformFail{gidlir.Record{
				Name: "ExampleTable",
				Fields: []gidlir.Field{
					{
						// 1 is a known field
						Key:   gidlir.FieldKey{UnknownOrdinal: 1},
						Value: gidlir.UnknownData{},
					},
				},
			}, "field name must be used rather than ordinal 1"},
			conformFail{gidlir.Record{
				Name: "ExampleTable",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "DefinitelyNotS"}, Value: "foo"},
				},
			}, "field DefinitelyNotS: unknown"},
			conformFail{gidlir.Record{
				Name: "DefinitelyNotExampleTable",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}, "expecting table test.mixer/ExampleTable"},
			conformFail{nil, "expecting non-null table"},
			conformFail{"foo", "expecting table"},
			conformFail{0, "expecting table"},
		},
	)
}

func TestFlexibleUnionDeclConformsNonNullable(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleFlexibleUnion", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	unionDecl := decl.(*UnionDecl)
	checkConforms(t,
		context{},
		unionDecl,
		[]conformTest{
			conformOk{gidlir.Record{
				Name: "ExampleFlexibleUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformOk{gidlir.Record{
				Name: "ExampleFlexibleUnion",
				Fields: []gidlir.Field{
					{
						Key:   gidlir.FieldKey{UnknownOrdinal: 2},
						Value: gidlir.UnknownData{},
					},
				},
			}},
			conformFail{gidlir.Record{
				Name: "ExampleFlexibleUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "DefinitelyNotS"}, Value: "foo"},
				},
			}, "field DefinitelyNotS: unknown"},
			conformFail{gidlir.Record{
				Name: "DefinitelyNotExampleFlexibleUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}, "expecting union test.mixer/ExampleFlexibleUnion"},
			conformFail{gidlir.Record{
				Name: "ExampleFlexibleUnion",
				Fields: []gidlir.Field{
					{
						Key:   gidlir.FieldKey{UnknownOrdinal: 1},
						Value: gidlir.UnknownData{},
					},
				},
			}, "field name must be used rather than ordinal 1"},
			conformFail{nil, "expecting non-null union"},
			conformFail{"foo", "expecting union"},
			conformFail{0, "expecting union"},
		},
	)
}

func TestUnionDeclConformsNullable(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleFlexibleUnion", true)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	unionDecl := decl.(*UnionDecl)
	checkConforms(t,
		context{},
		unionDecl,
		[]conformTest{
			conformOk{gidlir.Record{
				Name: "ExampleFlexibleUnion",
				Fields: []gidlir.Field{
					{Key: gidlir.FieldKey{Name: "s"}, Value: "foo"},
				},
			}},
			conformOk{nil},
		},
	)
}

func TestStrictUnionConforms(t *testing.T) {
	decl, ok := testSchema(t).lookupDeclByName("ExampleStrictUnion", false)
	if !ok {
		t.Fatalf("lookupDeclByName failed")
	}
	unionDecl := decl.(*UnionDecl)
	checkConforms(t,
		context{},
		unionDecl,
		[]conformTest{
			conformFail{gidlir.Record{
				Name: "ExampleStrictUnion",
				Fields: []gidlir.Field{
					{
						Key:   gidlir.FieldKey{UnknownOrdinal: 2},
						Value: gidlir.UnknownData{},
					},
				},
			}, "cannot use unknown ordinal in a strict union"},
		},
	)
}

func TestArrayDeclConforms(t *testing.T) {
	two := 2
	checkConforms(t,
		context{},
		&ArrayDecl{
			schema: testSchema(t),
			typ: fidlgen.Type{
				Kind:         fidlgen.ArrayType,
				ElementCount: &two,
				ElementType: &fidlgen.Type{
					Kind:             fidlgen.PrimitiveType,
					PrimitiveSubtype: fidlgen.Uint8,
				},
			},
		},
		[]conformTest{
			conformOk{[]gidlir.Value{uint64(1), uint64(2)}},
			conformFail{[]gidlir.Value{}, "expecting 2 elements"},
			conformFail{[]gidlir.Value{uint64(1)}, "expecting 2 elements"},
			conformFail{[]gidlir.Value{uint64(1), uint64(1), uint64(1)}, "expecting 2 elements"},
			conformFail{[]gidlir.Value{"a", "b"}, "[0]: expecting int64 or uint64"},
			conformFail{[]gidlir.Value{nil, nil}, "[0]: expecting int64 or uint64"},
		},
	)
}

func TestVectorDeclConforms(t *testing.T) {
	two := 2
	checkConforms(t,
		context{},
		&VectorDecl{
			schema: testSchema(t),
			typ: fidlgen.Type{
				Kind:         fidlgen.VectorType,
				ElementCount: &two,
				ElementType: &fidlgen.Type{
					Kind:             fidlgen.PrimitiveType,
					PrimitiveSubtype: fidlgen.Uint8,
				},
			},
		},
		[]conformTest{
			conformOk{[]gidlir.Value{}},
			conformOk{[]gidlir.Value{uint64(1)}},
			conformOk{[]gidlir.Value{uint64(1), uint64(2)}},
			conformFail{[]gidlir.Value{uint64(1), uint64(1), uint64(1)}, "expecting at most 2 elements"},
			conformFail{[]gidlir.Value{"a", "b"}, "[0]: expecting int64 or uint64"},
			conformFail{[]gidlir.Value{nil, nil}, "[0]: expecting int64 or uint64"},
		},
	)
}

func TestVectorDeclConformsWithHandles(t *testing.T) {
	checkConforms(t,
		context{
			handleDefs: []gidlir.HandleDef{
				{Subtype: fidlgen.HandleSubtypeEvent},
				{Subtype: fidlgen.HandleSubtypeEvent},
			},
		},
		&VectorDecl{
			schema: testSchema(t),
			typ: fidlgen.Type{
				Kind: fidlgen.VectorType,
				ElementType: &fidlgen.Type{
					Kind:          fidlgen.HandleType,
					HandleSubtype: fidlgen.HandleSubtypeEvent,
				},
			},
		},
		[]conformTest{
			conformOk{[]gidlir.Value{}},
			conformOk{[]gidlir.Value{defaultMetadataForHandle(0)}},
			conformOk{[]gidlir.Value{
				defaultMetadataForHandle(0),
				defaultMetadataForHandle(1),
			}},
			conformOk{[]gidlir.Value{
				defaultMetadataForHandle(1),
				defaultMetadataForHandle(0),
			}},
			// The parser is responsible for ensuring handles are used exactly
			// once, not the mixer, so this passes.
			conformOk{[]gidlir.Value{
				defaultMetadataForHandle(0),
				defaultMetadataForHandle(0),
			}},
			conformFail{[]gidlir.Value{uint64(0)}, "[0]: expecting handle"},
			conformFail{[]gidlir.Value{nil}, "[0]: expecting non-null handle"},
		},
	)
}
