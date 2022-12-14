// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_FILTER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_FILTER_H_

#include "src/developer/debug/zxdb/console/output_buffer.h"

namespace zxdb {

class ConsoleContext;
class Filter;

OutputBuffer FormatFilter(const ConsoleContext* context, const Filter* filter);

// Formats the current filter list in a table.
OutputBuffer FormatFilterList(ConsoleContext* context, int indent = 0);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_FILTER_H_
