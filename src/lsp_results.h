#ifndef ASTOOLS_LSP_RESULTS_H
#define ASTOOLS_LSP_RESULTS_H
#include "lsp.h"
#define LSP_FILES 16u

typedef struct {
  astools_ctx *ctx;
  const astools_effective *grants;
  int64_t deadline;
  astools_task *cancel;
  astools_lsp_file files[LSP_FILES];
  size_t count, excluded;
  int truncated;
  jx_value *items;
} astools_lsp_view;

astools_err astools_lsp_render(astools_lsp_view *view, const char *command, const jx_value *reply);
astools_err astools_lsp_recheck(astools_lsp_view *view);
void astools_lsp_view_free(astools_lsp_view *view);
#endif
