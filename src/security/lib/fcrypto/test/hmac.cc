// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/security/lib/fcrypto/hmac.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <zxtest/zxtest.h>

#include "src/security/lib/fcrypto/bytes.h"
#include "src/security/lib/fcrypto/digest.h"
#include "src/security/lib/fcrypto/secret.h"
#include "src/security/lib/fcrypto/test/utils.h"

namespace crypto {
namespace testing {
namespace {

TEST(HMAC, Init) {
  HMAC hmac;
  Secret key;

  size_t key_len;
  ASSERT_OK(GetDigestLen(digest::kSHA256, &key_len));

  // Bad digest
  EXPECT_STATUS(hmac.Init(digest::kUninitialized, key), ZX_ERR_INVALID_ARGS);

  // Bad flags
  EXPECT_STATUS(hmac.Init(digest::kSHA256, key, 0x8000), ZX_ERR_INVALID_ARGS);

  // Short key
  ASSERT_OK(key.Generate(key_len - 1));
  EXPECT_STATUS(hmac.Init(digest::kSHA256, key), ZX_ERR_INVALID_ARGS);

  // Medium key
  ASSERT_OK(key.Generate(key_len));
  EXPECT_OK(hmac.Init(digest::kSHA256, key));

  // Long key
  ASSERT_OK(key.Generate(zx_system_get_page_size()));
  EXPECT_OK(hmac.Init(digest::kSHA256, key));
}

TEST(HMAC, Update) {
  HMAC hmac;

  size_t key_len;
  Secret key;
  Bytes buf;
  ASSERT_OK(GetDigestLen(digest::kSHA256, &key_len));
  ASSERT_OK(key.Generate(key_len));
  ASSERT_OK(buf.Randomize(zx_system_get_page_size()));

  // Uninitialized
  EXPECT_STATUS(hmac.Update(buf.get(), buf.len()), ZX_ERR_BAD_STATE);

  // Null data
  ASSERT_OK(hmac.Init(digest::kSHA256, key));
  EXPECT_OK(hmac.Update(nullptr, 0));
  EXPECT_STATUS(hmac.Update(nullptr, buf.len()), ZX_ERR_INVALID_ARGS);

  // Multiple calls
  EXPECT_OK(hmac.Update(buf.get(), buf.len()));
  EXPECT_OK(hmac.Update(buf.get(), buf.len()));
}

TEST(HMAC, Final) {
  HMAC hmac;

  size_t key_len;
  Secret key;
  Bytes buf;
  ASSERT_OK(GetDigestLen(digest::kSHA256, &key_len));
  ASSERT_OK(key.Generate(key_len));
  ASSERT_OK(buf.Randomize(zx_system_get_page_size()));

  // Uninitialized
  Bytes out;
  EXPECT_STATUS(hmac.Final(&out), ZX_ERR_BAD_STATE);

  // Bad parameter
  ASSERT_OK(hmac.Init(digest::kSHA256, key));
  EXPECT_STATUS(hmac.Final(nullptr), ZX_ERR_INVALID_ARGS);

  // No update
  EXPECT_OK(hmac.Final(&out));

  // No update after final without init
  EXPECT_STATUS(hmac.Update(buf.get(), buf.len()), ZX_ERR_BAD_STATE);

  // With update
  ASSERT_OK(hmac.Init(digest::kSHA256, key));
  ASSERT_OK(hmac.Update(buf.get(), buf.len()));
  EXPECT_OK(hmac.Final(&out));
}

TEST(HMAC, Create) {
  Secret key;
  Bytes digest1, digest2, block1, block2;

  ASSERT_OK(block1.Randomize(zx_system_get_page_size()));

  size_t key_len;
  ASSERT_OK(GetDigestLen(digest::kSHA256, &key_len));

  // Bad parameters
  EXPECT_STATUS(
      HMAC::Create(digest::kUninitialized, key, block1.get(), zx_system_get_page_size(), &digest1),
      ZX_ERR_INVALID_ARGS);
  ASSERT_OK(key.Generate(key_len - 1));
  EXPECT_STATUS(
      HMAC::Create(digest::kSHA256, key, block1.get(), zx_system_get_page_size(), &digest1),
      ZX_ERR_INVALID_ARGS);
  ASSERT_OK(key.Generate(key_len));
  EXPECT_STATUS(HMAC::Create(digest::kSHA256, key, nullptr, zx_system_get_page_size(), &digest1),
                ZX_ERR_INVALID_ARGS);
  EXPECT_STATUS(
      HMAC::Create(digest::kSHA256, key, block1.get(), zx_system_get_page_size(), nullptr),
      ZX_ERR_INVALID_ARGS);

  // Same blocks, same HMACs
  ASSERT_OK(block2.Copy(block1));
  EXPECT_OK(HMAC::Create(digest::kSHA256, key, block1.get(), zx_system_get_page_size(), &digest1));
  EXPECT_OK(HMAC::Create(digest::kSHA256, key, block2.get(), zx_system_get_page_size(), &digest2));
  EXPECT_TRUE(digest1 == digest2);

  // Different blocks, different HMACs
  block2.get()[0] ^= 1;
  EXPECT_OK(HMAC::Create(digest::kSHA256, key, block2.get(), zx_system_get_page_size(), &digest2));
  EXPECT_TRUE(digest1 != digest2);
}

TEST(HMAC, Verify) {
  Secret key;
  Bytes out, block;

  ASSERT_OK(block.Randomize(zx_system_get_page_size()));

  size_t key_len;
  ASSERT_OK(GetDigestLen(digest::kSHA256, &key_len));

  // Bad parameters
  EXPECT_STATUS(
      HMAC::Verify(digest::kUninitialized, key, block.get(), zx_system_get_page_size(), out),
      ZX_ERR_INVALID_ARGS);
  ASSERT_OK(key.Generate(key_len - 1));
  EXPECT_STATUS(HMAC::Verify(digest::kSHA256, key, block.get(), zx_system_get_page_size(), out),
                ZX_ERR_INVALID_ARGS);
  ASSERT_OK(key.Generate(key_len));
  EXPECT_STATUS(HMAC::Verify(digest::kSHA256, key, nullptr, zx_system_get_page_size(), out),
                ZX_ERR_INVALID_ARGS);
  ASSERT_OK(key.Generate(key_len));
  EXPECT_STATUS(HMAC::Verify(digest::kSHA256, key, block.get(), zx_system_get_page_size(), out),
                ZX_ERR_INVALID_ARGS);

  // Verify valid
  ASSERT_OK(key.Generate(key_len));
  ASSERT_OK(HMAC::Create(digest::kSHA256, key, block.get(), zx_system_get_page_size(), &out));
  EXPECT_OK(HMAC::Verify(digest::kSHA256, key, block.get(), zx_system_get_page_size(), out));

  // Verify invalid
  block.get()[0] ^= 1;
  EXPECT_STATUS(HMAC::Verify(digest::kSHA256, key, block.get(), zx_system_get_page_size(), out),
                ZX_ERR_IO_DATA_INTEGRITY);
  block.get()[0] ^= 1;
  EXPECT_OK(HMAC::Verify(digest::kSHA256, key, block.get(), zx_system_get_page_size(), out));

  ASSERT_OK(key.Generate(key_len));
  EXPECT_STATUS(HMAC::Verify(digest::kSHA256, key, block.get(), zx_system_get_page_size(), out),
                ZX_ERR_IO_DATA_INTEGRITY);
}

// The following tests are taken from RFC 4231 section 4.  They are intentionally formatted to be as
// close to the RFC's representation as possible.
void TestRfc4231_TC(const char* xkey, const char* xdata, const char* xhmac) {
  Secret key;
  Bytes data, hmac;
  ASSERT_OK(HexToSecret(xkey, &key));
  ASSERT_OK(HexToBytes(xdata, &data));
  ASSERT_OK(HexToBytes(xhmac, &hmac));
  EXPECT_OK(HMAC::Verify(digest::kSHA256, key, data.get(), data.len(), hmac,
                         HMAC::ALLOW_WEAK_KEY | HMAC::ALLOW_TRUNCATION));
}

// clang-format off
TEST(Rfc4231, TC1) {
  ASSERT_NO_FATAL_FAILURE(TestRfc4231_TC(
    /* Key */    "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"
                 "0b0b0b0b",                        // 20 bytes
    /* Data */   "4869205468657265",                // "Hi There"
    /* SHA256 */ "b0344c61d8db38535ca8afceaf0bf12b"
                 "881dc200c9833da726e9376c2e32cff7"));
}

TEST(Rfc4231, TC2) {
  ASSERT_NO_FATAL_FAILURE(TestRfc4231_TC(
    /* Key */    "4a656665",                        // "Jefe"
    /* Data */   "7768617420646f2079612077616e7420" // "what do ya want "
                 "666f72206e6f7468696e673f",        // "for nothing?"
    /* SHA256 */ "5bdcc146bf60754e6a042426089575c7"
                 "5a003f089d2739839dec58b964ec3843"));
}

TEST(Rfc4231, TC3) {
  ASSERT_NO_FATAL_FAILURE(TestRfc4231_TC(
    /* Key */    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaa",                        // 20 bytes
    /* Data */   "dddddddddddddddddddddddddddddddd"
                 "dddddddddddddddddddddddddddddddd"
                 "dddddddddddddddddddddddddddddddd"
                 "dddd",                            // 50 bytes
    /* SHA256 */ "773ea91e36800e46854db8ebd09181a7"
                 "2959098b3ef8c122d9635514ced565fe"));
}

TEST(Rfc4231, TC4) {
  ASSERT_NO_FATAL_FAILURE(TestRfc4231_TC(
    /* Key */    "0102030405060708090a0b0c0d0e0f10"
                 "111213141516171819",              // 25 bytes
    /* Data */   "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
                 "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
                 "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
                 "cdcd",                            // 50 bytes
    /* SHA256 */ "82558a389a443c0ea4cc819899f2083a"
                 "85f0faa3e578f8077a2e3ff46729665b"));
}

TEST(Rfc4231, TC5) {
  ASSERT_NO_FATAL_FAILURE(TestRfc4231_TC(
    /* Key */
    /* Key */    "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c"
                 "0c0c0c0c",                        // 20 bytes
    /* Data */   "546573742057697468205472756e6361" // "Test With Trunca"
                 "74696f6e",                        // "tion"
    /* SHA256 */ "a3b6167473100ee06e0c796c2955552b"));
}

TEST(Rfc4231, TC6) {
  ASSERT_NO_FATAL_FAILURE(TestRfc4231_TC(
    /* Key */    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaa",                          // 131 bytes
    /* Data */   "54657374205573696e67204c61726765" // "Test Using Large"
                 "72205468616e20426c6f636b2d53697a" // "r Than Block-Siz"
                 "65204b6579202d2048617368204b6579" // "e Key - Hash Key"
                 "204669727374",                    // " First"
    /* SHA256 */ "60e431591ee0b67f0d8a26aacbf5b77f"
                 "8e0bc6213728c5140546040f0ee37f54"));
}

TEST(Rfc4231, TC7) {
  ASSERT_NO_FATAL_FAILURE(TestRfc4231_TC(
    /* Key */    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaa",                          // 131 bytes
    /* Data */   "54686973206973206120746573742075" // "This is a test u"
                 "73696e672061206c6172676572207468" // "sing a larger th"
                 "616e20626c6f636b2d73697a65206b65" // "an block-size ke"
                 "7920616e642061206c61726765722074" // "y and a larger t"
                 "68616e20626c6f636b2d73697a652064" // "han block-size d"
                 "6174612e20546865206b6579206e6565" // "ata. The key nee"
                 "647320746f2062652068617368656420" // "ds to be hashed "
                 "6265666f7265206265696e6720757365" // "before being use"
                 "642062792074686520484d414320616c" // "d by the HMAC al"
                 "676f726974686d2e",                // "gorithm."
    /* SHA256 */ "9b09ffa71b942fcb27635fbcd5b0e944"
                 "bfdc63644f0713938a7f51535c3a35e2"));
}  // clang-format on

}  // namespace
}  // namespace testing
}  // namespace crypto
