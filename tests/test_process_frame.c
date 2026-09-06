#include "astools_test.h"
#include "execution.h"

static int rejects(const char *text, size_t cap, astools_err expected) {
  astools_buf b;
  char *out = NULL;
  size_t len = 0;
  astools_buf_init(&b);
  if (astools_buf_appends(&b, text) != ASTOOLS_OK) return 0;
  astools_err e = astools_pp_take_frame(&b, cap, &out, &len);
  int ok = e == expected && !out && !len;
  free(out);
  astools_buf_free(&b);
  return ok;
}

TEST(fragmented_and_consecutive_frames) {
  const char *wire = "Content-Length: 4\r\n\r\n\"α\"";
  astools_buf b;
  char *out = NULL;
  size_t len = 0;
  astools_buf_init(&b);
  for (size_t i = 0; i < strlen(wire); i++) {
    ASSERT_EQ_INT(astools_buf_appendc(&b, wire[i]), ASTOOLS_OK);
    astools_err e = astools_pp_take_frame(&b, 4, &out, &len);
    ASSERT_EQ_INT(e, i + 1 == strlen(wire) ? ASTOOLS_OK : ASTOOLS_ERR_BUSY);
  }
  ASSERT_EQ_INT(len, 4);
  ASSERT_EQ_STR(out, "\"α\"");
  free(out);
  ASSERT_EQ_INT(b.len, 0);
  ASSERT_EQ_INT(astools_buf_appends(&b, "Content-Length: 2\r\n\r\n{}Content-Length: 2\r\n\r\n[]"),
                ASTOOLS_OK);
  ASSERT_EQ_INT(astools_pp_take_frame(&b, 2, &out, &len), ASTOOLS_OK);
  ASSERT_EQ_STR(out, "{}");
  free(out);
  ASSERT_EQ_INT(astools_pp_take_frame(&b, 2, &out, &len), ASTOOLS_OK);
  ASSERT_EQ_STR(out, "[]");
  free(out);
  ASSERT_EQ_INT(b.len, 0);
  astools_buf_free(&b);
}

TEST(ambiguous_lengths_and_encodings_rejected) {
  ASSERT_TRUE(
      rejects("Content-Length: 2\r\ncontent-length: 2\r\n\r\n{}", 100, ASTOOLS_ERR_PROTOCOL));
  ASSERT_TRUE(rejects("Content-Length: -2\r\n\r\n{}", 100, ASTOOLS_ERR_PROTOCOL));
  ASSERT_TRUE(rejects("Content-Length: +2\r\n\r\n{}", 100, ASTOOLS_ERR_PROTOCOL));
  ASSERT_TRUE(rejects("Content-Length: 2.0\r\n\r\n{}", 100, ASTOOLS_ERR_PROTOCOL));
  ASSERT_TRUE(rejects("Content-Length: 0\r\n\r\n", 100, ASTOOLS_ERR_PROTOCOL));
  ASSERT_TRUE(rejects("Other: 2\r\n\r\n{}", 100, ASTOOLS_ERR_PROTOCOL));
  ASSERT_TRUE(rejects("Content-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}", 100,
                      ASTOOLS_ERR_PROTOCOL));
  ASSERT_TRUE(
      rejects("Content-Length: 99999999999999999999999999999\r\n\r\n", 100, ASTOOLS_ERR_TOOL));
  ASSERT_TRUE(rejects("Content-Length: 101\r\n\r\n", 100, ASTOOLS_ERR_TOOL));
  ASSERT_TRUE(rejects("Content-Length: 100\r\n\r\n", 100, ASTOOLS_ERR_BUSY));
}

TEST(header_caps_and_supported_type) {
  astools_buf b;
  char *out = NULL;
  size_t len = 0;
  astools_buf_init(&b);
  for (size_t i = 0; i < 8192; i++) ASSERT_EQ_INT(astools_buf_appendc(&b, 'a'), ASTOOLS_OK);
  ASSERT_EQ_INT(astools_pp_take_frame(&b, 100, &out, &len), ASTOOLS_ERR_PROTOCOL);
  astools_buf_free(&b);
  astools_buf_init(&b);
  ASSERT_EQ_INT(
      astools_buf_appends(
          &b,
          "content-type: application/vscode-jsonrpc; charset=utf-8\r\nCONTENT-LENGTH: 2\r\n\r\n{}"),
      ASTOOLS_OK);
  ASSERT_EQ_INT(astools_pp_take_frame(&b, 2, &out, &len), ASTOOLS_OK);
  ASSERT_EQ_STR(out, "{}");
  free(out);
  astools_buf_free(&b);
}

TEST_LIST = {
    TEST_ENTRY(fragmented_and_consecutive_frames),
    TEST_ENTRY(ambiguous_lengths_and_encodings_rejected),
    TEST_ENTRY(header_caps_and_supported_type),
};
RUN_ALL_TESTS()
