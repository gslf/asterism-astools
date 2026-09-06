/* Fixed-duration profile shared by contracts, configuration and standard tools. */
#ifndef ASTOOLS_DURATION_H
#define ASTOOLS_DURATION_H
#include <stdbool.h>
#include <stdint.h>
/* Execution periods fit the public uint32 deadline and exclude Windows INFINITE. */
#define ASTOOLS_PERIOD_MAX_MS (UINT32_MAX - UINT32_C(1))
/* PnW or PnDTnHnMnS; seconds may have 1..3 decimal digits. No rounding,
 * calendar months/years, signs, exponents or locale-dependent parsing. */
bool astools_duration_parse_ms(const char *text, int64_t *out);
#endif
