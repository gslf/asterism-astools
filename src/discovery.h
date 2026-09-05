#ifndef ASTOOLS_DISCOVERY_H
#define ASTOOLS_DISCOVERY_H
#include "astools_internal.h"
#define ASTOOLS_SELECTION_MAX 64
typedef struct {
  astools_tool *tool;
  const astools_cmd *command;
} astools_command_view;
struct astools_selection {
  astools_ctx *ctx; /* context outlives selection and submitted tasks */
  uint64_t revision;
  size_t count, candidates, omitted;
  astools_command_view views[ASTOOLS_SELECTION_MAX];
  astools_selected_command commands[ASTOOLS_SELECTION_MAX];
  char *catalog, *grammar, *schemas;
};
astools_err astools_catalog_commands(const astools_command_view *commands, size_t count,
                                     astools_catalog_level level, char **out);
astools_err astools_gbnf_commands(const astools_command_view *commands, size_t count, char **out);
#endif
