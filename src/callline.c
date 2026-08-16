/*
 * callline.c — CALL/RESULT/ERROR line handling.
 *
 * Parsing: model output is scanned line by line; after optional leading
 * spaces/tabs a line must start with the exact word "CALL " to count.
 * The ref token "<slug>[@<semverish>].<slug>" is split at its LAST dot
 * because versions may themselves contain dots while command slugs
 * cannot. The args object is a single-line balanced '{...}' tracked with
 * quote state ('"' toggles, '\\' escapes inside strings; braces counted
 * outside strings — typed literals like r"PT5S" are plain quotes after a
 * letter, which the toggle handles). Trailing text after the closing '}'
 * on the same line is free prose and ignored. The first well-formed call
 * line wins; malformed CALL lines are skipped.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expr)                                                            \
  do {                                                                       \
    astools_err try_e_ = (expr);                                             \
    if (try_e_ != ASTOOLS_OK) return try_e_;                                 \
  } while (0)

/* ---- CALL parsing ------------------------------------------------------- */

static bool slug_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
}

static bool ver_char(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
         (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '+' || ch == '-';
}

static bool ref_token_char(char ch) { return ver_char(ch) || ch == '@'; }

typedef struct {
  const char *ref; /* includes "@version" when present */
  size_t ref_len;
  const char *cmd;
  size_t cmd_len;
  const char *args; /* the balanced object, braces included */
  size_t args_len;
} call_spans;

/* One line, [line, line+len). true when it is a well-formed call line. */
static bool parse_call_line(const char *line, size_t len, call_spans *out) {
  size_t i = 0, t0, tlen, dot, k;
  int depth = 0;
  bool in_str = false, esc = false;

  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  if (len - i < 5 || memcmp(line + i, "CALL ", 5) != 0) return false;
  i += 5;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

  t0 = i;
  while (i < len && ref_token_char(line[i])) i++;
  tlen = i - t0;
  if (tlen == 0) return false;

  /* split "<tool>[@<version>].<command>" at the LAST dot */
  dot = tlen;
  for (k = tlen; k > 0; k--) {
    if (line[t0 + k - 1] == '.') {
      dot = k - 1;
      break;
    }
  }
  if (dot == tlen || dot == 0 || dot + 1 == tlen) return false;
  for (k = dot + 1; k < tlen; k++)
    if (!slug_char(line[t0 + k])) return false;
  {
    size_t at = dot;
    for (k = 0; k < dot; k++) {
      if (line[t0 + k] == '@') {
        at = k;
        break;
      }
    }
    if (at == 0) return false; /* empty tool id */
    for (k = 0; k < at; k++)
      if (!slug_char(line[t0 + k])) return false;
    if (at < dot) {
      if (at + 1 == dot) return false; /* empty version */
      for (k = at + 1; k < dot; k++)
        if (!ver_char(line[t0 + k])) return false;
    }
  }

  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i >= len || line[i] != '{') return false;
  out->args = line + i;
  for (; i < len; i++) {
    char ch = line[i];
    if (in_str) {
      if (esc)
        esc = false;
      else if (ch == '\\')
        esc = true;
      else if (ch == '"')
        in_str = false;
    } else if (ch == '"') {
      in_str = true;
    } else if (ch == '{') {
      depth++;
    } else if (ch == '}') {
      depth--;
      if (depth == 0) {
        out->args_len = (size_t)(line + i + 1 - out->args);
        out->ref = line + t0;
        out->ref_len = dot;
        out->cmd = line + t0 + dot + 1;
        out->cmd_len = tlen - dot - 1;
        return true; /* trailing prose after '}' is ignored */
      }
    }
  }
  return false; /* object not closed on this line */
}

astools_err astools_callline_parse(const char *model_output, char **out_ref,
                                   char **out_command, char **out_args) {
  const char *p;

  if (!model_output || !out_ref || !out_command || !out_args)
    return ASTOOLS_ERR_INVALID;
  *out_ref = NULL;
  *out_command = NULL;
  *out_args = NULL;

  p = model_output;
  for (;;) {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    call_spans sp;
    if (parse_call_line(p, len, &sp)) {
      *out_ref = astools_strndup(sp.ref, sp.ref_len);
      *out_command = astools_strndup(sp.cmd, sp.cmd_len);
      *out_args = astools_strndup(sp.args, sp.args_len);
      if (!*out_ref || !*out_command || !*out_args) {
        free(*out_ref);
        free(*out_command);
        free(*out_args);
        *out_ref = NULL;
        *out_command = NULL;
        *out_args = NULL;
        return ASTOOLS_ERR_NOMEM;
      }
      return ASTOOLS_OK;
    }
    if (!eol) break;
    p = eol + 1;
  }
  return ASTOOLS_ERR_NOT_FOUND;
}

/* ---- RESULT / ERROR formatting ------------------------------------------ */

/* Force one line: '\n' becomes ' ', '\r' is dropped. Compact xCDN is
 * single-line already; this defends the protocol invariant regardless. */
static astools_err append_oneline(astools_buf *b, const char *s) {
  if (!s) return ASTOOLS_OK;
  for (; *s; s++) {
    char ch = *s;
    if (ch == '\r') continue;
    if (ch == '\n') ch = ' ';
    TRY(astools_buf_appendc(b, ch));
  }
  return ASTOOLS_OK;
}

static void buf_rtrim(astools_buf *b) {
  while (b->len > 0 &&
         (b->data[b->len - 1] == ' ' || b->data[b->len - 1] == '\t'))
    b->len--;
  if (b->data) b->data[b->len] = '\0';
}

/* Set obj["key"] to a copied string value; false on allocation failure. */
static bool obj_set_string(xcdn_value_t *obj, const char *key,
                           const char *val) {
  xcdn_value_t *sv;
  xcdn_node_t *node;

  sv = xcdn_value_string(val ? val : "");
  if (!sv) return false;
  node = xcdn_node_new(sv);
  if (!node) {
    xcdn_value_free(sv);
    return false;
  }
  xcdn_object_set(obj, key, node);
  return true;
}

astools_err astools_callline_format(const char *ref, const char *command,
                                    const astools_result *r,
                                    char **out_line) {
  astools_buf b;
  astools_err e;

  if (!ref || !command || !r || !out_line) return ASTOOLS_ERR_INVALID;
  *out_line = NULL;
  astools_buf_init(&b);

  e = astools_buf_appends(&b, r->ok ? "RESULT " : "ERROR ");
  if (e == ASTOOLS_OK) e = append_oneline(&b, ref);
  if (e == ASTOOLS_OK) e = astools_buf_appendc(&b, '.');
  if (e == ASTOOLS_OK) e = append_oneline(&b, command);
  if (e == ASTOOLS_OK) e = astools_buf_appendc(&b, ' ');
  if (e != ASTOOLS_OK) goto fail;

  if (r->ok) {
    const char *res = r->result_xcdn;
    if (!res || astools_str_blank(res)) res = "{}";
    e = append_oneline(&b, res);
    if (e != ASTOOLS_OK) goto fail;
  } else {
    /* {code, message} built programmatically so the serializer performs
     * the escaping — the fields are untrusted tool output */
    xcdn_value_t *obj;
    char *txt;

    obj = xcdn_value_object();
    if (!obj) {
      e = ASTOOLS_ERR_NOMEM;
      goto fail;
    }
    if (!obj_set_string(obj, "code", r->error_code) ||
        !obj_set_string(obj, "message", r->error_message)) {
      xcdn_value_free(obj);
      e = ASTOOLS_ERR_NOMEM;
      goto fail;
    }
    txt = astools_xcdn_write_value(obj, false);
    xcdn_value_free(obj);
    if (!txt) {
      e = ASTOOLS_ERR_NOMEM;
      goto fail;
    }
    e = append_oneline(&b, txt);
    free(txt);
    if (e != ASTOOLS_OK) goto fail;
  }

  buf_rtrim(&b); /* single line, no trailing whitespace or newline */
  *out_line = astools_buf_detach(&b);
  if (!*out_line) return ASTOOLS_ERR_NOMEM;
  return ASTOOLS_OK;

fail:
  astools_buf_free(&b);
  return e;
}
