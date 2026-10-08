#include "config_server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "activity.h"
#include "ap_clients.h"
#include "capture_server.h"
#include "config.h"
#include "config_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "firmware_image.h"
#include "http_util.h"
#include "nvs.h"
#include "ota_update.h"
#include "radio/radio.h"
#include "session_auth.h"
#include "wifi_link.h"

static const char *TAG = "config_server";

// Big enough for the full form / config JSON with WU_MAP_MAX mappings, every
// id/key at max length and (pathologically) every byte JSON-escaping to 6x.
#if RAINLOG_RADIO
#define BODY_MAX 8192
#define PAGE_MAX 16384
#else
#define BODY_MAX 4096
#define PAGE_MAX 8192
#endif

// The config page: self-contained HTML compressed at build time with Zopfli.
// Embedded as binary gzip bytes, without a terminating null byte.
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

// The raindrop PNG, served as /favicon.ico for pages that aren't the embedded
// config page (the login form relies on the browser's automatic request).
extern const uint8_t favicon_png_start[] asm("_binary_favicon_png_start");
extern const uint8_t favicon_png_end[] asm("_binary_favicon_png_end");

static esp_timer_handle_t s_restart_timer;

// True if the AP password is still the well-known shipped default (printed on
// the LCD), which we don't allow keeping once configured.
static bool ap_pass_is_default(const char *pass) {
  return strcmp(pass, CFG_WIFI_AP_PASSWORD) == 0;
}

// Minimal base64 decoder for the Basic auth header (mbedtls 4.x dropped its
// base64 module). Stops at '=' padding. Returns the decoded length, or -1 on
// an invalid character / output overflow.
static int b64_decode(const char *in, uint8_t *out, size_t outcap) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int val = 0;
  int bits = 0;
  size_t o = 0;
  for (const char *p = in; *p != '\0' && *p != '='; p++) {
    const char *idx = strchr(alphabet, *p);
    if (idx == NULL) {
      return -1;
    }
    val = (val << 6) | (int)(idx - alphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= outcap) {
        return -1;
      }
      out[o++] = (uint8_t)((val >> bits) & 0xFF);
    }
  }
  return (int)o;
}

// True if the request carries "Authorization: Basic <base64(user:pass)>" with
// the right password. The username is ignored (there is only one identity).
// Kept for curl/scripts; browsers use the login form (no WWW-Authenticate is
// ever sent, so they never pop the native auth dialog).
static bool basic_auth_ok(httpd_req_t *req) {
  char hdr[160];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) !=
          ESP_OK ||
      strncasecmp(hdr, "Basic ", 6) != 0) {
    return false;
  }
  uint8_t dec[100];
  int n = b64_decode(hdr + 6, dec, sizeof(dec) - 1);
  if (n <= 0) {
    return false;
  }
  dec[n] = '\0';
  const char *colon = strchr((const char *)dec, ':');
  if (colon == NULL) {
    return false;
  }
  return session_auth_password_matches(colon + 1);
}

// Setup access control. One secret, the bridge WiFi password: on the bridge's
// own WiFi it is the WPA2 key that got you on, so requests there pass with no
// further prompt (also, the phone captive-portal webviews that auto-open the
// setup page cannot show any prompt). From the home LAN, the same password is
// entered on the login form (session cookie) or sent as HTTP Basic auth
// (curl). The upload capture endpoint is NOT behind this gate - it stays
// strictly SoftAP-only so the LAN can never spoof readings.
static bool authorized(httpd_req_t *req) {
  return http_util_from_softap(req) || session_auth_ok(req) ||
         basic_auth_ok(req);
}

// Auth gate used by every non-page config route. Sends the 401 itself; caller
// just returns. (The page route serves the login form instead, see
// page_handler.)
static bool reject_unauthorized(httpd_req_t *req) {
  if (authorized(req)) {
    return false;
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "sign in required");
  return true;
}

static esp_err_t send_error_page(httpd_req_t *req, const char *msg) {
  char page[512];
  snprintf(
      page, sizeof(page),
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "</head><body style=\"font-family:sans-serif;max-width:430px;"
      "margin:2em auto;padding:0 1em\"><h2>Couldn't save</h2>"
      "<p style=\"color:#c00\">%s</p><p><a href=\"/\">Back to setup</a></p>"
      "</body></html>",
      msg);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req, page);
  return ESP_OK;
}

static const char *SAVED_PAGE =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta http-equiv=refresh content=\"8;url=/\"></head>"
    "<body style=\"font-family:sans-serif;text-align:center;margin-top:3em\">"
    "<h2>Saved. Rebooting&hellip;</h2>"
    "<p>The bridge will reconnect with the new settings.</p></body></html>";

// Escape a string for embedding inside a JSON double-quoted value.
static void json_escape(const char *src, char *dst, size_t dstsize) {
  size_t o = 0;
  for (const char *p = src; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    char esc[7];  // worst case "\u00XX"
    const char *rep;
    size_t need;
    if (c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = (char)c;
      esc[2] = '\0';
      rep = esc;
      need = 2;
    } else if (c < 0x20) {
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      rep = esc;
      need = 6;
    } else {
      esc[0] = (char)c;
      esc[1] = '\0';
      rep = esc;
      need = 1;
    }
    if (o + need + 1 > dstsize) {
      break;
    }
    memcpy(dst + o, rep, need);
    o += need;
  }
  dst[o] = '\0';
}

// GET /, /setup, /devices, /firmware: the embedded single-file config page
// (static, no templating). Field values are filled client-side from /config. A
// LAN visitor without a session gets the sign-in page instead of the config
// page.
static esp_err_t page_handler(httpd_req_t *req) {
  if (!authorized(req)) {
    return session_auth_send_login_page(req, false);
  }
  activity_poke();
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char *)index_html_gz_start,
                         index_html_gz_end - index_html_gz_start);
}

// GET /favicon.ico: the raindrop, deliberately ungated (it is just the logo,
// and the login page needs it before any session exists). Browsers accept PNG
// bytes at this name. Cacheable: the icon only changes with a firmware update.
static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
  return httpd_resp_send(req, (const char *)favicon_png_start,
                         favicon_png_end - favicon_png_start);
}

// GET /config: current settings as JSON for the page to populate the form.
// WiFi passwords are never returned, only booleans saying whether a value is
// set. The WU map is an array of {gauge_id, wu_id, wu_key}; the WU key IS
// returned (the page shows it behind a Show toggle) - it is a per-station
// upload key, not a network credential, and the page itself is already gated
// (SoftAP side or signed-in LAN session).
#if RAINLOG_RADIO
// Sensor names use protocol identity, independent of uploader assignments.
static void radio_name_key(unsigned model, unsigned id, unsigned channel,
                           char key[16]) {
  snprintf(key, 16, "r%u_%u_%u", model, id, channel);
}
static void radio_name_json(unsigned model, unsigned id, unsigned channel,
                            char escaped[198]) {
  char key[16], name[33] = {0};
  radio_name_key(model, id, channel, key);
  nvs_handle_t handle;
  if (nvs_open("radio_names", NVS_READONLY, &handle) == ESP_OK) {
    size_t size = sizeof(name);
    if (nvs_get_str(handle, key, name, &size) != ESP_OK) name[0] = 0;
    nvs_close(handle);
  }
  json_escape(name, escaped, 198);
}

#endif

char *config_server_config_json(void) {
  const bridge_config_t *cfg = config_get();
  // Worst case every byte escapes to "\u00XX" (6x).
  char sta_ssid[sizeof(cfg->sta_ssid) * 6];
  char ap_ssid[sizeof(cfg->ap_ssid) * 6];
  json_escape(cfg->sta_ssid, sta_ssid, sizeof(sta_ssid));
  json_escape(cfg->ap_ssid, ap_ssid, sizeof(ap_ssid));

  char *json = malloc(PAGE_MAX);
  if (json == NULL) {
    return NULL;
  }
  // ap_ip lets the page tell which side it is viewed from (device links only
  // work for viewers on the bridge's own WiFi).
  char ap_ip[16];
  wifi_link_ap_ip_str(ap_ip, sizeof(ap_ip));
  int o = snprintf(json, PAGE_MAX,
                   "{\"sta_ssid\":\"%s\",\"ap_ssid\":\"%s\","
                   "\"ap_pass_default\":%s,\"ap_ip\":\"%s\","
                   "\"bridge_wifi_auto_off\":%s,\"bridge_wifi_active\":%s,\"wu_map\":[",
                   sta_ssid, ap_ssid,
                   ap_pass_is_default(cfg->ap_pass) ? "true" : "false", ap_ip,
                   cfg->bridge_wifi_auto_off ? "true" : "false",
                   wifi_link_ap_enabled() ? "true" : "false");
  for (uint8_t i = 0; i < cfg->wu_map_count && o > 0 && o < PAGE_MAX; i++) {
    char device[sizeof(cfg->wu_map[i].device) * 6];
    json_escape(cfg->wu_map[i].device, device, sizeof(device));
    char wu_id[sizeof(cfg->wu_map[i].wu_id) * 6];
    char wu_key[sizeof(cfg->wu_map[i].wu_key) * 6];
    json_escape(cfg->wu_map[i].wu_id, wu_id, sizeof(wu_id));
    json_escape(cfg->wu_map[i].wu_key, wu_key, sizeof(wu_key));
    o += snprintf(json + o, PAGE_MAX - o,
                  "%s{\"gauge_id\":%lu,\"wu_id\":\"%s\",\"wu_key\":\"%s\","
                  "\"device\":\"%s\"}",
                  i ? "," : "", (unsigned long)cfg->wu_map[i].gauge_id, wu_id,
                  wu_key, device);
  }
#if RAINLOG_RADIO
  if (o > 0 && o < PAGE_MAX)
    o += snprintf(json + o, PAGE_MAX - o,
                  "],\"radio_enabled\":%s,\"radio_map\":[",
                  cfg->radio_enabled ? "true" : "false");
  for (unsigned i = 0; i < cfg->radio_map_count && o > 0 && o < PAGE_MAX; i++) {
    const radio_mapping_t *m = &cfg->radio_map[i];
    char name[198];
    radio_name_json(m->model, m->sensor_id, (unsigned char)m->channel, name);
    char key[sizeof(m->rainlog_key) * 6];
    json_escape(m->rainlog_key, key, sizeof(key));
    o += snprintf(json + o, PAGE_MAX - o,
                  "%s{\"model\":%u,\"sensor_id\":%lu,\"channel\":%u,\"gauge_"
                  "id\":%lu,\"rainlog_key\":\"%s\",\"name\":\"%s\"}",
                  i ? "," : "", m->model, (unsigned long)m->sensor_id,
                  (unsigned char)m->channel, (unsigned long)m->gauge_id, key, name);
  }
#endif
  if (o > 0 && o < PAGE_MAX) o += snprintf(json + o, PAGE_MAX - o, "]}");
  if (o < 0 || o >= PAGE_MAX) {
    free(json);
    return NULL;
  }
  return json;
}

static esp_err_t send_owned_json(httpd_req_t *req, char *json) {
  if (!json) return ESP_ERR_NO_MEM;
  httpd_resp_set_type(req, "application/json");
  esp_err_t err = httpd_resp_sendstr(req, json);
  free(json);
  return err;
}
#if RAINLOG_RADIO
char *config_server_radio_json(void) {
  radio_status_t status;
  radio_status(&status);
  radio_sensor_t sensors[RADIO_SENSORS_MAX];
  size_t count = radio_sensors(sensors, RADIO_SENSORS_MAX);
  char *json = malloc(PAGE_MAX);
  if (!json) return NULL;
  int o = snprintf(json, PAGE_MAX,
                   "{\"available\":%s,\"receiving\":%s,\"frequency_hz\":%lu,"
                   "\"error\":%d,\"sensors\":[",
                   status.available ? "true" : "false",
                   status.receiving ? "true" : "false",
                   (unsigned long)status.frequency_hz, status.error);
  unsigned seen = 0;
  int64_t now = esp_timer_get_time();
  for (unsigned i = 0; i < count && o > 0 && o < PAGE_MAX; i++) {
    const weather_packet_t *p = &sensors[i].reading.packet;
    const radio_mapping_t *m =
        config_find_radio_mapping(p->model, p->id, p->channel);
    char name[198];
    radio_name_json(p->model, p->id, (unsigned char)p->channel, name);
    o += snprintf(
        json + o, PAGE_MAX - o,
        "%s{\"model\":%u,\"sensor_id\":%u,\"channel\":%u,\"age_s\":%"
                  "lld,\"gauge_id\":%lu,\"has_rain\":%s,\"rain_raw\":%u,"
                  "\"packets\":%lu,\"rssi_dbm\":%.1f,\"name\":\"%s\"}",
        seen++ ? "," : "", p->model, p->id, (unsigned char)p->channel,
        (long long)((now - sensors[i].reading.received_us) / 1000000),
        m ? (unsigned long)m->gauge_id : 0, p->has_rain ? "true" : "false",
        p->rain_raw, (unsigned long)sensors[i].packets,
        sensors[i].reading.rssi_dbm, name);
  }
  if (o > 0 && o < PAGE_MAX) o += snprintf(json + o, PAGE_MAX - o, "]}");
  if (o < 0 || o >= PAGE_MAX) {
    free(json);
    return NULL;
  }
  return json;
}
static esp_err_t radio_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) return ESP_OK;
  return send_owned_json(req, config_server_radio_json());
}
#endif
static esp_err_t config_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) return ESP_OK;
  return send_owned_json(req, config_server_config_json());
}

// The /test handler responds first, then kicks the actual connect via this
// timer so the HTTP response flushes to the phone before the channel change
// drops it.
static esp_timer_handle_t s_test_kick;
static char s_test_ssid[33];
static char s_test_pass[65];

static void test_kick_cb(void *arg) {
  (void)arg;
  wifi_link_test_start(s_test_ssid, s_test_pass);
}

// POST /test: try the entered home-WiFi creds (blank fields fall back to
// saved).
static esp_err_t test_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) {
    return ESP_OK;
  }
  char *body = http_util_read_body(req, BODY_MAX);
  if (body == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    return ESP_OK;
  }

  const bridge_config_t *cfg = config_get();
  if (!http_util_form_get(body, "sta_ssid", s_test_ssid, sizeof(s_test_ssid)) ||
      s_test_ssid[0] == '\0') {
    snprintf(s_test_ssid, sizeof(s_test_ssid), "%s", cfg->sta_ssid);
  }
  if (!http_util_form_get(body, "sta_pass", s_test_pass, sizeof(s_test_pass)) ||
      s_test_pass[0] == '\0') {
    snprintf(s_test_pass, sizeof(s_test_pass), "%s", cfg->sta_pass);
  }
  free(body);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"s\":\"running\"}");
  // Stop first: start_once on an already-armed timer is a silent no-op error
  // (a rapid double Test click would drop the second test).
  esp_timer_stop(s_test_kick);
  esp_timer_start_once(s_test_kick, 600 * 1000);
  return ESP_OK;
}

static esp_err_t teststatus_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) {
    return ESP_OK;
  }
  const char *s;
  switch (wifi_link_test_status()) {
    case WIFI_TEST_RUNNING:
      s = "running";
      break;
    case WIFI_TEST_OK:
      s = "ok";
      break;
    case WIFI_TEST_FAIL:
      s = "fail";
      break;
    default:
      s = "idle";
      break;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "{\"s\":\"%s\"}", s);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, buf);
}

#define SCAN_JSON_MAX 2048
#define SCAN_AP_MAX 24

// Build + send a JSON array of unique networks as {"ssid":..,"rssi":..}, with
// the strongest signal per SSID. Heap buffers (not stack): ~3KB of aps + json
// would overflow the httpd task stack.
static char *scan_json(const wifi_ap_record_t *aps, int n) {
  char *json = malloc(SCAN_JSON_MAX);
  if (json == NULL) {
    return NULL;
  }
  size_t o = 0;
  json[o++] = '[';
  int added = 0;
  for (int i = 0; i < n && o < SCAN_JSON_MAX - 256; i++) {
    const char *ssid = (const char *)aps[i].ssid;
    if (ssid[0] == '\0') {
      continue;
    }
    bool dup = false;
    for (int j = 0; j < i; j++) {
      if (strcmp((const char *)aps[j].ssid, ssid) == 0) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    // Strongest signal among all APs broadcasting this SSID.
    int best = aps[i].rssi;
    for (int j = i + 1; j < n; j++) {
      if (strcmp((const char *)aps[j].ssid, ssid) == 0 && aps[j].rssi > best) {
        best = aps[j].rssi;
      }
    }
    if (added++) {
      json[o++] = ',';
    }
    char esc[6 * 33];  // SSID is <=32 bytes; worst case "\u00XX" (6) per byte
    json_escape(ssid, esc, sizeof(esc));
    o += snprintf(json + o, SCAN_JSON_MAX - o, "{\"ssid\":\"%s\",\"rssi\":%d}",
                  esc, best);
  }
  json[o++] = ']';
  json[o] = '\0';
  return json;
}

char *config_server_scan_json(bool live) {
  wifi_ap_record_t *aps = malloc(SCAN_AP_MAX * sizeof(*aps));
  if (!aps) return NULL;
  int n = live ? wifi_link_scan_live(aps, SCAN_AP_MAX)
               : wifi_link_cached_aps(aps, SCAN_AP_MAX);
  char *json = scan_json(aps, n);
  free(aps);
  return json;
}

// GET /scan: cached SSID list (instant, never disrupts the AP).
static esp_err_t scan_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) return ESP_OK;
  return send_owned_json(req, config_server_scan_json(false));
}

// GET /scanlive: on-demand live scan (silences the radio ~1-2s; the page covers
// it with a spinner). Refreshes the list even while connected.
static esp_err_t scanlive_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) return ESP_OK;
  return send_owned_json(req, config_server_scan_json(true));
}

esp_err_t config_server_save_form(const char *body, const char **error) {
  // Start from current config so blank password fields keep existing secrets.
  bridge_config_t cfg = *config_get();
  char tmp[65];
  http_util_form_get(body, "sta_ssid", cfg.sta_ssid, sizeof(cfg.sta_ssid));
  http_util_form_get(body, "ap_ssid", cfg.ap_ssid, sizeof(cfg.ap_ssid));
  if (http_util_form_get(body, "sta_pass", tmp, sizeof(tmp)) && tmp[0]) {
    snprintf(cfg.sta_pass, sizeof(cfg.sta_pass), "%s", tmp);
  }
  if (http_util_form_get(body, "ap_pass", tmp, sizeof(tmp)) && tmp[0]) {
    snprintf(cfg.ap_pass, sizeof(cfg.ap_pass), "%s", tmp);
  }

  // WU rows submit wd<i>/wu<i>/wk<i>, with rl<i> retained for legacy clients.
  // Blank keys preserve the matching device or legacy gauge credential.
  const bridge_config_t *prev = config_get();
  wu_mapping_t old[WU_MAP_MAX];
  uint8_t old_n = prev->wu_map_count;
  memcpy(old, prev->wu_map, sizeof(old));

  memset(cfg.wu_map, 0, sizeof(cfg.wu_map));
  uint8_t n = 0;
  for (int i = 0; i < WU_MAP_MAX; i++) {
    char name_rl[8], name_wu[8], name_wk[8], name_device[16];
    snprintf(name_device, sizeof(name_device), "wd%d", i);
    snprintf(name_rl, sizeof(name_rl), "rl%d", i);  // rainlog gauge id
    snprintf(name_wu, sizeof(name_wu), "wu%d", i);  // WU station id
    snprintf(name_wk, sizeof(name_wk), "wk%d", i);  // WU key
    char gid[24], wuid[64], wukey[65], device[40] = {0};
    http_util_form_get(body, name_device, device, sizeof(device));
    if (*device && !config_device_identity_valid(device)) {
      *error = "invalid WU device identity";
      return ESP_ERR_INVALID_ARG;
    }
    bool has_gid = http_util_form_get(body, name_rl, gid, sizeof(gid));
    bool has_wu = http_util_form_get(body, name_wu, wuid, sizeof(wuid));
    // The field takes the station id as Rainlog shows it ("Rainlog12345") or
    // the bare gauge number.
    uint32_t gauge = has_gid ? config_parse_gauge_id(gid, false) : 0;
    if ((!*device && gauge == 0) || !has_wu || wuid[0] == '\0') {
      continue;  // skip blank/incomplete rows
    }
    wu_mapping_t *e = &cfg.wu_map[n];
    e->gauge_id = gauge;
    snprintf(e->device, sizeof(e->device), "%s", device);
    snprintf(e->wu_id, sizeof(e->wu_id), "%s", wuid);
    if (http_util_form_get(body, name_wk, wukey, sizeof(wukey)) && wukey[0]) {
      snprintf(e->wu_key, sizeof(e->wu_key), "%s", wukey);
    } else {
      // Blank key: carry over the stored key for this gauge id, if any.
      for (uint8_t j = 0; j < old_n; j++) {
        if ((*device && !strcasecmp(old[j].device, device)) ||
            (!*old[j].device && gauge && old[j].gauge_id == gauge)) {
          snprintf(e->wu_key, sizeof(e->wu_key), "%s", old[j].wu_key);
          break;
        }
      }
    }
    n++;
  }
  cfg.wu_map_count = n;
  cfg.bridge_wifi_auto_off =
      http_util_form_get(body, "bridge_wifi_auto_off", tmp, sizeof(tmp)) ? 1
                                                                         : 0;

#if RAINLOG_RADIO
  // Radio fields are only present on radio boards.
  if (http_util_form_get(body, "radio_form", tmp, sizeof(tmp))) {
    cfg.radio_enabled =
        http_util_form_get(body, "radio_enabled", tmp, sizeof(tmp)) ? 1 : 0;
    memset(cfg.radio_map, 0, sizeof(cfg.radio_map));
    cfg.radio_map_count = 0;
    for (unsigned i = 0; i < RADIO_MAP_MAX; i++) {
      char name[24], model[16], id[24], channel[8], gauge[24], key[65];
#define RADIO_FIELD(field, dest)                          \
  do {                                                    \
    snprintf(name, sizeof(name), "radio_%s%u", field, i); \
    http_util_form_get(body, name, dest, sizeof(dest));   \
  } while (0)
      model[0] = id[0] = channel[0] = gauge[0] = key[0] = 0;
      RADIO_FIELD("model", model);
      RADIO_FIELD("id", id);
      RADIO_FIELD("channel", channel);
      RADIO_FIELD("gauge", gauge);
      RADIO_FIELD("key", key);
#undef RADIO_FIELD
      if (!id[0] && !gauge[0] && !key[0]) continue;
      radio_mapping_t *m = &cfg.radio_map[cfg.radio_map_count++];
      // Sensor ID zero is valid; parse via the shared strict decimal parser.
      m->sensor_id = config_parse_gauge_id(id, false);
      if (strspn(id, "0123456789") != strlen(id) ||
          (!m->sensor_id && strcmp(id, "0")) ||
          (!strcmp(model, "0") && m->sensor_id > 127) ||
          (strcmp(model, "0") && strcmp(model, "1"))) {
        *error = "Invalid radio sensor ID or model";
        return ESP_ERR_INVALID_ARG;
      }
      if (!strcmp(model, "1") && strlen(channel) != 1) {
        *error = "Invalid radio channel";
        return ESP_ERR_INVALID_ARG;
      }
      m->model = model[0] - '0';
      m->channel = m->model == WEATHER_ACURITE_5N1 ? channel[0] : 0;
      m->gauge_id = config_parse_gauge_id(gauge, false);
      if (key[0])
        snprintf(m->rainlog_key, sizeof(m->rainlog_key), "%s", key);
      else
        for (unsigned j = 0; j < prev->radio_map_count; j++) {
          const radio_mapping_t *old = &prev->radio_map[j];
          if (old->model == m->model && old->sensor_id == m->sensor_id &&
              old->channel == m->channel && old->gauge_id == m->gauge_id)
            snprintf(m->rainlog_key, sizeof(m->rainlog_key), "%s",
                     old->rainlog_key);
        }
    }
  }
#endif
  *error = ap_pass_is_default(cfg.ap_pass)
               ? "Please choose a Bridge Wi-Fi password (the default cannot be "
                 "kept)."
               : config_validate(&cfg);
  if (*error) return ESP_ERR_INVALID_ARG;
  esp_err_t err = config_save(&cfg);
  if (err != ESP_OK) *error = "save failed";
  return err;
}
void config_server_restart(void) {
  // Give HTTP or serial responses 1.5 s to flush before applying Wi-Fi changes.
  esp_timer_stop(s_restart_timer);
  esp_timer_start_once(s_restart_timer, 1500 * 1000);
}
static esp_err_t save_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) return ESP_OK;
  char *body = http_util_read_body(req, BODY_MAX);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    return ESP_OK;
  }
  const char *error = NULL;
  esp_err_t err = config_server_save_form(body, &error);
  free(body);
  if (err == ESP_ERR_INVALID_ARG) return send_error_page(req, error);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req, SAVED_PAGE);
  config_server_restart();
  return ESP_OK;
}

static void restart_cb(void *arg) {
  (void)arg;
  esp_restart();
}

// Eight rows can exceed 6 KiB when names/hostnames/rejection text all escape.
#define CLIENTS_JSON_MAX 12288

// GET /clients: the devices on the bridge's SoftAP, for the page's Devices
// tab. Each row: MAC, OUI vendor label, user-given name, DHCP-announced
// hostname, last known IP, live RSSI, connected flag, seconds since last
// seen, whether the row is the requester itself ("you", so the user can
// tell their phone from the weather station), this boot's upload counts
// (rx = captured, rl = accepted by Rainlog, wu = relayed to WU) and the
// sticky AP_CLIENT_ERR_* flags. name/hostname originate outside the
// firmware, so they are JSON-escaped; the rest is firmware-formatted.
char *config_server_clients_json(uint32_t peer) {
  ap_client_t list[AP_CLIENTS_MAX];
  int n = ap_clients_snapshot(list, AP_CLIENTS_MAX);
  int64_t now = esp_timer_get_time();

  char *json = malloc(CLIENTS_JSON_MAX);
  if (json == NULL) {
    return NULL;
  }
  int o = snprintf(json, CLIENTS_JSON_MAX, "[");
  for (int i = 0; i < n && o > 0 && o < CLIENTS_JSON_MAX; i++) {
    const ap_client_t *c = &list[i];
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", c->mac[0],
             c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5]);
    char ip[16] = "";
    if (c->ip.addr != 0) {
      esp_ip4addr_ntoa(&c->ip, ip, sizeof(ip));
    }
    char name[sizeof(c->name) * 6];
    char host[sizeof(c->hostname) * 6];
    char rlrej[sizeof(c->rl_reject) * 6];
    char wurej[sizeof(c->wu_reject) * 6];
    json_escape(c->name, name, sizeof(name));
    json_escape(c->hostname, host, sizeof(host));
    json_escape(c->rl_reject, rlrej, sizeof(rlrej));
    json_escape(c->wu_reject, wurej, sizeof(wurej));
    o += snprintf(json + o, CLIENTS_JSON_MAX - o,
                  "%s{\"mac\":\"%s\",\"vendor\":\"%s\",\"name\":\"%s\","
                  "\"hostname\":\"%s\",\"ip\":\"%s\","
                  "\"rssi\":%d,\"connected\":%s,\"age_s\":%lld,\"you\":%s,"
                  "\"rx\":%lu,\"rl\":%lu,\"wu\":%lu,\"err\":%u,"
                  "\"rlrej\":\"%s\",\"wurej\":\"%s\",\"gauge_id\":%lu}",
                  i ? "," : "", mac, c->vendor, name, host, ip, c->rssi,
                  c->connected ? "true" : "false",
                  (long long)((now - c->last_seen_us) / 1000000),
                  (peer != 0 && c->ip.addr == peer) ? "true" : "false",
                  (unsigned long)c->rx_count, (unsigned long)c->fwd_count,
                  (unsigned long)c->wu_count, (unsigned)c->err_flags, rlrej,
                  wurej, (unsigned long)c->gauge_id);
  }
  if (o > 0 && o < CLIENTS_JSON_MAX) {
    o += snprintf(json + o, CLIENTS_JSON_MAX - o, "]");
  }
  return json;
}

static esp_err_t clients_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) return ESP_OK;
  return send_owned_json(req,
                         config_server_clients_json(http_util_peer_ip4(req)));
}

// Parse "AA:BB:CC:DD:EE:FF" into 6 bytes. Returns false on malformed input.
static bool parse_mac(const char *s, uint8_t out[6]) {
  if (strlen(s) != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i % 3 == 2) {
      if (s[i] != ':') return false;
    } else if (!isxdigit((unsigned char)s[i]))
      return false;
  }
  return sscanf(s, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &out[0], &out[1],
                &out[2], &out[3], &out[4], &out[5]) == 6;
}
esp_err_t config_server_rename(const char *device_identity, const char *name) {
#if RAINLOG_RADIO
  if (!strncmp(device_identity, "radio:", 6)) {
    unsigned model, id, channel;
    int end = 0;
    if (sscanf(device_identity, "radio:%u:%u:%u%n", &model, &id, &channel, &end) != 3 ||
        device_identity[end] || model > WEATHER_ACURITE_5N1 || id > UINT16_MAX ||
        channel > UINT8_MAX || strlen(name) > 32) return ESP_ERR_INVALID_ARG;
    char key[16];
    radio_name_key(model, id, channel, key);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("radio_names", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = *name ? nvs_set_str(handle, key, name) : nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
  }
#endif
  uint8_t mac[6];
  if (!parse_mac(device_identity, mac) || strlen(name) > 32)
    return ESP_ERR_INVALID_ARG;
  return ap_clients_set_name(mac, name);
}

// POST /rename: set a Wi-Fi or radio device name (form fields mac, name;
// an empty or absent name clears it). Persisted across reboots.
static esp_err_t rename_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) {
    return ESP_OK;
  }
  char *body = http_util_read_body(req, BODY_MAX);
  if (body == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    return ESP_OK;
  }
  char mac_str[40];
  char name[33];
  bool has_mac = http_util_form_get(body, "mac", mac_str, sizeof(mac_str));
  if (!http_util_form_get(body, "name", name, sizeof(name))) {
    name[0] = '\0';
  }
  free(body);

  esp_err_t err =
      has_mac ? config_server_rename(mac_str, name) : ESP_ERR_INVALID_ARG;
  if (err == ESP_ERR_INVALID_ARG) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad device identity");
    return ESP_OK;
  }
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"s\":\"ok\"}");
}

// GET /ota/status: current OTA state (phase, running/latest version, progress).
// The page polls this to show "update available" and live download progress.
static esp_err_t ota_status_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) {
    return ESP_OK;
  }
  char buf[OTA_STATUS_JSON_MAX];
  ota_update_status_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, buf);
}

// Close failed uploads so unread binary bytes cannot become another request.
static esp_err_t upload_error(httpd_req_t *req, const char *status,
                              const char *error) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Connection", "close");
  char reply[192];
  snprintf(reply, sizeof(reply), "{\"error\":\"%s\"}", error);
  httpd_resp_sendstr(req, reply);
  return ESP_FAIL;
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) return ESP_FAIL;
  char intent[8], type[40];
  // A custom header and binary content type prevent cross-origin form posts.
  // The config server does not grant cross-origin preflight requests.
  if (httpd_req_get_hdr_value_str(req, "X-Rainlog-OTA", intent, sizeof(intent)) != ESP_OK ||
      strcmp(intent, "1") ||
      httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
      strcmp(type, "application/octet-stream"))
    return upload_error(req, "400 Bad Request", "Use the manual firmware uploader");
  const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
  if (!slot || req->content_len < FIRMWARE_PREFIX_SIZE || req->content_len > slot->size)
    return upload_error(req, "400 Bad Request", "Invalid firmware file size");
  uint8_t prefix[FIRMWARE_PREFIX_SIZE];
  size_t received = 0;
  while (received < sizeof(prefix)) {
    int n = httpd_req_recv(req, (char *)prefix + received, sizeof(prefix) - received);
    if (n <= 0) return upload_error(req, "400 Bad Request", "Firmware upload interrupted");
    received += n;
  }
  ota_manual_t *upload;
  const char *error = NULL;
  esp_err_t err = ota_manual_begin(prefix, sizeof(prefix), req->content_len, &upload, &error);
  if (err != ESP_OK)
    return upload_error(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "400 Bad Request", error);
  err = ota_manual_write(upload, prefix, sizeof(prefix));
  uint8_t buffer[1024];
  while (err == ESP_OK && received < req->content_len) {
    size_t remaining = req->content_len - received;
    int n = httpd_req_recv(req, (char *)buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
    if (n <= 0) { err = ESP_FAIL; break; }
    err = ota_manual_write(upload, buffer, n);
    received += n;
  }
  if (err != ESP_OK) {
    ota_manual_abort(upload);
    return upload_error(req, "400 Bad Request", "Upload failed; current firmware retained");
  }
  if (ota_manual_finish(upload, &error) != ESP_OK)
    return upload_error(req, "400 Bad Request", error);
  httpd_resp_set_type(req, "application/json");
  esp_err_t sent = httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
  config_server_restart();
  return sent;
}

// POST /ota/check: kick a manifest check now (non-blocking). The page then
// polls /ota/status for the result.
static esp_err_t ota_check_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) {
    return ESP_OK;
  }
  ota_update_request_check();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"s\":\"checking\"}");
}

// POST /ota/apply: download + install the newest firmware now (non-blocking;
// the device reboots into it on success). The page polls /ota/status for
// progress until the connection drops at reboot.
static esp_err_t ota_apply_handler(httpd_req_t *req) {
  if (reject_unauthorized(req)) {
    return ESP_OK;
  }
  ota_update_request_apply();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"s\":\"updating\"}");
}

// Catch-all for any unmatched path. On the SoftAP side, redirect to the config
// page: this makes the OS captive-portal probe (resolved to our AP IP by the
// captive DNS) pop the "sign in to network" page every time a device connects.
// On the LAN side, a plain 404 (no signposting the config page).
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
  (void)err;
  // Log the path: a console probing an unexpected URL (and getting this
  // handler's redirect instead of what it wanted) is otherwise invisible.
  ESP_LOGW(TAG, "unmatched request: %s", req->uri);
  if (!http_util_from_softap(req)) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
  }
  char ip[16];
  wifi_link_ap_ip_str(ip, sizeof(ip));
  // Stays in scope through httpd_resp_send (set_hdr stores the pointer).
  char location[32];
  snprintf(location, sizeof(location), "http://%s/", ip);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location);
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

void config_server_start(void) {
  httpd_handle_t server = capture_server_httpd();
  if (server == NULL) {
    ESP_LOGE(TAG, "shared httpd not started");
    return;
  }
  const esp_timer_create_args_t restart_args = {.callback = restart_cb,
                                                .name = "cfg_restart"};
  ESP_ERROR_CHECK(esp_timer_create(&restart_args, &s_restart_timer));
  const esp_timer_create_args_t kick_args = {.callback = test_kick_cb,
                                             .name = "cfg_test_kick"};
  ESP_ERROR_CHECK(esp_timer_create(&kick_args, &s_test_kick));

  const httpd_uri_t page = {
      .uri = "/", .method = HTTP_GET, .handler = page_handler};
  const httpd_uri_t favicon = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};
  const httpd_uri_t config = {
      .uri = "/config", .method = HTTP_GET, .handler = config_handler};
  const httpd_uri_t save = {
      .uri = "/save", .method = HTTP_POST, .handler = save_handler};
  const httpd_uri_t scan = {
      .uri = "/scan", .method = HTTP_GET, .handler = scan_handler};
  const httpd_uri_t scanlive = {
      .uri = "/scanlive", .method = HTTP_GET, .handler = scanlive_handler};
  const httpd_uri_t test = {
      .uri = "/test", .method = HTTP_POST, .handler = test_handler};
  const httpd_uri_t teststatus = {
      .uri = "/teststatus", .method = HTTP_GET, .handler = teststatus_handler};
  const httpd_uri_t clients = {
      .uri = "/clients", .method = HTTP_GET, .handler = clients_handler};
  const httpd_uri_t rename = {
      .uri = "/rename", .method = HTTP_POST, .handler = rename_handler};
  const httpd_uri_t ota_status = {
      .uri = "/ota/status", .method = HTTP_GET, .handler = ota_status_handler};
  const httpd_uri_t ota_check = {
      .uri = "/ota/check", .method = HTTP_POST, .handler = ota_check_handler};
  const httpd_uri_t ota_upload = {
      .uri = "/ota/upload", .method = HTTP_POST, .handler = ota_upload_handler};
  const httpd_uri_t ota_apply = {
      .uri = "/ota/apply", .method = HTTP_POST, .handler = ota_apply_handler};
  // Login/logout are deliberately ungated (login IS the gate; logout only
  // drops the caller's own session).
  const httpd_uri_t login = {.uri = "/login",
                             .method = HTTP_POST,
                             .handler = session_auth_login_handler};
  const httpd_uri_t logout = {.uri = "/logout",
                              .method = HTTP_POST,
                              .handler = session_auth_logout_handler};
#if RAINLOG_RADIO
  const httpd_uri_t radio = {
      .uri = "/radio", .method = HTTP_GET, .handler = radio_handler};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &radio));
#endif
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &page));
  const char *const page_routes[] = {"/setup", "/devices", "/firmware"};
  for (size_t i = 0; i < sizeof(page_routes) / sizeof(page_routes[0]); i++) {
    const httpd_uri_t route = {
        .uri = page_routes[i], .method = HTTP_GET, .handler = page_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &route));
  }
  httpd_register_uri_handler(server, &favicon);
  httpd_register_uri_handler(server, &config);
  httpd_register_uri_handler(server, &save);
  httpd_register_uri_handler(server, &scan);
  httpd_register_uri_handler(server, &scanlive);
  httpd_register_uri_handler(server, &test);
  httpd_register_uri_handler(server, &teststatus);
  httpd_register_uri_handler(server, &clients);
  httpd_register_uri_handler(server, &rename);
  httpd_register_uri_handler(server, &ota_status);
  httpd_register_uri_handler(server, &ota_check);
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_upload));
  httpd_register_uri_handler(server, &ota_apply);
  httpd_register_uri_handler(server, &login);
  httpd_register_uri_handler(server, &logout);
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);
  char ip[16];
  wifi_link_ap_ip_str(ip, sizeof(ip));
  ESP_LOGI(TAG, "config page at http://%s/", ip);
}
