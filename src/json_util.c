#include "json_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int json_extract_int(const char* json, const char* key, int fallback)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return fallback;
    p = strchr(p, ':');
    if (!p) return fallback;
    p++;
    while (*p == ' ') p++;
    return atoi(p);
}

bool json_extract_string_scoped(const char* json, size_t scope_len, const char* key,
                                 char* out, size_t out_size)
{
    char pattern[8];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t pattern_len = strlen(pattern);

    for (size_t i = 0; i + pattern_len <= scope_len; i++) {
        if (memcmp(json + i, pattern, pattern_len) != 0) continue;

        const char* p = json + i + pattern_len;
        const char* end = json + scope_len;
        while (p < end && *p != ':') p++;
        if (p >= end) return false;
        p++;
        while (p < end && *p == ' ') p++;
        if (p >= end || *p != '"') return false;
        p++;

        size_t n = 0;
        while (p < end && *p != '"' && n < out_size - 1) {
            out[n++] = *p++;
        }
        out[n] = '\0';
        return true;
    }
    return false;
}

bool parse_mac_address(const char* str, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    return true;
}
