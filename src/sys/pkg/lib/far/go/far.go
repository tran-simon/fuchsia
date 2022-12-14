// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package far implements Fuchsia archive operations. At this time only
// archive writing is supported. The specification for the archive format
// can be found in
// https://fuchsia.dev/fuchsia-src/concepts/storage/archive_format
package far

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

// Magic is the first bytes of a FAR archive
const Magic = "\xc8\xbf\x0b\x48\xad\xab\xc5\x11"

// ErrInvalidArchive is returned from reads when the archive is not of the expected format or is corrupted.
type ErrInvalidArchive string

func (e ErrInvalidArchive) Error() string {
	return fmt.Sprintf("far: archive is not valid. %s", string(e))
}

// ChunkType is a uint64 representing the type of a non-index chunk
type ChunkType uint64

// alignment values from the FAR specification
const (
	contentAlignment = 4096
	chunkAlignment   = 8
)

// Various chunk types
const (
	DirChunk      ChunkType = 0x2d2d2d2d2d524944 // "DIR-----"
	DirNamesChunk ChunkType = 0x53454d414e524944 // "DIRNAMES"
)

// Index is the first chunk of an archive
type Index struct {
	// Magic bytes must equal the Magic constant
	Magic [8]byte
	// Length of all index entries in bytes
	Length uint64
}

// IndexLen is the byte size of the Index struct
const IndexLen = 8 + 8

// IndexEntry identifies the type and position of a chunk in the archive
type IndexEntry struct {
	// Type of the chunk
	Type ChunkType
	// Offset from the start of the archive
	Offset uint64
	// Length of the chunk
	Length uint64
}

// IndexEntryLen is the byte size of the IndexEntry struct
const IndexEntryLen = 8 + 8 + 8

// DirectoryEntry indexes into the dirnames and contents chunks to provide
// access to file names and file contents respectively.
type DirectoryEntry struct {
	NameOffset uint32
	NameLength uint16
	Reserved   uint16
	DataOffset uint64
	DataLength uint64
	Reserved2  uint64
}

// DirectoryEntryLen is the byte size of the DirectoryEntry struct
const DirectoryEntryLen = 4 + 2 + 2 + 8 + 8 + 8

// PathData is a concatenated list of names of files that makes up the unpadded
// portion of a dirnames chunk.
type PathData []byte

// EntryReader allows reading the contents of an archive entry, file or directory.
type EntryReader struct {
	// Offset of this entry from the start of the archive
	Offset uint64
	// Length of this entry in the archive
	Length uint64

	Source io.ReaderAt
}

// Write writes a list of files to the given io. The inputs map provides a list
// of target archive paths mapped to on-disk file paths from which the content
// should be fetched.
func Write(w io.Writer, inputs map[string]string) error {
	var filenames = make([]string, 0, len(inputs))
	for name := range inputs {
		filenames = append(filenames, name)
	}
	sort.Strings(filenames)

	var pathData PathData
	var entries []DirectoryEntry
	for _, name := range filenames {
		bname := []byte(name)
		entries = append(entries, DirectoryEntry{
			NameOffset: uint32(len(pathData)),
			NameLength: uint16(len(bname)),
		})
		pathData = append(pathData, bname...)
	}

	index := Index{
		Length: IndexEntryLen * 2,
	}
	copy(index.Magic[:], []byte(Magic))

	dirIndex := IndexEntry{
		Type:   DirChunk,
		Offset: IndexLen + IndexEntryLen*2,
		Length: uint64(len(entries) * DirectoryEntryLen),
	}

	nameIndex := IndexEntry{
		Type:   DirNamesChunk,
		Offset: dirIndex.Offset + dirIndex.Length,
		Length: align(uint64(len(pathData)), 8),
	}

	if err := binary.Write(w, binary.LittleEndian, index); err != nil {
		return err
	}
	if err := binary.Write(w, binary.LittleEndian, dirIndex); err != nil {
		return err
	}
	if err := binary.Write(w, binary.LittleEndian, nameIndex); err != nil {
		return err
	}

	contentOffset := align(nameIndex.Offset+nameIndex.Length, contentAlignment)

	for i := range entries {
		entries[i].DataOffset = contentOffset
		n, err := fileSize(inputs[filenames[i]])
		if err != nil {
			return err
		}
		entries[i].DataLength = uint64(n)
		contentOffset = align(contentOffset+entries[i].DataLength, contentAlignment)

		if err := binary.Write(w, binary.LittleEndian, entries[i]); err != nil {
			return err
		}
	}

	if err := binary.Write(w, binary.LittleEndian, pathData); err != nil {
		return err
	}

	if _, err := w.Write(make([]byte, int(nameIndex.Length)-len(pathData))); err != nil {
		return err
	}

	pos := nameIndex.Offset + nameIndex.Length
	pad := align(pos, contentAlignment) - pos
	if _, err := w.Write(make([]byte, pad)); err != nil {
		return err
	}

	for i, name := range filenames {
		f, err := os.Open(inputs[name])
		if err != nil {
			return err
		}
		if _, err := io.Copy(w, f); err != nil {
			return err
		}
		if err := f.Close(); err != nil {
			return err
		}

		pos := entries[i].DataOffset + entries[i].DataLength
		pad := align(pos, contentAlignment) - pos
		if _, err := w.Write(make([]byte, pad)); err != nil {
			return err
		}
	}

	return nil
}

// Reader wraps an io.ReaderAt providing access to FAR contents from that io.
// It caches the directory and path information after it is read, and provides
// io.Readers for files contained in the archive.
type Reader struct {
	source       io.ReaderAt
	index        Index
	indexEntries []IndexEntry
	dirEntries   []DirectoryEntry
	pathData     PathData
}

// NewReader wraps the given io.ReaderAt and returns a struct that provides indexed access to the FAR contents.
func NewReader(s io.ReaderAt) (*Reader, error) {
	r := &Reader{source: s}
	if err := r.readIndex(); err != nil {
		return nil, err
	}
	return r, nil
}

// Close closes the underlying reader source, if it implements io.Closer
func (r *Reader) Close() error {
	if c, ok := r.source.(io.Closer); ok {
		return c.Close()
	}
	return nil
}

func (r *Reader) readIndex() error {
	buf := make([]byte, IndexLen)
	if _, err := r.source.ReadAt(buf, 0); err != nil {
		return err
	}
	copy(r.index.Magic[:], buf)
	r.index.Length = binary.LittleEndian.Uint64(buf[len(r.index.Magic):])

	if !bytes.Equal(r.index.Magic[:], []byte(Magic)) {
		return ErrInvalidArchive("bad magic")
	}
	if r.index.Length%IndexEntryLen != 0 {
		return ErrInvalidArchive("bad index length")
	}

	nentries := r.index.Length / IndexEntryLen
	r.indexEntries = make([]IndexEntry, nentries)

	if err := r.readEntries(); err != nil {
		return err
	}

	if err := r.verifyDirEntries(); err != nil {
		return err
	}

	return r.verifyContentChunks()
}

// readEntries reads index entries and directory information into the Reader struct.
func (r *Reader) readEntries() error {
	var dirIndex, dirNamesIndex *IndexEntry
	buf := make([]byte, IndexEntryLen)
	for i := range r.indexEntries {
		if _, err := r.source.ReadAt(buf, int64(IndexLen+(i*IndexEntryLen))); err != nil {
			return err
		}

		r.indexEntries[i].Type = ChunkType(binary.LittleEndian.Uint64(buf))
		r.indexEntries[i].Offset = binary.LittleEndian.Uint64(buf[8:])
		r.indexEntries[i].Length = binary.LittleEndian.Uint64(buf[16:])

		if i > 0 {
			if r.indexEntries[i-1].Type > r.indexEntries[i].Type {
				return ErrInvalidArchive(fmt.Sprintf("invalid index entry order, chunk type %x before chunk type %x", r.indexEntries[i-1].Type, r.indexEntries[i].Type))
			} else if r.indexEntries[i-1].Type == r.indexEntries[i].Type {
				return ErrInvalidArchive(fmt.Sprintf("duplicate chunk types of %x in index", r.indexEntries[i].Type))
			}
		}
		if r.indexEntries[i].Offset < r.index.Length {
			return ErrInvalidArchive("short offset")
		}

		// All chunks must be aligned on 64 bit boundaries.
		if (r.indexEntries[i].Offset % chunkAlignment) != 0 {
			return ErrInvalidArchive("chunk not aligned on an 8 byte boundary")
		}

		switch r.indexEntries[i].Type {
		case DirChunk:
			dirIndex = &r.indexEntries[i]
			if dirIndex.Length%DirectoryEntryLen != 0 {
				return ErrInvalidArchive("bad directory index")
			}
		case DirNamesChunk:
			dirNamesIndex = &r.indexEntries[i]
			// DirNamesChunk length must be a multiple of 8.
			if dirNamesIndex.Length%8 != 0 {
				return ErrInvalidArchive("dir names chunk length is not a multiple of 8")
			}
		}

		// Chunks must be tightly packed.
		var expectedOffset uint64
		if i == 0 {
			expectedOffset = IndexLen + r.index.Length
		} else {
			prev := r.indexEntries[i-1]
			expectedOffset = prev.Offset + prev.Length
		}
		expectedOffset = align(expectedOffset, chunkAlignment)
		if r.indexEntries[i].Offset != expectedOffset {
			return ErrInvalidArchive(fmt.Sprintf("chunk violates the tightly packed constraint: expected offset: %x, actual offset: %x", expectedOffset, r.indexEntries[i].Offset))
		}
	}

	if dirIndex == nil || dirNamesIndex == nil {
		return ErrInvalidArchive("missing required chunk")
	}

	buf = make([]byte, dirIndex.Length)
	if _, err := r.source.ReadAt(buf, int64(dirIndex.Offset)); err != nil {
		return err
	}
	r.dirEntries = make([]DirectoryEntry, dirIndex.Length/DirectoryEntryLen)
	// TODO(raggi): eradicate copies, etc.
	if err := binary.Read(bytes.NewReader(buf), binary.LittleEndian, &r.dirEntries); err != nil {
		return err
	}

	r.pathData = make([]byte, dirNamesIndex.Length)
	if _, err := r.source.ReadAt(r.pathData, int64(dirNamesIndex.Offset)); err != nil {
		return err
	}

	return nil
}

// verifyDirEntries verifies directories and path compliance.
func (r *Reader) verifyDirEntries() error {
	for i, cur := range r.dirEntries {
		cs := cur.NameOffset
		ce := cs + uint32(cur.NameLength)
		if ce > uint32(len(r.pathData)) {
			return ErrInvalidArchive("invalid dir name length")
		}
		if err := validateName(r.pathData[cs:ce]); err != nil {
			return err
		}
		// Verify lexicographical order of dir name strings.
		if i == 0 {
			continue
		}
		prev := r.dirEntries[i-1]
		ps := prev.NameOffset
		pe := ps + uint32(prev.NameLength)
		if strings.Compare(string(r.pathData[cs:ce]), string(r.pathData[ps:pe])) != 1 {
			return ErrInvalidArchive("invalid order of dir names")
		}
	}

	return nil
}

// verifyContentChunks verifies alignment and packing of content chunks.
func (r *Reader) verifyContentChunks() error {
	for i, cur := range r.dirEntries {
		cs := cur.DataOffset
		if (cs % contentAlignment) != 0 {
			return ErrInvalidArchive(fmt.Sprintf("content chunk at index %v not aligned on a 4096 byte boundary", i))
		}

		if i == 0 {
			// Find the start of the first content chunk and verify packing.
			// Note that this access is safe because prior index verification has
			// ensured that there are at least two entries.
			li := r.indexEntries[len(r.indexEntries)-1]
			expectedOffset := align(li.Offset+li.Length, contentAlignment)
			if expectedOffset != cs {
				return ErrInvalidArchive(fmt.Sprintf("first content chunk violates the tightly packed constraint: expected offset: 0x%x, actual offset: 0x%x", expectedOffset, cs))
			}
		} else {
			// Verify packing and ordering versus the previous content chunk.
			prev := r.dirEntries[i-1]
			ps := prev.DataOffset
			pe := ps + prev.DataLength
			if pe > cs {
				return ErrInvalidArchive(fmt.Sprintf("content chunk at index %v starts before the previous chunk ends", i))
			}
			expectedOffset := align(pe, contentAlignment)
			if cs != expectedOffset {
				return ErrInvalidArchive(fmt.Sprintf("content chunk violates the tightly packed constraint: expected offset: 0x%x, actual offset: 0x%x", expectedOffset, cs))
			}
		}

	}

	// Ensure the last content chunk does not extend beyond the end of the file.
	if len(r.dirEntries) != 0 {
		le := r.dirEntries[len(r.dirEntries)-1]
		expectedEnd := align(le.DataOffset+le.DataLength, contentAlignment)
		buf := make([]byte, 1)
		if _, err := r.source.ReadAt(buf, int64(expectedEnd)-1); err != nil {
			return ErrInvalidArchive("last content chunk extends beyond end of file")
		}
	}

	return nil
}

// List provides the list of all file names in the archive
func (r *Reader) List() []string {
	var names = make([]string, 0, len(r.dirEntries))
	for i := range r.dirEntries {
		de := &r.dirEntries[i]
		names = append(names, string(r.pathData[de.NameOffset:de.NameOffset+uint32(de.NameLength)]))
	}
	return names
}

func (r *Reader) openEntry(de *DirectoryEntry) *EntryReader {
	return &EntryReader{de.DataOffset, de.DataLength, r.source}
}

// Open finds the file in the archive and returns an EntryReader that can read the contents
func (r *Reader) Open(path string) (*EntryReader, error) {
	bpath := []byte(path)
	for i := range r.dirEntries {
		de := &r.dirEntries[i]
		if bytes.Equal(bpath, r.pathData[de.NameOffset:de.NameOffset+uint32(de.NameLength)]) {
			return r.openEntry(de), nil
		}
	}
	return nil, &os.PathError{Op: "open", Path: path, Err: os.ErrNotExist}
}

// ReadFile reads a whole file out of the archive
func (r *Reader) ReadFile(path string) ([]byte, error) {
	bpath := []byte(path)
	for i := range r.dirEntries {
		de := &r.dirEntries[i]
		if bytes.Equal(bpath, r.pathData[de.NameOffset:de.NameOffset+uint32(de.NameLength)]) {
			buf := make([]byte, de.DataLength)
			_, err := r.source.ReadAt(buf, int64(de.DataOffset))
			return buf, err
		}
	}
	return nil, os.ErrNotExist
}

func (r *Reader) GetSize(path string) uint64 {
	bpath := []byte(path)
	for i := range r.dirEntries {
		de := &r.dirEntries[i]
		if bytes.Equal(bpath, r.pathData[de.NameOffset:de.NameOffset+uint32(de.NameLength)]) {
			return de.DataLength
		}
	}
	return 0
}

// IsFAR looks for the FAR magic header, returning true if it is found. Only the header is consumed from the given input. If any IO error occurs, false is returned.
func IsFAR(r io.Reader) bool {
	m := make([]byte, len(Magic))
	_, err := io.ReadFull(r, m)
	if err != nil {
		return false
	}
	return bytes.Equal(m, []byte(Magic))
}

func (e *EntryReader) ReadAt(buf []byte, offset int64) (int, error) {
	if offset >= int64(e.Length) || offset < 0 {
		return 0, io.EOF
	}

	// clamp the read request to the top of the range
	max := int(e.Length - uint64(offset))
	if max > len(buf) {
		max = len(buf)
	}

	return e.Source.ReadAt(buf[:max], int64(e.Offset+uint64(offset)))
}

// align rounds i up to a multiple of n
func align(i, n uint64) uint64 {
	n--
	return (i + n) & ^n
}

func fileSize(path string) (int64, error) {
	info, err := os.Stat(path)
	if err != nil {
		return 0, err
	}
	return info.Size(), nil
}

// validateName checks the argument for compliance to the FAR archive spec.
func validateName(n []byte) error {
	if len(n) == 0 {
		return ErrInvalidArchive("name has zero length")
	}
	if n[0] == '/' {
		return ErrInvalidArchive("name must not start with '/'")

	}
	if n[len(n)-1] == '/' {
		return ErrInvalidArchive("name must not end with '/'")
	}

	// States for the parser
	const (
		empty = iota
		dot
		dotdot
		other
	)

	state := empty

	for _, c := range n {
		switch c {
		case 0x0:
			return ErrInvalidArchive("name contains a null byte")
		case '/':
			switch state {
			case empty:
				return ErrInvalidArchive("name contains empty segment")
			case dot:
				return ErrInvalidArchive("name contains '.' segment")
			case dotdot:
				return ErrInvalidArchive("name contains '..' segment")
			default:
				state = empty
			}
		case '.':
			switch state {
			case empty:
				state = dot
			case dot:
				state = dotdot
			default:
				state = other
			}
		default:
			state = other
		}
	}

	switch state {
	case dot:
		return ErrInvalidArchive("name contains '.' segment")
	case dotdot:
		return ErrInvalidArchive("name contains '..' segment")
	}

	return nil
}
