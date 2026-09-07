/* Shared package fixture: the real runtime still parses and authorizes every call. */
#ifndef ASTOOLS_TEST_LSP_FIXTURE_H
#define ASTOOLS_TEST_LSP_FIXTURE_H
#include "lsp.h"
#include "fakes.h"
typedef struct {
  char root[256], file[512], config[512], args[768];
  astools_ctx *c;
} lsp_fixture;
static int lsp_setup(lsp_fixture *f, const char *binary, const char *mode, const char *source,
                     const char *level) {
  memset(f, 0, sizeof *f);
  if (!astools_test_tmpdir(f->root)) return 0;
  astools_buf commands = {0};
  const char *names[] = {"symbols", "definition", "references", "diagnostics"};
  for (size_t i = 0; i < 4; i++) {
    astools_buf_printf(
        &commands,
        "%s#command {name:\"%s\",summary:\"Query exact "
        "source\",annotations:{read_only:true},params:["
        "#param "
        "{name:\"path\",required:true,type:#type{kind:\"path\",access:\"read\",must_exist:true}},"
        "#param {name:\"sha256\",required:true,type:#type{kind:\"string\",min_len:64,max_len:64}}",
        i ? "," : "", names[i]);
    if (i == 1 || i == 2)
      astools_buf_appends(
          &commands,
          ",#param {name:\"line\",required:true,type:#type{kind:\"integer\",min:1,max:2147483647}},"
          "#param "
          "{name:\"column\",required:true,type:#type{kind:\"integer\",min:1,max:2147483647}}");
    astools_buf_appends(&commands, "]}");
  }
  int ok = fake_registry_write(
      f->root, "lsp", binary, mode,
      "fs:[{path:\"${workspace}\",access:\"read\"}],net:false,proc:false,env:[]", commands.data,
      NULL);
  astools_buf_free(&commands);
  if (!ok) return 0;
  char path[512], *text = NULL;
  size_t n = 0;
  snprintf(path, sizeof path, "%s/lsp/manifest.xcdn", f->root);
  if (os_read_file(path, &text, &n) != ASTOOLS_OK) return 0;
  char *start = strstr(text, "mode: \"oneshot\"");
  astools_buf b = {0};
  ok = start && astools_buf_append(&b, text, (size_t)(start - text)) == ASTOOLS_OK &&
       astools_buf_appends(&b, "mode: \"persistent\", protocol: \"lsp\"") == ASTOOLS_OK &&
       astools_buf_appends(&b, start + strlen("mode: \"oneshot\"")) == ASTOOLS_OK &&
       os_write_file(path, b.data, b.len) == ASTOOLS_OK;
  astools_buf_free(&b);
  free(text);
  if (!ok) return 0;
  snprintf(f->file, sizeof f->file, "%s/épreuve.cpp", f->root);
  snprintf(f->config, sizeof f->config, "%s/config.xcdn", f->root);
  if (os_write_file(f->file, source, strlen(source)) != ASTOOLS_OK) return 0;
  char extra[256];
  snprintf(extra, sizeof extra, "sandbox:{default_level:\"%s\",strict_fallback:\"reject\"},",
           level);
  if (!fake_config_write(f->config, f->root, 1, f->root, extra)) return 0;
  astools_open_params p = {0};
  p.config_path = f->config;
  if (astools_open(&p, &f->c) != ASTOOLS_OK) return 0;
  uint8_t hash[32];
  char hex[65];
  astools_sha256(source, strlen(source), hash);
  for (size_t i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
  snprintf(f->args, sizeof f->args, "{path:\"épreuve.cpp\",sha256:\"%s\"", hex);
  return 1;
}
static void lsp_drop(lsp_fixture *f) {
  astools_close(f->c);
  astools_test_rmtree(f->root);
}
static astools_err lsp_call(lsp_fixture *f, const char *command, const char *position, int ms,
                            jx_value **out) {
  char args[1024];
  snprintf(args, sizeof args, "%s%s}", f->args, position ? position : "");
  astools_result result = {0};
  astools_err e = astools_invoke(f->c, "lsp", command, args, ms, &result);
  *out = NULL;
  if (e == ASTOOLS_OK && (!result.ok || !result.result_xcdn ||
                          jx_parse(result.result_xcdn, strlen(result.result_xcdn), out)))
    e = ASTOOLS_ERR_PROTOCOL;
  if (e != ASTOOLS_OK)
    fprintf(stderr, "LSP %s returned %s (%s): %s\n", command, astools_err_name(e),
            result.error_code ? result.error_code : "",
            result.error_message ? result.error_message : "");
  astools_result_free(&result);
  return e;
}
#endif
