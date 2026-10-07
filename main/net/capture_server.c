#include "capture_server.h"

#include <stdlib.h>
#include <string.h>

#include "ap_clients.h"
#include "config_store.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "forwarder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_util.h"
#include "lwip/sockets.h"

static const char *TAG = "capture_server";

#define MAX_QUERY_LEN 1024

// Shared httpd instance; the config server registers its routes on this too.
static httpd_handle_t s_server;

// Self-signed cert/key (CN rtupdate.wunderground.com) for the HTTPS capture
// server below. Some consoles (AcuRite) upload to the spoofed WU host over TLS
// rather than plain HTTP; they don't validate the chain, so an untrusted cert
// is accepted. Local-only (terminates TLS for a spoofed host on the SoftAP),
// so the key is intentionally committed.
extern const uint8_t wu_cert_pem_start[] asm("_binary_wu_cert_pem_start");
extern const uint8_t wu_cert_pem_end[] asm("_binary_wu_cert_pem_end");
extern const uint8_t wu_key_pem_start[] asm("_binary_wu_key_pem_start");
extern const uint8_t wu_key_pem_end[] asm("_binary_wu_key_pem_end");
static httpd_handle_t s_tls_server;

httpd_handle_t capture_server_httpd(void) { return s_server; }

// Extract the upload params: usually the URL query string (GET), but some
// consoles POST the same urlencoded params as the request body. Malloc'd;
// caller frees. NULL if absent/oversize/unreadable.
static char *read_upload_params(httpd_req_t *req) {
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen >= MAX_QUERY_LEN) {
    ESP_LOGW(TAG, "upload query too long (%u bytes), dropping", (unsigned)qlen);
    return NULL;
  }
  if (qlen > 0) {
    char *query = malloc(qlen + 1);
    if (query != NULL &&
        httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK) {
      return query;
    }
    free(query);
    return NULL;
  }
  return http_util_read_body(req, MAX_QUERY_LEN);
}

static esp_err_t wu_upload_handler(httpd_req_t *req) {
  // Uploads come from the station on the SoftAP side only; the home LAN must
  // not be able to spoof readings into the gauge.
  if (!http_util_from_softap(req)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
    return ESP_OK;
  }
  char *params = read_upload_params(req);

  // Answer the station first: it only needs the literal body "success", and
  // must not wait out our (slow, TLS) forwarding - the forwarder task does
  // that work off this thread.
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "success");

  if (params != NULL) {
    ESP_LOGI(TAG, "captured upload (%u bytes)", (unsigned)strlen(params));
    uint32_t peer = http_util_peer_ip4(req);
    char station_id[64] = {0};
    http_util_form_get(params, "ID", station_id, sizeof(station_id));
    ap_clients_note_upload(peer, config_parse_gauge_id(station_id, true));
    forwarder_submit(params, peer);
    free(params);
  }
  return ESP_OK;
}

// Bring up the HTTPS capture server on :443 with the self-signed cert and
// register the same WU upload handler, so a console (AcuRite) that uploads to
// the spoofed WU host over TLS is captured just like a plain-HTTP one.
static void tls_capture_start(void) {
  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.servercert = wu_cert_pem_start;
  conf.servercert_len = wu_cert_pem_end - wu_cert_pem_start;
  conf.prvtkey_pem = wu_key_pem_start;
  conf.prvtkey_len = wu_key_pem_end - wu_key_pem_start;
  conf.httpd.stack_size = 10240;
  conf.httpd.max_uri_handlers = 4;
  // Cap held sockets and recycle the oldest, so the TLS server can't starve
  // the forwarder's outbound socket to Rainlog. The console uses one at a time.
  conf.httpd.max_open_sockets = 3;
  conf.httpd.lru_purge_enable = true;

  esp_err_t err = httpd_ssl_start(&s_tls_server, &conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
    return;
  }
  const httpd_uri_t get_uri = {.uri = config_get()->wu_update_path,
                               .method = HTTP_GET,
                               .handler = wu_upload_handler};
  const httpd_uri_t post_uri = {.uri = config_get()->wu_update_path,
                                .method = HTTP_POST,
                                .handler = wu_upload_handler};
  httpd_register_uri_handler(s_tls_server, &get_uri);
  httpd_register_uri_handler(s_tls_server, &post_uri);
  ESP_LOGI(TAG, "HTTPS capture server up on :443 (self-signed)");
}

void capture_server_start(void) {
  if (config_wifi_interception_enabled()) tls_capture_start();
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;
  // Cap held sockets so the plain-HTTP server leaves room for the TLS server,
  // DNS, and the forwarder/OTA TLS clients within LWIP's 16-socket table.
  config.max_open_sockets = 4;
  // WU capture (GET+POST) plus the config server's routes share this server
  // (17 routes registered at present).
  config.max_uri_handlers = 20;
  // Generous task stack: the default 4KB overflowed once (instant client
  // drops). Forwarding now runs on its own task, but the config handlers still
  // template/parse sizable forms, and the headroom is cheap insurance.
  config.stack_size = 10240;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return;
  }

  const httpd_uri_t get_uri = {
      .uri = config_get()->wu_update_path,
      .method = HTTP_GET,
      .handler = wu_upload_handler,
  };
  const httpd_uri_t post_uri = {
      .uri = config_get()->wu_update_path,
      .method = HTTP_POST,
      .handler = wu_upload_handler,
  };
  if (config_wifi_interception_enabled()) {
    httpd_register_uri_handler(s_server, &get_uri);
    httpd_register_uri_handler(s_server, &post_uri);
  }
  ESP_LOGI(TAG, "capture server up on port 80 %s",
           config_get()->wu_update_path);
}
