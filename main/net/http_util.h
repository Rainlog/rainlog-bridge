// Rainlog Wireless Bridge - shared helpers for HTTP request handlers.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

// Read the full request body (looping until content_len bytes arrive; a single
// httpd_req_recv may return a partial TCP segment) into a malloc'd
// NUL-terminated buffer. Returns NULL if there is no body, the body exceeds
// max_len, or a receive fails. Caller frees.
char *http_util_read_body(httpd_req_t *req, size_t max_len);

// Extract a urlencoded form field from a request body into out (URL-decoded:
// '+' -> space, %XX -> byte). Returns true if the field is present.
bool http_util_form_get(const char *body, const char *key, char *out,
                        size_t outsize);

// True if the request arrived on the SoftAP interface (the bridge's own WiFi).
// The config and capture endpoints are SoftAP-only: the home LAN (STA side)
// must not be able to read or change the bridge config, or spoof uploads.
bool http_util_from_softap(httpd_req_t *req);

// The requesting client's IPv4 address (network byte order, as in
// esp_ip4_addr_t.addr), or 0 if unknown. Used to flag "this device" in the
// client list.
uint32_t http_util_peer_ip4(httpd_req_t *req);

// esp_http_client TX and RX buffer size. The TX side is the critical one: the
// whole request line ("GET <path>?<query> HTTP/1.1") is built in the TX
// buffer, and a forwarded console upload's query can run several hundred
// bytes. With the 512 default, a query past ~430 bytes leaves no room for the
// first request header, and esp_http_client (IDF v6) then SILENTLY truncates
// the request (http_header_generate_string logs "Buffer length is small to
// fit all the headers", esp_http_client_request_send breaks out with no
// terminating blank line) and the request stalls to an EAGAIN timeout. 2048
// covers the forwarder's worst case (1024-byte captured query + bridge identity + path + headers)
// with room to spare. Shared by http_util_https_get and the OTA download.
#define HTTP_UTIL_BUF_SIZE 2048

// HTTPS GET host+path?query (TLS via the bundled CA store). Captures up to
// resp_cap-1 bytes of the response body into resp, NUL-terminated; pass
// resp=NULL / resp_cap=0 to discard the body. On a completed request returns
// true and writes the HTTP status code to *status (status may be NULL).
// Returns false on a transport failure (no response), having logged it.
bool http_util_https_get(const char *host, const char *path, const char *query,
                         char *resp, size_t resp_cap, int *status);
