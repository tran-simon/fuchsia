// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_E2E_TESTS_FUZZY_MATCHER_H_
#define SRC_DEVELOPER_DEBUG_E2E_TESTS_FUZZY_MATCHER_H_

#include <initializer_list>
#include <sstream>
#include <string>

namespace zxdb {

// A helper class to match output sequentially as |MatchesLine| is called repeatedly.
class FuzzyMatcher {
 public:
  explicit FuzzyMatcher(const std::string& content) : content_(content) {}

  // The content should contain a line that matches the given substrings.
  // Note that this method will consume the content, so a subsequent call may return differently.
  bool MatchesLine(std::initializer_list<std::string_view> substrs);

 private:
  std::stringstream content_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_E2E_TESTS_FUZZY_MATCHER_H_
