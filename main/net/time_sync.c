#include "time_sync.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "upload_stats.h"

static const char *TAG = "time_sync";
static bool s_started;

static void on_sync(struct timeval *tv) {
  (void)tv;
  ESP_LOGI(TAG, "wall clock synced via SNTP");
  // A valid wall clock lets the stats re-anchor the persisted 24h window.
  upload_stats_clock_synced();
}

void time_sync_start(void) {
  if (s_started) {
    return;
  }
  s_started = true;
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  sntp_set_time_sync_notification_cb(on_sync);
  esp_sntp_init();
}
