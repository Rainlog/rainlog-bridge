#include "http_util.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "wifi_link.h"

static const char *TAG = "http_util";

#define HTTPS_GET_TIMEOUT_MS 15000

char *http_util_read_body(httpd_req_t *req, size_t max_len) {
  size_t len = req->content_len;
  if (len == 0 || len >= max_len) {
    return NULL;
  }
  char *body = malloc(len + 1);
  if (body == NULL) {
    return NULL;
  }
  size_t off = 0;
  while (off < len) {
    int got = httpd_req_recv(req, body + off, len - off);
    if (got <= 0) {
      free(body);
      return NULL;
    }
    off += (size_t)got;
  }
  body[len] = '\0';
  return body;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// URL-decode src into dst ('+' -> space, %XX -> byte).
static void url_decode(const char *src, char *dst, size_t dstsize) {
  size_t o = 0;
  for (size_t i = 0; src[i] != '\0' && o + 1 < dstsize; i++) {
    if (src[i] == '+') {
      dst[o++] = ' ';
    } else if (src[i] == '%' && hexval(src[i + 1]) >= 0 &&
               hexval(src[i + 2]) >= 0) {
      dst[o++] = (char)((hexval(src[i + 1]) << 4) | hexval(src[i + 2]));
      i += 2;
    } else {
      dst[o++] = src[i];
    }
  }
  dst[o] = '\0';
}

bool http_util_form_get(const char *body, const char *key, char *out,
                        size_t outsize) {
  size_t klen = strlen(key);
  const char *p = body;
  while ((p = strstr(p, key)) != NULL) {
    // Match "key=" at a field boundary (start of body or right after '&').
    bool boundary = (p == body) || (p[-1] == '&');
    if (boundary && p[klen] == '=') {
      const char *val = p + klen + 1;
      const char *end = strchr(val, '&');
      size_t vlen = end ? (size_t)(end - val) : strlen(val);
      char raw[256];
      if (vlen >= sizeof(raw)) {
        vlen = sizeof(raw) - 1;
      }
      memcpy(raw, val, vlen);
      raw[vlen] = '\0';
      url_decode(raw, out, outsize);
      return true;
    }
    p += klen;
  }
  return false;
}

// IPv4 address of one end of a request's socket (ours via getsockname, the
// client's via getpeername), or 0 if unknown. The httpd listener may be an
// IPv6 socket, in which case an IPv4 address shows up v4-mapped
// (::ffff:a.b.c.d).
static uint32_t request_ip4(httpd_req_t *req, bool peer) {
  int fd = httpd_req_to_sockfd(req);
  if (fd < 0) {
    return 0;
  }
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  int rc = peer ? getpeername(fd, (struct sockaddr *)&addr, &len)
                : getsockname(fd, (struct sockaddr *)&addr, &len);
  if (rc != 0) {
    return 0;
  }
  if (addr.ss_family == AF_INET) {
    return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
  }
#if LWIP_IPV6
  if (addr.ss_family == AF_INET6) {
    const uint8_t *a = ((struct sockaddr_in6 *)&addr)->sin6_addr.s6_addr;
    static const uint8_t v4mapped_prefix[12] = {0, 0, 0, 0, 0,    0,
                                                0, 0, 0, 0, 0xFF, 0xFF};
    if (memcmp(a, v4mapped_prefix, sizeof(v4mapped_prefix)) == 0) {
      uint32_t ip4;
      memcpy(&ip4, a + 12, 4);
      return ip4;
    }
  }
#endif
  return 0;
}

uint32_t http_util_peer_ip4(httpd_req_t *req) { return request_ip4(req, true); }

bool http_util_from_softap(httpd_req_t *req) {
  uint32_t local = request_ip4(req, false);
  if (local == 0) {
    return false;
  }
  esp_netif_ip_info_t ap_ip = {0};
  if (esp_netif_get_ip_info(wifi_link_ap_netif(), &ap_ip) != ESP_OK) {
    return false;
  }
  return local == ap_ip.ip.addr;
}

// Accumulates response bytes into a caller-owned, NUL-terminated buffer.
struct https_body {
  char *buf;
  size_t cap;
  size_t len;
};

static esp_err_t https_get_event(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    struct https_body *ctx = (struct https_body *)evt->user_data;
    if (ctx != NULL && ctx->buf != NULL && ctx->len + 1 < ctx->cap) {
      size_t room = ctx->cap - 1 - ctx->len;
      size_t take =
          ((size_t)evt->data_len < room) ? (size_t)evt->data_len : room;
      memcpy(ctx->buf + ctx->len, evt->data, take);
      ctx->len += take;
      ctx->buf[ctx->len] = '\0';
    }
  }
  return ESP_OK;
}

bool http_util_https_get(const char *host, const char *path, const char *query,
                         char *resp, size_t resp_cap, int *status) {
  struct https_body ctx = {.buf = resp, .cap = resp_cap, .len = 0};
  if (resp != NULL && resp_cap > 0) {
    resp[0] = '\0';
  }
  esp_http_client_config_t cfg = {
      .host = host,
      .path = path,
      .query = query,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = HTTPS_GET_TIMEOUT_MS,
      .event_handler = https_get_event,
      .user_data = &ctx,
      // Both sides: the 512-byte TX default truncates long-query request
      // lines and stalls the request (see http_util.h).
      .buffer_size = HTTP_UTIL_BUF_SIZE,
      .buffer_size_tx = HTTP_UTIL_BUF_SIZE,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == NULL) {
    return false;
  }
  esp_err_t err = esp_http_client_perform(client);
  int code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "GET %s failed: %s", host, esp_err_to_name(err));
    return false;
  }
  if (status != NULL) {
    *status = code;
  }
  return true;
}
