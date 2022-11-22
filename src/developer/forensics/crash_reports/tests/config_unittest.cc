// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/crash_reports/config.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/errors.h>

#include <gtest/gtest.h>

#include "src/lib/files/scoped_temp_dir.h"

namespace forensics {
namespace crash_reports {
namespace {

constexpr Config::UploadPolicy kDisabled = Config::UploadPolicy::kDisabled;
constexpr Config::UploadPolicy kEnabled = Config::UploadPolicy::kEnabled;
constexpr Config::UploadPolicy kReadFromPrivacySettings =
    Config::UploadPolicy::kReadFromPrivacySettings;

class ConfigTest : public testing::Test {
 protected:
  std::string WriteConfig(const std::string& config) {
    std::string path;
    FX_CHECK(tmp_dir_.NewTempFileWithData(config, &path));
    return path;
  }

 private:
  files::ScopedTempDir tmp_dir_;
};

// Parse |config_str| into |var| or assert if the operation fails.
#define PARSE_OR_ASSERT(var, config_str)                 \
  auto tmp_##var = ParseConfig(WriteConfig(config_str)); \
  ASSERT_TRUE(tmp_##var.has_value());                    \
  auto var = std::move(tmp_##var.value());

// Parse |config_str| and assert it is malformed.
#define ASSERT_IS_BAD_CONFIG(config_str) \
  ASSERT_FALSE(ParseConfig(WriteConfig(config_str)).has_value());

TEST_F(ConfigTest, MissingCrashReportUploadPolicy) {
  ASSERT_IS_BAD_CONFIG(R"({
})");
}

TEST_F(ConfigTest, BadCrashReportUploadPolicy) {
  ASSERT_IS_BAD_CONFIG(R"({
    "crash_report_upload_policy": "other"
})");
}

TEST_F(ConfigTest, SpruiousFields) {
  ASSERT_IS_BAD_CONFIG(R"({
    "crash_report_upload_policy": "disabled",
    "spurious": ""
})");
}

TEST_F(ConfigTest, UploadDisabled) {
  PARSE_OR_ASSERT(config, R"({
    "crash_report_upload_policy": "disabled"
})");
  EXPECT_EQ(config.crash_report_upload_policy, kDisabled);
}

TEST_F(ConfigTest, UploadEnabled) {
  PARSE_OR_ASSERT(config, R"({
    "crash_report_upload_policy": "enabled"
})");
  EXPECT_EQ(config.crash_report_upload_policy, kEnabled);
}

TEST_F(ConfigTest, UploadReadFromPrivacySettings) {
  PARSE_OR_ASSERT(config, R"({
    "crash_report_upload_policy": "read_from_privacy_settings"
})");
  EXPECT_EQ(config.crash_report_upload_policy, kReadFromPrivacySettings);
}

TEST_F(ConfigTest, MissingConfig) { ASSERT_FALSE(ParseConfig("undefined file").has_value()); }

}  // namespace

// Pretty-prints Config::UploadPolicy in gTest matchers instead of the default byte
// string in case of failed expectations.
void PrintTo(const Config::UploadPolicy& upload_policy, std::ostream* os) {
  *os << ToString(upload_policy);
}

#undef ASSERT_IS_BAD_CONFIG
#undef PARSE_OR_ASSERT

}  // namespace crash_reports
}  // namespace forensics
