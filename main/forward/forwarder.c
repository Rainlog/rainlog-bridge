#include "forwarder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ap_clients.h"
#include "board.h"
#include "config_store.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fs.h"
#include "http_util.h"
#include "radio_upload.h"
#include "status_led.h"
#include "upload_stats.h"
#include "wifi_link.h"

static const char *TAG = "forwarder";
static char s_bridge_id[13];

// Captured station query (1024 bytes) plus the trusted device-identity tag.
#define QUERY_BUF (1024 + 64)
#define FORWARD_QUEUE_LEN 8
// The worker runs the TLS handshake (mbedtls) inline plus two QUERY_BUF
// buffers; the default-size stack would overflow.
#define FORWARDER_TASK_STACK 10240

// Store-and-forward: an upload that fails to reach a target is kept here and
// retried, so a WiFi/uplink blip does not lose a rain reading. Rainlog stores
// the reading at its own dateutc timestamp, so a delayed forward still lands at
// the right time. Per-target flags mean a reading whose Rainlog forward
// succeeded but whose WU relay failed only re-sends the WU half on retry.
//
// The buffer lives in RAM uncompressed: a query is ~250 B, so the whole buffer
// is a few KB, far cheaper than the deflate working memory that would compete
// with the TLS handshake heap. It is also mirrored to LittleFS so a reboot
// (OTA, crash, power blip) does not drop undelivered readings.
//
// Three bounds keep it from hoarding stale data: a count cap (oldest dropped
// when full), an attempt cap, and an age cap. Drained NEWEST-first so the
// freshest reading wins Rainlog's one-accept-per-300s slot; with cumulative
// rain fields the newest carries the full total, and older entries clear as
// they hit RATELIMIT in the same pass.
#define RETRY_MAX_ENTRIES 24
#define RETRY_MAX_ATTEMPTS \
  30  // give up after this many live attempts (uplink up)
#define RETRY_INTERVAL_MS 60000
// Drop a buffered reading older than this: delivered dateutc=now it would land
// hours off, and for cumulative rain it is superseded by anything newer.
#define RETRY_MAX_AGE_S (6 * 3600)

// Persisted copy of the buffer (see fs.c). Written atomically, throttled.
#define RETRY_FILE FS_BASE "/retry.bin"
#define RETRY_TMP FS_BASE "/retry.tmp"
#define RETRY_MAGIC 0x31524C52u  // "RLR1"
#define RETRY_VERSION 1
#define RETRY_PERSIST_MIN_US ((int64_t)15 * 1000000)
// SNTP unsynced below this wall-clock value (ages have no anchor without it).
#define WALL_CLOCK_MIN_UNIX 1600000000  // 2020-09-13

typedef struct {
  char *query;      // malloc'd untagged upload, owned by the buffer
  uint32_t src_ip;  // uploading station's SoftAP address; 0 = unknown
  int64_t
      captured_wall;  // unix seconds at capture (0 if no clock yet); age cap
  bool rainlog_done;  // got an HTTP 200 from Rainlog (accepted or rejected)
  bool wu_done;       // got 200 from WU, or this gauge has no WU mapping
  uint16_t attempts;  // live send attempts so far (only counted uplink up)
} pending_t;

// One captured upload in flight to the worker task: the source station's
// address plus the query string, in a single malloc'd block.
typedef struct {
  uint32_t src_ip;
  char query[];
} fwd_msg_t;

// Queue of malloc'd fwd_msg_t pointers, owned by the worker task once
// enqueued.
static QueueHandle_t s_queue;

// Retry buffer + its drain cadence. Touched only by the forwarder task.
static pending_t s_pending[RETRY_MAX_ENTRIES];
static int s_pending_count;
static int64_t s_last_retry_us;
static bool s_buffer_dirty;        // buffer changed since the last persist
static int64_t s_last_persist_us;  // throttle for the LittleFS write

static bool wall_clock_valid(void) {
  return time(NULL) >= (time_t)WALL_CLOCK_MIN_UNIX;
}

static const char *s_last_result = "--";
static int64_t s_last_upload_us;  // esp_timer time; 0 = never

// ---- helpers --------------------------------------------------------------

static bool is_unreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static void url_encode(const char *in, char *out, size_t outlen) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const char *p = in; *p && o + 4 < outlen; p++) {
    unsigned char c = (unsigned char)*p;  // no sign extension for bytes >= 128
    if (is_unreserved(*p)) {
      out[o++] = *p;
    } else {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 0xF];
    }
  }
  out[o] = '\0';
}

// Case-insensitive match of the first `klen` chars of `tok` against `name`.
static bool key_is(const char *tok, size_t klen, const char *name) {
  return strlen(name) == klen && strncasecmp(tok, name, klen) == 0;
}

// Build a forward query: the given station_id/key as ID/PASSWORD plus every
// param from the station's upload except its own ID/PASSWORD. Returns query
// length, or -1 on overflow. Used for both the Rainlog and the WU relay, each
// with its own configured credentials.
static int build_query(const char *station_id, const char *key, const char *raw,
                       char *out, size_t outlen) {
  char enc_id[128];
  char enc_key[160];
  url_encode(station_id, enc_id, sizeof(enc_id));
  url_encode(key, enc_key, sizeof(enc_key));
  int n = snprintf(out, outlen, "ID=%s&PASSWORD=%s", enc_id, enc_key);
  if (n < 0 || (size_t)n >= outlen) {
    return -1;
  }

  char *copy = strdup(raw);
  if (copy == NULL) {
    return -1;
  }
  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, "&", &saveptr); tok != NULL;
       tok = strtok_r(NULL, "&", &saveptr)) {
    const char *eq = strchr(tok, '=');
    size_t klen = eq ? (size_t)(eq - tok) : strlen(tok);
    if (key_is(tok, klen, "ID") || key_is(tok, klen, "PASSWORD") ||
        key_is(tok, klen, "wu_device") || key_is(tok, klen, "rlsource") || key_is(tok, klen, "sensor_model") ||
        key_is(tok, klen, "sensor_id") || key_is(tok, klen, "sensor_channel") ||
        key_is(tok, klen, "bridge_model") || key_is(tok, klen, "bridge_id")) {
      continue;
    }
    int m = snprintf(out + n, outlen - n, "&%s", tok);
    if (m < 0 || (size_t)(n + m) >= outlen) {
      free(copy);
      return -1;
    }
    n += m;
  }
  free(copy);
  return n;
}

// Device identity travels in the existing query/retry format, so old retry
// files still load and new ones retain MAC identity across DHCP changes.
static void upload_device(const char *query, uint32_t src_ip, char out[40]) {
  out[0] = 0;
  if (http_util_form_get(query, "wu_device", out, 40) &&
      config_device_identity_valid(out)) return;
  char model[32], id[16], channel[4] = {0};
  if (http_util_form_get(query, "sensor_model", model, sizeof(model)) &&
      http_util_form_get(query, "sensor_id", id, sizeof(id))) {
    http_util_form_get(query, "sensor_channel", channel, sizeof(channel));
    unsigned protocol = !strcmp(model, "LaCrosse-TX5U") ? 0 : 1;
    snprintf(out, 40, "radio:%u:%lu:%u", protocol, strtoul(id, NULL, 10),
             (unsigned char)channel[0]);
    if (config_device_identity_valid(out)) return;
  }
  uint8_t mac[6];
  if (ap_clients_mac_for_ip(src_ip, mac))
    snprintf(out, 40, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
  else out[0] = 0;
}

// HTTPS GET to host+path?query, capturing up to resp_cap bytes of the body.
// Returns true on a 200 response (the only status either upstream treats as
// delivered).
static bool https_get(const char *host, const char *path, const char *query,
                      char *resp, size_t resp_cap) {
  int status = 0;
  return http_util_https_get(host, path, query, resp, resp_cap, &status) &&
         status == 200;
}

// Pull the Rainlog gauge id out of an upload whose station id is the literal
// "Rainlog" + the gauge number (ID=Rainlog12345). Returns 0 if the ID field is
// absent or not in that form.
static uint32_t parse_rainlog_gauge_id(const char *raw) {
  char *copy = strdup(raw);
  if (copy == NULL) {
    return 0;
  }
  uint32_t gauge = 0;
  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, "&", &saveptr); tok != NULL;
       tok = strtok_r(NULL, "&", &saveptr)) {
    const char *eq = strchr(tok, '=');
    size_t klen = eq ? (size_t)(eq - tok) : strlen(tok);
    if (key_is(tok, klen, "ID") && eq != NULL) {
      gauge = config_parse_gauge_id(eq + 1, true);
      break;
    }
  }
  free(copy);
  return gauge;
}

// True if the upload's dateutc is the literal "now" (the station has no real
// clock / uses receipt time). Such readings are timestamped by Rainlog on
// arrival, so consecutive ones within a rate-limit window are interchangeable
// and the intermediate ones can be dropped (see the throttle below). A real
// dateutc means distinct moments, so those are never throttled.
static bool dateutc_is_now(const char *raw) {
  char *copy = strdup(raw);
  if (copy == NULL) {
    return false;  // can't tell; treat as real (never throttle)
  }
  bool is_now = false;
  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, "&", &saveptr); tok != NULL;
       tok = strtok_r(NULL, "&", &saveptr)) {
    const char *eq = strchr(tok, '=');
    size_t klen = eq ? (size_t)(eq - tok) : strlen(tok);
    if (key_is(tok, klen, "dateutc") && eq != NULL) {
      is_now = strcasecmp(eq + 1, "now") == 0;
      break;
    }
  }
  free(copy);
  return is_now;
}

// Rainlog accepts at most one reading per gauge every 300s; a RapidFire console
// (e.g. AcuRite) uploads every ~18s, so all but one per window are rejected as
// RATELIMIT - wasting a TLS handshake each time. Throttle the Rainlog forward
// to one per gauge per window, but only for dateutc=now readings (dropping an
// intermediate "now" snapshot loses nothing; it is full state and timestamped
// on receipt). 305s leaves a margin over Rainlog's 300s so we don't race its
// edge. Per gauge so distinct stations don't throttle each other; the WU relay
// is never throttled (its RapidFire wants every upload).
#define RL_THROTTLE_US ((int64_t)305 * 1000000)
#define RL_THROTTLE_GAUGES 8
static struct {
  uint32_t gauge;     // 0 = empty slot
  int64_t last_send;  // esp_timer time of the last Rainlog forward attempt
} s_rl_throttle[RL_THROTTLE_GAUGES];

// True if a dateutc=now forward to this gauge should be skipped (one went out
// less than a window ago). gauge 0 (unparseable id) is never throttled.
static bool rl_throttled(uint32_t gauge, int64_t now) {
  if (gauge == 0) {
    return false;
  }
  for (int i = 0; i < RL_THROTTLE_GAUGES; i++) {
    if (s_rl_throttle[i].gauge == gauge) {
      return now - s_rl_throttle[i].last_send < RL_THROTTLE_US;
    }
  }
  return false;
}

// Record that a Rainlog forward just went out for this gauge, opening its
// window. Reuses the gauge's slot if present, else the oldest slot (an empty
// slot has last_send 0, so it is naturally the oldest and gets picked first).
static void rl_mark_sent(uint32_t gauge, int64_t now) {
  if (gauge == 0) {
    return;
  }
  int oldest = 0;
  for (int i = 0; i < RL_THROTTLE_GAUGES; i++) {
    if (s_rl_throttle[i].gauge == gauge) {
      oldest = i;
      break;
    }
    if (s_rl_throttle[i].last_send < s_rl_throttle[oldest].last_send) {
      oldest = i;
    }
  }
  s_rl_throttle[oldest].gauge = gauge;
  s_rl_throttle[oldest].last_send = now;
}

// Send one captured or encoded radio upload to Rainlog. The console is configured with its
// Rainlog station id + key directly, so the upload is already correct; we pass
// it through, appending firmware, board model and factory-MAC identity so the
// backend can identify the wireless bridge. Only the
// Rainlog forward is tagged; the WU relay never sees our private param. If the
// tagged query would overflow, fall back to the untagged query rather than
// truncate real reading params.
//
// Returns true if Rainlog returned HTTP 200 (whether or not it accepted the
// reading) - i.e. the reading was delivered to the server. A false return is a
// transport/non-200 failure worth retrying. Updates the LED / stats / result.
static bool try_rainlog(const char *raw_query, uint32_t src_ip) {
  if (!parse_rainlog_gauge_id(raw_query)) return true;
  char rl_query[QUERY_BUF + 256];
  const char *rl_send = raw_query;
  char model[128];
  url_encode(BOARD_ID, model, sizeof(model));
  int n = snprintf(rl_query, sizeof(rl_query),
                   "%s&rlbridge=%.16s&bridge_model=%s&bridge_id=%s", raw_query,
                   esp_app_get_description()->version, model, s_bridge_id);
  if (n > 0 && (size_t)n < sizeof(rl_query)) {
    rl_send = rl_query;
  }
  char body[64];
  bool ok = https_get(config_get()->rainlog_host, config_get()->wu_update_path,
                      rl_send, body, sizeof(body));
  bool accepted = ok && strstr(body, "success") != NULL;
  s_last_upload_us = esp_timer_get_time();
  if (accepted) {
    s_last_result = "OK";
    upload_stats_record_success(UPLOAD_TARGET_RL);
    ap_clients_note_forwarded(src_ip);
    // Delivered and accepted: both Rainlog problems are resolved.
    ap_clients_clear_error(src_ip,
                           AP_CLIENT_ERR_RL_FAIL | AP_CLIENT_ERR_RL_REJECT);
    status_led_flash_success();
    status_led_set_error(false);
    ESP_LOGI(TAG, "Rainlog accepted (24h=%d)",
             upload_stats_count_last_24h(UPLOAD_TARGET_RL));
  } else if (!ok) {
    // Transport/uplink failure: surface as an error and let the caller buffer.
    s_last_result = "FAIL";
    ap_clients_note_error(src_ip, AP_CLIENT_ERR_RL_FAIL, NULL);
    status_led_set_error(true);
    ESP_LOGW(TAG, "Rainlog forward failed (no 200); buffering for retry");
  } else {
    // 200 but not "success" (e.g. RATELIMIT, INVALIDPASSWORDID). The server saw
    // it, so retrying would just resend a reading it already declined: treat as
    // delivered. Do not latch red on a possibly-transient rate limit. We
    // reached Rainlog, so clear any stale unreachable flag; keep the body as
    // the reject detail behind the reject flag.
    s_last_result = "REJECT";
    ap_clients_clear_error(src_ip, AP_CLIENT_ERR_RL_FAIL);
    ap_clients_note_error(src_ip, AP_CLIENT_ERR_RL_REJECT, body);
    ESP_LOGW(TAG, "Rainlog did not accept: %s", body);
  }
  return ok;
}

// Relay one upload to the real Weather Underground under the WU credentials
// mapped to this gauge (rewriting ID/PASSWORD; the rest of the reading is
// passed on). The mapping is re-resolved on every call so a config change
// between retries is picked up. Returns true if there is nothing left to do:
// no mapping, accepted, or WU answered but declined (a reject would just be
// re-declined, so it is not worth retrying - mirroring try_rainlog). Only a
// transport failure (WU unreachable) returns false for the retry buffer.
static bool try_wu(const char *raw_query, uint32_t src_ip) {
  uint32_t gauge = parse_rainlog_gauge_id(raw_query);
  char device[40];
  upload_device(raw_query, src_ip, device);
  const wu_mapping_t *m = config_find_wu_device(device, gauge);
  if (m == NULL || m->wu_id[0] == '\0') {
    return true;  // no WU relay for this gauge
  }
  char wu_query[QUERY_BUF];
  if (build_query(m->wu_id, m->wu_key, raw_query, wu_query, sizeof(wu_query)) <
      0) {
    return true;  // query won't fit; no retry will change that
  }
  char body[64];
  int status = 0;
  bool transport =
      http_util_https_get(config_get()->wu_host, config_get()->wu_update_path,
                          wu_query, body, sizeof(body), &status);
  bool accepted = transport && status == 200 && strstr(body, "success") != NULL;
  if (accepted) {
    upload_stats_record_success(UPLOAD_TARGET_WU);
    ap_clients_note_wu_forwarded(src_ip);
    ap_clients_clear_error(src_ip,
                           AP_CLIENT_ERR_WU_FAIL | AP_CLIENT_ERR_WU_REJECT);
    ESP_LOGI(TAG, "WU forward (gauge %lu -> %s) ok", (unsigned long)gauge,
             m->wu_id);
  } else if (!transport) {
    ap_clients_note_error(src_ip, AP_CLIENT_ERR_WU_FAIL, NULL);
    ESP_LOGW(TAG, "WU forward (gauge %lu -> %s) unreachable; buffering",
             (unsigned long)gauge, m->wu_id);
  } else {
    // Reached WU (got a response), so clear any stale unreachable flag. A
    // 200-but-not-success has a body reason; a non-200 has none, so fall back
    // to a short "HTTP <code>" so the detail is never empty.
    ap_clients_clear_error(src_ip, AP_CLIENT_ERR_WU_FAIL);
    char httpmsg[16];
    const char *detail = body;
    if (body[0] == '\0') {
      snprintf(httpmsg, sizeof(httpmsg), "HTTP %d", status);
      detail = httpmsg;
    }
    ap_clients_note_error(src_ip, AP_CLIENT_ERR_WU_REJECT, detail);
    ESP_LOGW(TAG, "WU did not accept (gauge %lu -> %s, status %d): %s",
             (unsigned long)gauge, m->wu_id, status, body);
  }
  return transport;
}

// Remove the buffer entry at index i, freeing its query. Entries above i shift
// down. Safe in a reverse (newest-first) loop: lower indices are untouched.
static void buffer_remove(int i) {
  free(s_pending[i].query);
  memmove(&s_pending[i], &s_pending[i + 1],
          (s_pending_count - i - 1) * sizeof(pending_t));
  s_pending_count--;
  s_buffer_dirty = true;
}

// Buffer an upload that did not fully deliver, dropping the oldest entry if the
// buffer is full. Takes ownership of a fresh copy of raw_query.
static void buffer_add(const char *raw_query, uint32_t src_ip,
                       bool rainlog_done, bool wu_done) {
  char *copy = strdup(raw_query);
  if (copy == NULL) {
    return;  // out of memory; reading lost, but better than a crash
  }
  if (s_pending_count == RETRY_MAX_ENTRIES) {
    ESP_LOGW(TAG, "retry buffer full; dropping oldest reading");
    buffer_remove(0);
  }
  int64_t wall = (int64_t)time(NULL);
  s_pending[s_pending_count++] = (pending_t){
      .query = copy,
      .src_ip = src_ip,
      .captured_wall = wall_clock_valid() ? wall : 0,
      .rainlog_done = rainlog_done,
      .wu_done = wu_done,
      .attempts = 1,  // the live attempt that just failed counts
  };
  s_buffer_dirty = true;
  ESP_LOGI(TAG, "buffered upload for retry (%d pending)", s_pending_count);
}

// ---- buffer persistence (LittleFS) ----------------------------------------

// Serialize the buffer to RETRY_FILE via temp + rename (atomic). Variable
// length: header {magic, version, count} then per entry the fixed fields plus
// the query string.
static void retry_save(void) {
  FILE *fp = fopen(RETRY_TMP, "wb");
  if (fp == NULL) {
    return;
  }
  uint32_t magic = RETRY_MAGIC;
  uint16_t ver = RETRY_VERSION, cnt = (uint16_t)s_pending_count;
  bool ok = fwrite(&magic, 4, 1, fp) == 1 && fwrite(&ver, 2, 1, fp) == 1 &&
            fwrite(&cnt, 2, 1, fp) == 1;
  for (int i = 0; ok && i < s_pending_count; i++) {
    pending_t *e = &s_pending[i];
    uint16_t qlen = (uint16_t)strlen(e->query);
    uint8_t rd = e->rainlog_done, wd = e->wu_done;
    ok = fwrite(&e->captured_wall, 8, 1, fp) == 1 &&
         fwrite(&e->src_ip, 4, 1, fp) == 1 && fwrite(&rd, 1, 1, fp) == 1 &&
         fwrite(&wd, 1, 1, fp) == 1 && fwrite(&e->attempts, 2, 1, fp) == 1 &&
         fwrite(&qlen, 2, 1, fp) == 1 && fwrite(e->query, 1, qlen, fp) == qlen;
  }
  ok = (fclose(fp) == 0) && ok;
  if (ok && rename(RETRY_TMP, RETRY_FILE) == 0) {
    return;
  }
  ESP_LOGW(TAG, "retry buffer save failed");
  remove(RETRY_TMP);
}

// Reload the buffer at boot. Stops at the first malformed/short record (loads
// whatever was intact before it). Caps at RETRY_MAX_ENTRIES.
static void retry_load(void) {
  FILE *fp = fopen(RETRY_FILE, "rb");
  if (fp == NULL) {
    return;  // nothing persisted
  }
  uint32_t magic = 0;
  uint16_t ver = 0, cnt = 0;
  if (fread(&magic, 4, 1, fp) != 1 || fread(&ver, 2, 1, fp) != 1 ||
      fread(&cnt, 2, 1, fp) != 1 || magic != RETRY_MAGIC ||
      ver != RETRY_VERSION) {
    fclose(fp);
    return;
  }
  for (uint16_t i = 0; i < cnt && s_pending_count < RETRY_MAX_ENTRIES; i++) {
    int64_t cw = 0;
    uint32_t ip = 0;
    uint8_t rd = 0, wd = 0;
    uint16_t att = 0, qlen = 0;
    if (fread(&cw, 8, 1, fp) != 1 || fread(&ip, 4, 1, fp) != 1 ||
        fread(&rd, 1, 1, fp) != 1 || fread(&wd, 1, 1, fp) != 1 ||
        fread(&att, 2, 1, fp) != 1 || fread(&qlen, 2, 1, fp) != 1) {
      break;
    }
    if (qlen == 0 || qlen >= QUERY_BUF) {
      break;  // garbage length
    }
    char *q = malloc(qlen + 1);
    if (q == NULL || fread(q, 1, qlen, fp) != qlen) {
      free(q);
      break;
    }
    q[qlen] = '\0';
    s_pending[s_pending_count++] = (pending_t){.query = q,
                                               .src_ip = ip,
                                               .captured_wall = cw,
                                               .rainlog_done = rd != 0,
                                               .wu_done = wd != 0,
                                               .attempts = att};
  }
  fclose(fp);
  if (s_pending_count > 0) {
    ESP_LOGI(TAG, "restored %d buffered upload(s) from flash", s_pending_count);
  }
}

// Flush the buffer to flash if it changed, throttled. When the buffer empties,
// remove the file immediately so a reboot does not reload stale entries.
static void retry_persist(void) {
  if (!s_buffer_dirty) {
    return;
  }
  if (s_pending_count == 0) {
    remove(RETRY_FILE);
    s_buffer_dirty = false;
    s_last_persist_us = esp_timer_get_time();
    return;
  }
  int64_t now = esp_timer_get_time();
  if (s_last_persist_us != 0 &&
      now - s_last_persist_us < RETRY_PERSIST_MIN_US) {
    return;  // too soon; stays dirty, written on a later pass
  }
  retry_save();
  s_buffer_dirty = false;
  s_last_persist_us = now;
}

// Forward a freshly captured upload, buffering it for retry if either target
// did not deliver. Runs on the forwarder task only.
static void forward_upload(const char *raw_query, uint32_t src_ip) {
  bool rainlog_done;
  // Throttle the Rainlog forward of a dateutc=now reading to one per gauge per
  // window (Rainlog's rate limit); a skipped intermediate is full state and
  // timestamped on receipt, so nothing is lost. Real-timestamped readings are
  // always forwarded. Skips are not buffered (a fresher one will come).
  if (!parse_rainlog_gauge_id(raw_query)) {
    rainlog_done = true; // WU-only stations never send to Rainlog.
  } else if (dateutc_is_now(raw_query)) {
    uint32_t gauge = parse_rainlog_gauge_id(raw_query);
    int64_t now = esp_timer_get_time();
    if (rl_throttled(gauge, now)) {
      ESP_LOGI(TAG, "throttled Rainlog forward for gauge %lu (<305s)",
               (unsigned long)gauge);
      rainlog_done = true;  // intentionally skipped, not a failure
    } else {
      rainlog_done = try_rainlog(raw_query, src_ip);
      if (rainlog_done) {
        rl_mark_sent(gauge, now);  // 200 (accepted or RATELIMIT): window opens
      }
    }
  } else {
    rainlog_done = try_rainlog(raw_query, src_ip);
  }
  bool wu_done = try_wu(raw_query, src_ip);
  if (!rainlog_done || !wu_done) {
    buffer_add(raw_query, src_ip, rainlog_done, wu_done);
  }
}

// Drop buffered readings older than the age cap. Runs regardless of uplink so
// stale data is shed even during a long outage (needs a valid wall clock; a
// pre-clock capture has captured_wall 0 and is exempt until then).
static void expire_stale(void) {
  if (!wall_clock_valid()) {
    return;
  }
  int64_t now_wall = (int64_t)time(NULL);
  for (int i = s_pending_count - 1; i >= 0; i--) {
    if (s_pending[i].captured_wall != 0 &&
        now_wall - s_pending[i].captured_wall > RETRY_MAX_AGE_S) {
      ESP_LOGW(TAG, "dropping stale buffered reading (> %dh old)",
               RETRY_MAX_AGE_S / 3600);
      buffer_remove(i);
    }
  }
}

// Retry the pending targets of every buffered upload, NEWEST first so the
// freshest reading takes Rainlog's one-accept-per-window slot (older ones then
// clear as RATELIMIT, a 200). Skipped while the uplink is down (no point
// spending TLS attempts). On the first failed send the pass bails - a target is
// down, so the rest would just rack up 15s timeouts and starve new uploads;
// they wait for the next interval. An entry is dropped once both targets
// deliver or it exhausts its live attempts.
static bool source_conflict(const char *query);

static void retry_pass(void) {
  expire_stale();
  if (s_pending_count == 0 || !wifi_link_sta_has_ip()) {
    return;
  }
  for (int i = s_pending_count - 1; i >= 0; i--) {
    pending_t *e = &s_pending[i];
    if (source_conflict(e->query)) {
      buffer_remove(i);
      continue;
    }
    bool failed = false;
    if (!e->rainlog_done) {
      e->rainlog_done = try_rainlog(e->query, e->src_ip);
      failed = failed || !e->rainlog_done;
    }
    if (!e->wu_done) {
      e->wu_done = try_wu(e->query, e->src_ip);
      failed = failed || !e->wu_done;
    }
    e->attempts++;
    if (e->rainlog_done && e->wu_done) {
      ESP_LOGI(TAG, "retried upload delivered (%d pending)",
               s_pending_count - 1);
      buffer_remove(i);
    } else if (e->attempts >= RETRY_MAX_ATTEMPTS) {
      ESP_LOGW(TAG, "dropping upload after %u attempts (rainlog=%d wu=%d)",
               e->attempts, e->rainlog_done, e->wu_done);
      buffer_remove(i);
    }
    if (failed) {
      break;  // a target is down; resume next interval
    }
  }
}

// Queued/retried uploads must still belong to the configured source.
static bool source_conflict(const char *query) {
  bool radio_source = strstr(query, "&rlsource=radio&") != NULL;
  uint32_t gauge = parse_rainlog_gauge_id(query);
  return gauge && config_gauge_uses_radio(gauge) != radio_source;
}

static bool enqueue_upload(const char *query, uint32_t src_ip);
static bool enqueue_radio_upload(const char *query) {
  return enqueue_upload(query, 0);
}

// ---- public ---------------------------------------------------------------

static void forwarder_task(void *arg) {
  (void)arg;
  while (true) {
    // Wake for a newly captured upload, or wake on the timeout to drain the
    // retry buffer.
    fwd_msg_t *msg = NULL;
    if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(1000)) ==
        pdTRUE) {
      if (!source_conflict(msg->query)) forward_upload(msg->query, msg->src_ip);
      free(msg);
    }
    radio_upload_poll(enqueue_radio_upload);
    // Retry the buffer at most once per interval, whether we woke for a new
    // upload or the receive timed out.
    int64_t now = esp_timer_get_time();
    if (now - s_last_retry_us >= (int64_t)RETRY_INTERVAL_MS * 1000) {
      s_last_retry_us = now;
      retry_pass();
    }
    // Mirror any buffer change to flash (throttled).
    retry_persist();
  }
}

void forwarder_init(void) {
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  snprintf(s_bridge_id, sizeof(s_bridge_id), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  s_queue = xQueueCreate(FORWARD_QUEUE_LEN, sizeof(fwd_msg_t *));
  retry_load();  // resume undelivered readings from before the last reboot
  xTaskCreate(forwarder_task, "forwarder", FORWARDER_TASK_STACK, NULL, 3, NULL);
}

static bool enqueue_upload(const char *raw_query, uint32_t src_ip) {
  if (s_queue == NULL || raw_query == NULL || raw_query[0] == '\0') {
    return false;
  }
  fwd_msg_t *msg = malloc(sizeof(*msg) + strlen(raw_query) + 1);
  if (msg == NULL) {
    return false;
  }
  msg->src_ip = src_ip;
  strcpy(msg->query, raw_query);
  if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
    free(msg);
    ESP_LOGW(TAG, "forward queue full, dropping upload");
    return false;
  }
  return true;
}

bool forwarder_submit(const char *raw_query, uint32_t src_ip) {
  if (!raw_query || config_gauge_uses_radio(parse_rainlog_gauge_id(raw_query)))
    return false;
  uint8_t mac[6];
  if (!ap_clients_mac_for_ip(src_ip, mac)) return enqueue_upload(raw_query, src_ip);
  // Replace any supplied identity with the MAC observed on our own SoftAP.
  char *copy = strdup(raw_query);
  char *tagged = malloc(strlen(raw_query) + 64);
  if (!copy || !tagged) { free(copy); free(tagged); return false; }
  size_t used = 0;
  char *save = NULL;
  for (char *part = strtok_r(copy, "&", &save); part; part = strtok_r(NULL, "&", &save)) {
    char *eq = strchr(part, '=');
    if (key_is(part, eq ? (size_t)(eq - part) : strlen(part), "wu_device")) continue;
    used += sprintf(tagged + used, "%s%s", used ? "&" : "", part);
  }
  snprintf(tagged + used, 64, "&wu_device=%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  bool queued = enqueue_upload(tagged, src_ip);
  free(copy); free(tagged);
  return queued;
}

const char *forwarder_last_result(void) { return s_last_result; }

int forwarder_pending_count(void) { return s_pending_count; }

int64_t forwarder_seconds_since_upload(void) {
  if (s_last_upload_us == 0) {
    return -1;
  }
  return (esp_timer_get_time() - s_last_upload_us) / 1000000;
}
