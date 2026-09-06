#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int json_extract_int(const char* json, const char* key, int fallback);

bool json_extract_string_scoped(const char* json, size_t scope_len, const char* key,
                                 char* out, size_t out_size);

bool parse_mac_address(const char* str, uint8_t mac[6]);

#endif // JSON_UTIL_H
