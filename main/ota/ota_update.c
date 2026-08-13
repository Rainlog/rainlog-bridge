#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_util.h"
#include "psa/crypto.h"
#include "wifi_link.h"

static const char *TAG = "ota_update";

// First check fires this soon after the uplink comes up, then the interval
// doubles each time (exponential backoff) up to the steady-state cap below.
#define OTA_FIRST_DELAY_MS (2 * 1000)
// Steady-state check interval, randomized 12-24h, expressed in seconds; also
// the cap the backoff grows toward.
#define CHECK_MIN_S (12 * 60 * 60)
#define CHECK_MAX_S (24 * 60 * 60)

#define MANIFEST_BUF 512
#define HTTP_TIMEOUT_MS 15000

// Task-notify bits used to wake the task for an on-demand action; absent a
// notification the task wakes on its randomized periodic timeout.
#define CMD_CHECK (1u << 0)
#define CMD_APPLY (1u << 1)

typedef enum {
  PHASE_IDLE,
  PHASE_CHECKING,
  PHASE_UPTODATE,
  PHASE_AVAILABLE,
  PHASE_UPDATING,
  PHASE_ERROR,
} ota_phase_t;

// Shared status, guarded by s_lock. Single writer (the OTA task) and several
// readers (LCD, HTTP handlers), so the lock just keeps multi-field reads/writes
// (phase + strings + progress) consistent.
static struct {
  ota_phase_t phase;
  bool available;
  char latest[24];  // version string from the manifest
  char url[160];    // image URL from the manifest
  int progress;     // 0-100 during PHASE_UPDATING
  char error[64];   // last error message (PHASE_ERROR)
} s_ota;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

// ---- small helpers --------------------------------------------------------

static void set_phase(ota_phase_t phase) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_ota.phase = phase;
  xSemaphoreGive(s_lock);
}

static void set_error(const char *msg) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_ota.phase = PHASE_ERROR;
  snprintf(s_ota.error, sizeof(s_ota.error), "%s", msg);
  xSemaphoreGive(s_lock);
  ESP_LOGW(TAG, "%s", msg);
}

// Skip an optional leading 'v' so "v1.2.3" and "1.2.3" compare equal.
static const char *strip_v(const char *s) {
  return (*s == 'v' || *s == 'V') ? s + 1 : s;
}

// True if dotted-int version `cand` is strictly newer than `cur`. Parses up to
// three components (major.minor.patch); missing components count as 0.
static bool version_is_newer(const char *cand, const char *cur) {
  int a1 = 0, b1 = 0, c1 = 0, a2 = 0, b2 = 0, c2 = 0;
  sscanf(strip_v(cand), "%d.%d.%d", &a1, &b1, &c1);
  sscanf(strip_v(cur), "%d.%d.%d", &a2, &b2, &c2);
  if (a1 != a2) return a1 > a2;
  if (b1 != b2) return b1 > b2;
  return c1 > c2;
}

// Extract a JSON string value for `key` from a flat object into out. Minimal:
// handles `"key" : "value"` with backslash escapes, not nested objects.
static bool json_string(const char *body, const char *key, char *out,
                        size_t outlen) {
  char pat[40];
  int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
  if (n < 0 || (size_t)n >= sizeof(pat)) {
    return false;
  }
  const char *p = strstr(body, pat);
  if (p == NULL) {
    return false;
  }
  p = strchr(p + n, ':');
  if (p == NULL) {
    return false;
  }
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
    p++;
  }
  if (*p != '"') {
    return false;
  }
  p++;
  size_t o = 0;
  while (*p != '\0' && *p != '"' && o + 1 < outlen) {
    if (*p == '\\' && p[1] != '\0') {
      p++;  // pass the escaped char through literally (enough for
            // URLs/versions)
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
  return true;
}

// ---- manifest fetch -------------------------------------------------------

// Fetch + parse the manifest into latest/url/sha256. Returns true on a 200
// with every requested field present. `url` and `sha` may be NULL if the
// caller only wants the version; when requested, a missing field fails the
// fetch (make-ota.sh always writes all three, and the sha256 is what the
// update's integrity rests on - TLS here does not check cert expiry).
static bool fetch_manifest(char *latest, size_t latest_len, char *url,
                           size_t url_len, char *sha, size_t sha_len) {
  char *body = malloc(MANIFEST_BUF);
  if (body == NULL) {
    return false;
  }
  int status = 0;
  bool got = http_util_https_get(CFG_OTA_HOST, CFG_OTA_MANIFEST_PATH, NULL,
                                 body, MANIFEST_BUF, &status);

  // The manifest's "board" must match ours so a device never applies another
  // board's image (each board has its own manifest; see make-ota.sh). Absent
  // means a pre-board manifest, accepted for compatibility.
  char board[40];
  bool board_ok = !json_string(body, "board", board, sizeof(board)) ||
                  strcmp(board, BOARD_ID) == 0;

  bool ok = false;
  if (!got) {
    // transport failure already logged by http_util_https_get
  } else if (status != 200) {
    ESP_LOGW(TAG, "manifest HTTP %d", status);
  } else if (!board_ok) {
    ESP_LOGW(TAG, "manifest is for board \"%s\", not \"%s\"", board, BOARD_ID);
  } else if (!json_string(body, "version", latest, latest_len)) {
    ESP_LOGW(TAG, "manifest missing version");
  } else {
    ok = url == NULL || json_string(body, "url", url, url_len);
    if (!ok) {
      ESP_LOGW(TAG, "manifest missing url");
    } else if (sha != NULL && !json_string(body, "sha256", sha, sha_len)) {
      ESP_LOGW(TAG, "manifest missing sha256");
      ok = false;
    }
  }
  free(body);
  return ok;
}

// SHA-256 the first `len` bytes of `part` and compare (case-insensitively)
// against the 64-char lowercase hex digest `expect_hex`. Used to confirm the
// image just written to flash matches the one the manifest published, an
// end-to-end integrity check on top of esp_https_ota's own image validation.
static bool partition_sha256_matches(const esp_partition_t *part, size_t len,
                                     const char *expect_hex) {
  uint8_t *buf = malloc(4096);
  if (buf == NULL) {
    return false;
  }
  // PSA crypto (mbedtls 4.x is PSA-first; the legacy mbedtls/sha256.h is gone).
  // psa_crypto_init is idempotent, so calling it here is safe even though TLS
  // has likely already initialized it.
  psa_crypto_init();
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
  for (size_t off = 0; ok && off < len;) {
    size_t chunk = (len - off < 4096) ? (len - off) : 4096;
    if (esp_partition_read(part, off, buf, chunk) != ESP_OK ||
        psa_hash_update(&op, buf, chunk) != PSA_SUCCESS) {
      ok = false;
      break;
    }
    off += chunk;
  }
  free(buf);

  uint8_t digest[32];
  size_t digest_len = 0;
  if (ok) {
    ok = psa_hash_finish(&op, digest, sizeof(digest), &digest_len) ==
             PSA_SUCCESS &&
         digest_len == sizeof(digest);
  }
  if (!ok) {
    psa_hash_abort(&op);
    return false;
  }
  char hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  return strcasecmp(hex, expect_hex) == 0;
}

// ---- check + apply (run on the OTA task) ----------------------------------

static void do_check(void) {
  set_phase(PHASE_CHECKING);
  char latest[24], url[160];
  if (!fetch_manifest(latest, sizeof(latest), url, sizeof(url), NULL, 0)) {
    set_error("Update check failed");
    return;
  }
  const char *running = esp_app_get_description()->version;
  bool newer = version_is_newer(latest, running);

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_ota.available = newer;
  snprintf(s_ota.latest, sizeof(s_ota.latest), "%s", latest);
  snprintf(s_ota.url, sizeof(s_ota.url), "%s", url);
  s_ota.phase = newer ? PHASE_AVAILABLE : PHASE_UPTODATE;
  s_ota.error[0] = '\0';
  xSemaphoreGive(s_lock);

  ESP_LOGI(TAG, "manifest %s (running %s): %s", latest, running,
           newer ? "update available" : "up to date");
}

static void do_apply(void) {
  // Re-fetch the manifest so we apply the freshest image URL and re-confirm
  // it is actually newer than what is running.
  char latest[24], url[160], sha[65];
  if (!fetch_manifest(latest, sizeof(latest), url, sizeof(url), sha,
                      sizeof(sha))) {
    set_error("Update check failed");
    return;
  }
  const char *running = esp_app_get_description()->version;
  if (!version_is_newer(latest, running)) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ota.available = false;
    snprintf(s_ota.latest, sizeof(s_ota.latest), "%s", latest);
    s_ota.phase = PHASE_UPTODATE;
    xSemaphoreGive(s_lock);
    return;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  snprintf(s_ota.latest, sizeof(s_ota.latest), "%s", latest);
  snprintf(s_ota.url, sizeof(s_ota.url), "%s", url);
  s_ota.progress = 0;
  s_ota.phase = PHASE_UPDATING;
  s_ota.error[0] = '\0';
  xSemaphoreGive(s_lock);
  ESP_LOGI(TAG, "applying update %s from %s", latest, url);

  esp_http_client_config_t http_cfg = {
      .url = url,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = HTTP_TIMEOUT_MS,
      .keep_alive_enable = true,
      // Same generous buffers as the forwarder path (see http_util.h); the
      // image URL is short, but consistency here is cheap.
      .buffer_size = HTTP_UTIL_BUF_SIZE,
      .buffer_size_tx = HTTP_UTIL_BUF_SIZE,
  };
  esp_https_ota_config_t ota_cfg = {.http_config = &http_cfg};
  esp_https_ota_handle_t handle = NULL;
  if (esp_https_ota_begin(&ota_cfg, &handle) != ESP_OK) {
    set_error("Download failed to start");
    return;
  }

  esp_err_t err;
  while ((err = esp_https_ota_perform(handle)) ==
         ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    int total = esp_https_ota_get_image_size(handle);
    int read = esp_https_ota_get_image_len_read(handle);
    if (total > 0) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      s_ota.progress = read * 100 / total;
      xSemaphoreGive(s_lock);
    }
  }

  if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
    esp_https_ota_abort(handle);
    set_error("Download failed");
    return;
  }

  // Confirm the bytes written to flash match the manifest's published hash
  // before committing the update. The image is fully written by now but not yet
  // marked bootable, so aborting on a mismatch leaves the running image intact.
  const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
  int written = esp_https_ota_get_image_len_read(handle);
  if (update == NULL || written <= 0 ||
      !partition_sha256_matches(update, (size_t)written, sha)) {
    esp_https_ota_abort(handle);
    set_error("Image hash mismatch");
    return;
  }
  ESP_LOGI(TAG, "image sha256 verified against manifest");

  if (esp_https_ota_finish(handle) != ESP_OK) {
    set_error("Image verification failed");
    return;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_ota.progress = 100;
  xSemaphoreGive(s_lock);
  ESP_LOGW(TAG, "update written, rebooting into new image");
  vTaskDelay(pdMS_TO_TICKS(800));  // let the HTTP status poll flush to the page
  esp_restart();
}

// ---- task -----------------------------------------------------------------

// Block until the STA uplink is usable (or forever; this device's job is to
// uplink, so there is nothing else to do without it).
static void wait_for_uplink(void) {
  while (!wifi_link_sta_has_ip()) {
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

static void ota_task(void *arg) {
  (void)arg;

  wait_for_uplink();

  // Check 2s after the uplink first comes up, then back off exponentially up to
  // a randomized 12-24h steady-state interval. The fast first check surfaces an
  // available update promptly; the doubling avoids hammering the manifest while
  // still retrying soon if an early check fails.
  uint32_t delay_ms = OTA_FIRST_DELAY_MS;

  for (;;) {
    // Compute ticks in 64-bit: ms*tick_rate would overflow a 32-bit TickType_t
    // at the day-scale cap.
    TickType_t timeout =
        (TickType_t)(((uint64_t)delay_ms * configTICK_RATE_HZ) / 1000ULL);

    uint32_t cmd = 0;
    if (xTaskNotifyWait(0, UINT32_MAX, &cmd, timeout) == pdTRUE) {
      // On-demand from the web UI. Apply implies a fresh check inside do_apply.
      if (cmd & CMD_APPLY) {
        wait_for_uplink();
        do_apply();
      } else if (cmd & CMD_CHECK) {
        wait_for_uplink();
        do_check();
      }
    } else if (wifi_link_sta_has_ip()) {
      // Backoff timeout elapsed.
      do_check();
    }

    // Double the interval, capped at a freshly drawn 12-24h value (re-drawn
    // each time so the steady state keeps its jitter).
    uint64_t cap_ms =
        (uint64_t)(CHECK_MIN_S + (esp_random() % (CHECK_MAX_S - CHECK_MIN_S))) *
        1000ULL;
    uint64_t next_ms = (uint64_t)delay_ms * 2;
    delay_ms = (uint32_t)(next_ms < cap_ms ? next_ms : cap_ms);
  }
}

// ---- public ---------------------------------------------------------------

void ota_update_start(void) {
  if (s_task != NULL) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  snprintf(s_ota.latest, sizeof(s_ota.latest), "%s", "");
  // The OTA path runs the TLS handshake + esp_https_ota inline; give it a roomy
  // stack like the forwarder's.
  xTaskCreate(ota_task, "ota", 10240, NULL, 3, &s_task);
}

bool ota_update_available(void) {
  if (s_lock == NULL) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool a = s_ota.available;
  xSemaphoreGive(s_lock);
  return a;
}

void ota_update_latest(char *out, size_t len) {
  if (s_lock == NULL) {
    if (len > 0) {
      out[0] = '\0';
    }
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  snprintf(out, len, "%s", s_ota.latest);
  xSemaphoreGive(s_lock);
}

void ota_update_request_check(void) {
  if (s_task != NULL) {
    xTaskNotify(s_task, CMD_CHECK, eSetBits);
  }
}

void ota_update_request_apply(void) {
  if (s_task != NULL) {
    xTaskNotify(s_task, CMD_APPLY, eSetBits);
  }
}

void ota_update_status_json(char *out, size_t len) {
  static const char *const names[] = {"idle",      "checking", "uptodate",
                                      "available", "updating", "error"};
  const char *running = esp_app_get_description()->version;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  snprintf(out, len,
           "{\"phase\":\"%s\",\"running\":\"%.12s\",\"latest\":\"%s\","
           "\"available\":%s,\"progress\":%d,\"error\":\"%s\"}",
           names[s_ota.phase], running, s_ota.latest,
           s_ota.available ? "true" : "false", s_ota.progress, s_ota.error);
  xSemaphoreGive(s_lock);
}

void ota_update_mark_valid(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      ESP_LOGI(TAG, "OTA image marked valid (rollback cancelled)");
    }
  }
}
