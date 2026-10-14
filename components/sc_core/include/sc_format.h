#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

// Fixed-point telemetry formatting avoids newlib floating printf allocations.
static inline bool sc_format_fixed(char *out, size_t cap, float value, unsigned decimals)
{
    if (!isfinite(value) || decimals > 6) return false;
    uint32_t scale = 1;
    for (unsigned i = 0; i < decimals; i++) scale *= 10;
    double rounded = fabs((double)value) * scale + 0.5;
    if (rounded >= UINT32_MAX) return false;
    uint32_t units = (uint32_t)rounded;
    int n = decimals
        ? snprintf(out, cap, "%s%u.%0*u", signbit(value) ? "-" : "",
                   (unsigned)(units / scale), (int)decimals, (unsigned)(units % scale))
        : snprintf(out, cap, "%s%u", signbit(value) ? "-" : "", (unsigned)units);
    return n >= 0 && (size_t)n < cap;
}
