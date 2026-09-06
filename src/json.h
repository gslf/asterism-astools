/*
 * json.h — strict RFC 8259 JSON codec for process transports.
 *
 * DOM-style value tree shared by MCP and language-server transports.
 * Tool manifests and ordinary tool requests still use xCDN.
 *
 * Guarantees:
 *   - Strict parsing: UTF-8 only (validated), \uXXXX escapes with surrogate
 *     pairs decoded to UTF-8, depth cap 64, no comments, no trailing
 *     garbage, no NaN/Inf, no unescaped control characters in strings.
 *   - Duplicate keys and embedded NUL keys are rejected.
 *   - Numbers carry a double plus an int64 view when the literal is
 *     integral and in range (jx_is_int).
 *   - No global state; every function is thread-compatible over disjoint
 *     trees.
 *
 * Ownership:
 *   - Constructors return NULL on allocation failure (jx_string also on
 *     invalid UTF-8, jx_double also on non-finite input).
 *   - jx_array_push / jx_object_set ALWAYS take ownership of the value,
 *     including on failure (the value is freed); a NULL value yields -1.
 *     This makes chained construction leak-free: free the outermost value
 *     once and check the accumulated status.
 *   - Object keys are copied and treated as NUL-terminated C strings.
 */

#ifndef ASTOOLS_JSON_H
#define ASTOOLS_JSON_H

#include <stddef.h>
#define jx_utf8_valid astls_x_jx_utf8_valid
int jx_utf8_valid(const char *text, size_t len);

/* Private link symbols cannot collide with an embedding host. */
#define jx_parse astls_x_jx_parse
#define jx_free astls_x_jx_free
#define jx_clone astls_x_jx_clone
#define jx_null astls_x_jx_null
#define jx_bool astls_x_jx_bool
#define jx_int astls_x_jx_int
#define jx_double astls_x_jx_double
#define jx_string astls_x_jx_string
#define jx_array astls_x_jx_array
#define jx_object astls_x_jx_object
#define jx_typeof astls_x_jx_typeof
#define jx_bool_value astls_x_jx_bool_value
#define jx_is_int astls_x_jx_is_int
#define jx_int_value astls_x_jx_int_value
#define jx_double_value astls_x_jx_double_value
#define jx_string_value astls_x_jx_string_value
#define jx_string_length astls_x_jx_string_length
#define jx_array_push astls_x_jx_array_push
#define jx_array_len astls_x_jx_array_len
#define jx_array_at astls_x_jx_array_at
#define jx_object_set astls_x_jx_object_set
#define jx_object_get astls_x_jx_object_get
#define jx_object_count astls_x_jx_object_count
#define jx_object_key_at astls_x_jx_object_key_at
#define jx_object_value_at astls_x_jx_object_value_at
#define jx_write astls_x_jx_write

typedef enum {
  JX_NULL = 0,
  JX_BOOL,
  JX_NUMBER,
  JX_STRING,
  JX_ARRAY,
  JX_OBJECT
} jx_type;

typedef struct jx_value jx_value;

/* Parse [text, text+len) as exactly one JSON text (any value type at the
 * top level). Returns 0 and sets *out on success; nonzero on any error
 * with *out = NULL. text need not be NUL-terminated. */
int  jx_parse(const char *text, size_t len, jx_value **out);
void jx_free(jx_value *v); /* NULL is a no-op */

/* Deep copy; NULL on allocation failure or v == NULL. */
jx_value *jx_clone(const jx_value *v);

/* ---- constructors ------------------------------------------------------- */

jx_value *jx_null(void);
jx_value *jx_bool(int b);
jx_value *jx_int(long long i);
jx_value *jx_double(double d);        /* NULL on NaN/Inf */
jx_value *jx_string(const char *utf8); /* copies; NULL on invalid UTF-8 */
jx_value *jx_array(void);
jx_value *jx_object(void);

/* ---- accessors ---------------------------------------------------------- */

jx_type jx_typeof(const jx_value *v); /* JX_NULL when v == NULL */

int         jx_bool_value(const jx_value *v);    /* 0 unless JX_BOOL */
int         jx_is_int(const jx_value *v);
long long   jx_int_value(const jx_value *v);     /* truncates non-ints */
double      jx_double_value(const jx_value *v);
const char *jx_string_value(const jx_value *v);  /* NUL-terminated; NULL
                                                    unless JX_STRING */
size_t      jx_string_length(const jx_value *v); /* bytes, embedded NULs
                                                    included */

/* ---- containers --------------------------------------------------------- */

int       jx_array_push(jx_value *arr, jx_value *v); /* 0 ok; owns v */
size_t    jx_array_len(const jx_value *arr);
jx_value *jx_array_at(const jx_value *arr, size_t i); /* borrowed */

int       jx_object_set(jx_value *obj, const char *key, jx_value *v);
jx_value *jx_object_get(const jx_value *obj, const char *key); /* borrowed */
size_t    jx_object_count(const jx_value *obj);
const char *jx_object_key_at(const jx_value *obj, size_t i);
jx_value   *jx_object_value_at(const jx_value *obj, size_t i);

/* ---- writer ------------------------------------------------------------- */

/* Serialize to a malloc'd NUL-terminated string; NULL on allocation
 * failure. pretty == 0 emits compact JSON with no raw newlines (control
 * characters in strings are escaped), suitable for line-delimited
 * transports. */
char *jx_write(const jx_value *v, int pretty);

#endif /* ASTOOLS_JSON_H */
