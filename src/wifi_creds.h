#ifndef WIFI_CREDS_H
#define WIFI_CREDS_H

#include <stdbool.h>
#include <stddef.h>

#define WIFI_CREDS_SSID_MAX_LEN 33
#define WIFI_CREDS_PASSWORD_MAX_LEN 64

bool wifi_creds_load(char* ssid, size_t ssid_size, char* password, size_t password_size);
bool wifi_creds_save(const char* ssid, const char* password);

#endif // WIFI_CREDS_H
