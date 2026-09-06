/* Private semantic adapter. No method here grants authority from server metadata. */
#ifndef ASTOOLS_LSP_H
#define ASTOOLS_LSP_H
#include "execution.h"
#include "json.h"
#define LSP_FILE_BYTES (1024u * 1024u)
#define LSP_RESULTS 128u
#define LSP_SYMBOLS 1u
#define LSP_DEFINITION 2u
#define LSP_REFERENCES 4u

typedef struct {
  char *path, *uri, *text;
  size_t len;
  char sha256[65];
} astools_lsp_file;

char *astools_lsp_uri(const char *path);
char *astools_lsp_path(const char *uri);
astools_err astools_lsp_file_read(const char *root, const char *path,
                                  const astools_effective *grants, astools_lsp_file *out);
void astools_lsp_file_free(astools_lsp_file *file);
int astools_lsp_offset(const astools_lsp_file *file, const jx_value *position, size_t *out);
const char *astools_lsp_string(const jx_value *object, const char *key);
/* send/request take ownership of params, including on failure. */
astools_err astools_lsp_send(astools_ctx *c, astools_pproc *p, int id, const char *method,
                             jx_value *params, int64_t deadline, astools_task *cancel);
astools_err astools_lsp_receive(astools_ctx *c, astools_pproc *p, int id, const char *uri,
                                int version, int64_t deadline, astools_task *cancel,
                                jx_value **out);
astools_err astools_lsp_request(astools_ctx *c, astools_pproc *p, const char *method,
                                jx_value *params, int64_t deadline, astools_task *cancel,
                                jx_value **out);
astools_err astools_lsp_initialize(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                   astools_task *cancel);
astools_err astools_lsp_invoke(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                               const xcdn_node_t *args, const astools_effective *grants,
                               int64_t deadline, astools_task *cancel, astools_result *result);
#endif
