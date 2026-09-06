/**
 * http_server.c
 *
 * Minimal HTTP/1.1 server using lwIP's raw (callback-based) TCP API --
 * NO_SYS=1 mode, no sockets, no threads. This is deliberately small and
 * single-purpose: three routes (list/save/forget profiles) plus one
 * static bootstrap page. Not a general-purpose web server.
 *
 * Runs entirely from lwIP's own callback context (serviced in the
 * background by pico_cyw43_arch_lwip_threadsafe_background -- see
 * CMakeLists.txt), on core 1 alongside BTstack. No manual polling loop
 * needed; once http_server_init() has set up the listening socket,
 * everything else happens via callbacks.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/mem.h"

#include "controller_config.h"
#include "json_util.h"
#include "web_ui.h"

#define REQ_BUF_SIZE 1024
#define RESP_BUF_SIZE 4096

// Small case-insensitive prefix compare -- avoids depending on
// strncasecmp() (a POSIX/BSD extension not guaranteed to be pulled in
// by every C library configuration this toolchain might use).
static bool starts_with_ci(const char* s, const char* prefix)
{
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a += ('a' - 'A');
        if (b >= 'A' && b <= 'Z') b += ('a' - 'A');
        if (a != b) return false;
    }
    return true;
}

typedef struct {
    char   req[REQ_BUF_SIZE];
    size_t req_len;
} conn_state_t;

static const char* remap_name(int8_t r)
{
    switch (r) {
        case REMAP_BUTTON_A: return "A";
        case REMAP_BUTTON_B: return "B";
        case REMAP_BUTTON_X: return "X";
        case REMAP_BUTTON_Y: return "Y";
        default: return "default";
    }
}

static int8_t parse_remap_value(const char* val)
{
    if (strcmp(val, "A") == 0) return REMAP_BUTTON_A;
    if (strcmp(val, "B") == 0) return REMAP_BUTTON_B;
    if (strcmp(val, "X") == 0) return REMAP_BUTTON_X;
    if (strcmp(val, "Y") == 0) return REMAP_BUTTON_Y;
    return REMAP_DEFAULT;
}

// Finds the end of the HTTP headers ("\r\n\r\n") in the accumulated
// request buffer. Returns the offset right after it, or -1 if not
// found yet (need to wait for more data).
static int find_headers_end(const char* buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}

static int get_content_length(const char* headers, size_t headers_len)
{
    const char* p = headers;
    const char* end = headers + headers_len;
    const char* needle = "Content-Length:";
    size_t needle_len = strlen(needle);

    for (; p + needle_len < end; p++) {
        if (starts_with_ci(p, needle)) {
            p += needle_len;
            while (p < end && *p == ' ') p++;
            return atoi(p);
        }
    }
    return 0;
}

// Builds the JSON array response body for GET /api/list.
static size_t build_list_response(char* out, size_t out_size)
{
    controller_profile_t profiles[CONTROLLER_CONFIG_MAX_SLOTS];
    int n = controller_config_list(profiles, CONTROLLER_CONFIG_MAX_SLOTS);

    size_t pos = 0;
    pos += snprintf(out + pos, out_size - pos, "[");
    for (int i = 0; i < n; i++) {
        pos += snprintf(out + pos, out_size - pos,
            "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"model\":\"%s\","
            "\"port\":%d,\"remap\":{\"a\":\"%s\",\"b\":\"%s\",\"x\":\"%s\",\"y\":\"%s\"}}",
            (i > 0) ? "," : "",
            profiles[i].mac[0], profiles[i].mac[1], profiles[i].mac[2],
            profiles[i].mac[3], profiles[i].mac[4], profiles[i].mac[5],
            profiles[i].model_name,
            profiles[i].gc_port + 1,
            remap_name(profiles[i].remap_a), remap_name(profiles[i].remap_b),
            remap_name(profiles[i].remap_x), remap_name(profiles[i].remap_y));
        if (pos >= out_size) break;
    }
    pos += snprintf(out + pos, out_size - pos, "]");
    return pos;
}

// Handles POST /api/save. Body must contain at least "mac" and "port",
// optionally "remap": {"a":..,"b":..,"x":..,"y":..}. Returns true on
// success (profile found and saved).
static bool handle_save(const char* body, size_t body_len)
{
    char mac_str[24];
    if (!json_extract_string_scoped(body, body_len, "mac", mac_str, sizeof(mac_str))) {
        return false;
    }
    uint8_t mac[6];
    if (!parse_mac_address(mac_str, mac)) {
        return false;
    }

    controller_profile_t profile;
    if (!controller_config_get_or_create(mac, NULL, &profile)) {
        return false;
    }

    int port_1based = json_extract_int(body, "port", profile.gc_port + 1);
    if (port_1based >= 1 && port_1based <= 4) {
        profile.gc_port = (uint8_t)(port_1based - 1);
    }

    const char* remap_obj = strstr(body, "\"remap\"");
    if (remap_obj) {
        size_t remap_scope_len = (size_t)((body + body_len) - remap_obj);
        char val[16];
        if (json_extract_string_scoped(remap_obj, remap_scope_len, "a", val, sizeof(val)))
            profile.remap_a = parse_remap_value(val);
        if (json_extract_string_scoped(remap_obj, remap_scope_len, "b", val, sizeof(val)))
            profile.remap_b = parse_remap_value(val);
        if (json_extract_string_scoped(remap_obj, remap_scope_len, "x", val, sizeof(val)))
            profile.remap_x = parse_remap_value(val);
        if (json_extract_string_scoped(remap_obj, remap_scope_len, "y", val, sizeof(val)))
            profile.remap_y = parse_remap_value(val);
    }

    return controller_config_save(&profile);
}

// Handles POST /api/forget. Body must contain "mac".
static bool handle_forget(const char* body, size_t body_len)
{
    char mac_str[24];
    if (!json_extract_string_scoped(body, body_len, "mac", mac_str, sizeof(mac_str))) {
        return false;
    }
    uint8_t mac[6];
    if (!parse_mac_address(mac_str, mac)) {
        return false;
    }
    return controller_config_delete_by_mac(mac);
}

// Builds the full HTTP response (status line + headers + body) for a
// parsed request, into 'out' (size 'out_size'). Returns the total
// response length.
static size_t build_response(const char* method, const char* path,
                              const char* body, size_t body_len,
                              char* out, size_t out_size)
{
    char json_body[RESP_BUF_SIZE - 256];
    const char* content_type = "application/json";
    size_t json_len;

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        content_type = "text/html; charset=utf-8";
        json_len = strlen(WEB_UI_INDEX_HTML);
        memcpy(json_body, WEB_UI_INDEX_HTML, json_len);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/list") == 0) {
        json_len = build_list_response(json_body, sizeof(json_body));
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/save") == 0) {
        bool ok = handle_save(body, body_len);
        json_len = snprintf(json_body, sizeof(json_body), "{\"ok\":%s}", ok ? "true" : "false");
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/forget") == 0) {
        bool ok = handle_forget(body, body_len);
        json_len = snprintf(json_body, sizeof(json_body), "{\"ok\":%s}", ok ? "true" : "false");
    } else {
        content_type = "text/plain";
        json_len = snprintf(json_body, sizeof(json_body), "Not found");
        return snprintf(out, out_size,
            "HTTP/1.1 404 Not Found\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
            "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n%.*s",
            content_type, (unsigned)json_len, (int)json_len, json_body);
    }

    return snprintf(out, out_size,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n%.*s",
        content_type, (unsigned)json_len, (int)json_len, json_body);
}

static err_t on_recv(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err)
{
    conn_state_t* cs = (conn_state_t*)arg;

    if (err != ERR_OK || p == NULL) {
        // Remote closed the connection (or an error) -- clean up.
        if (p) pbuf_free(p);
        if (cs) mem_free(cs);
        tcp_arg(tpcb, NULL);
        tcp_close(tpcb);
        return ERR_OK;
    }

    // Accumulate into our request buffer (bounded -- drop anything past
    // our buffer size, our own requests are always small).
    size_t copy_len = p->tot_len;
    if (cs->req_len + copy_len > sizeof(cs->req) - 1) {
        copy_len = sizeof(cs->req) - 1 - cs->req_len;
    }
    if (copy_len > 0) {
        pbuf_copy_partial(p, cs->req + cs->req_len, (u16_t)copy_len, 0);
        cs->req_len += copy_len;
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    int headers_end = find_headers_end(cs->req, cs->req_len);
    if (headers_end < 0) {
        // Headers not fully received yet -- wait for more data.
        return ERR_OK;
    }

    int content_length = get_content_length(cs->req, (size_t)headers_end);
    size_t total_needed = (size_t)headers_end + (size_t)(content_length > 0 ? content_length : 0);
    if (cs->req_len < total_needed && total_needed < sizeof(cs->req)) {
        // Body not fully received yet -- wait for more data.
        return ERR_OK;
    }

    // Parse the request line: "METHOD /path HTTP/1.x"
    char method[8] = {0};
    char path[128] = {0};
    sscanf(cs->req, "%7s %127s", method, path);

    const char* body = cs->req + headers_end;
    size_t body_len = cs->req_len - (size_t)headers_end;

    static char resp[RESP_BUF_SIZE];
    size_t resp_len = build_response(method, path, body, body_len, resp, sizeof(resp));

    tcp_write(tpcb, resp, (u16_t)resp_len, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);

    mem_free(cs);
    tcp_arg(tpcb, NULL);
    tcp_close(tpcb);

    return ERR_OK;
}

static err_t on_accept(void* arg, struct tcp_pcb* newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

    conn_state_t* cs = (conn_state_t*)mem_malloc(sizeof(conn_state_t));
    if (!cs) {
        tcp_close(newpcb);
        return ERR_MEM;
    }
    memset(cs, 0, sizeof(*cs));

    tcp_arg(newpcb, cs);
    tcp_recv(newpcb, on_recv);
    tcp_priority(newpcb, TCP_PRIO_MIN);

    return ERR_OK;
}

void http_server_init(void)
{
    struct tcp_pcb* pcb = tcp_new();
    if (!pcb) {
        printf("HTTP server: failed to create PCB\n");
        return;
    }

    if (tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) {
        printf("HTTP server: failed to bind port 80\n");
        return;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) {
        printf("HTTP server: failed to listen\n");
        return;
    }

    tcp_accept(pcb, on_accept);
    printf("HTTP server: listening on port 80\n");
}
