/* Preserve decoded values through serialization, including embedded NULs. */
#include "json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int equal(const jx_value *a, const jx_value *b) {
  jx_type type = jx_typeof(a);
  if (type != jx_typeof(b))
    return 0;
  switch (type) {
  case JX_NULL:
    return 1;
  case JX_BOOL:
    return jx_bool_value(a) == jx_bool_value(b);
  case JX_NUMBER:
    return jx_is_int(a) && jx_is_int(b) ? jx_int_value(a) == jx_int_value(b)
                                        : jx_double_value(a) == jx_double_value(b);
  case JX_STRING: {
    size_t n = jx_string_length(a);
    return n == jx_string_length(b) && !memcmp(jx_string_value(a), jx_string_value(b), n);
  }
  case JX_ARRAY:
    if (jx_array_len(a) != jx_array_len(b))
      return 0;
    for (size_t i = 0; i < jx_array_len(a); i++)
      if (!equal(jx_array_at(a, i), jx_array_at(b, i)))
        return 0;
    return 1;
  case JX_OBJECT:
    if (jx_object_count(a) != jx_object_count(b))
      return 0;
    for (size_t i = 0; i < jx_object_count(a); i++) {
      const char *key = jx_object_key_at(a, i);
      const jx_value *value = jx_object_get(b, key);
      if (!value || !equal(jx_object_value_at(a, i), value))
        return 0;
    }
    return 1;
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 16384)
    return 0;
  (void)jx_utf8_valid((const char *)data, size);
  jx_value *value = NULL, *copy = NULL, *decoded = NULL;
  if (jx_parse((const char *)data, size, &value))
    return 0;
  copy = jx_clone(value);
  if (!copy) {
    jx_free(value);
    return 0;
  }
  if (!equal(value, copy))
    abort();
  char *wire = jx_write(value, 0);
  if (wire) {
    if (jx_parse(wire, strlen(wire), &decoded) || !equal(value, decoded))
      abort();
    char *again = jx_write(decoded, 0);
    if (again && strcmp(wire, again))
      abort();
    free(again);
    free(wire);
  }
  jx_free(value);
  jx_free(copy);
  jx_free(decoded);
  return 0;
}
