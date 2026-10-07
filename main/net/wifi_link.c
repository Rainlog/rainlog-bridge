#include "wifi_link.h"

#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "status_led.h"

static const char *TAG = "wifi_link";

// Back off between STA (re)connect attempts instead of hammering connect in a
// tight scan loop, which keeps the radio/CPU hot (especially while creds are
// wrong or the AP is out of range).
#define RECONNECT_BACKOFF_US (5 * 1000 * 1000)
#define TEST_TIMEOUT_US (12 * 1000 * 1000)
#define SCAN_CACHE_MAX 24

// SoftAP gateway/IP (also the config page + captive-portal address). Off the
// common home subnets to avoid colliding with the STA-side network.
#define WIFI_AP_IP "10.41.0.1"
// Captive-portal URL advertised via DHCP option 114 (RFC 8910).
#define WIFI_AP_PORTAL_URI "http://" WIFI_AP_IP "/"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile bool s_sta_has_ip;
static volatile int s_ap_sta_count;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_test_timer;
static volatile wifi_test_state_t s_test_state;

// Background scan cache (filled only while idle; read by the config page).
static wifi_ap_record_t s_scan_cache[SCAN_CACHE_MAX];
static int s_scan_count;
static SemaphoreHandle_t s_scan_lock;
// A scan is wanted (boot, or a client just left). Consumed by scan_task only
// while idle, so we never scan while anyone is connected.
static volatile bool s_rescan_when_idle = true;
static void scan_task(void *arg);

static void reconnect_timer_cb(void *arg) {
  (void)arg;
  esp_wifi_connect();
}

// WiFi config ssid/password fields are uint8_t[]; copy a C string in safely.
static void copy_str(uint8_t *dst, const char *src, size_t dstsize) {
  snprintf((char *)dst, dstsize, "%s", src);
}

static void apply_sta_config(const char *ssid, const char *pass) {
  wifi_config_t cfg = {0};
  copy_str(cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
  copy_str(cfg.sta.password, pass, sizeof(cfg.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

// Put the saved creds back in the radio once a test concludes. Without this a
// failed test would leave the test creds loaded, and every reconnect until the
// next reboot would retry the wrong network.
static void restore_saved_sta_config(void) {
  const bridge_config_t *cfg = config_get();
  apply_sta_config(cfg->sta_ssid, cfg->sta_pass);
}

static void test_timeout_cb(void *arg) {
  (void)arg;
  if (s_test_state == WIFI_TEST_RUNNING) {
    s_test_state = WIFI_TEST_FAIL;
    restore_saved_sta_config();
    esp_wifi_disconnect();  // stop trying the test creds; reconnect uses saved
  }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data) {
  (void)arg;
  (void)base;
  switch (id) {
    case WIFI_EVENT_STA_START:
      // Don't attempt the uplink until provisioned: avoids a futile scan storm
      // (heat) and keeps the LED on "provisioning" (blue) for the setup screen.
      if (config_is_provisioned()) {
        esp_wifi_connect();
      } else {
        ESP_LOGI(TAG, "unconfigured: SoftAP only, not connecting STA");
      }
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      s_sta_has_ip = false;
      if (s_test_state == WIFI_TEST_RUNNING) {
        break;  // a test in progress decides its own outcome (got-IP / timeout)
      }
      if (config_is_provisioned()) {
        status_led_set_error(true);  // losing a configured uplink is an error
        ESP_LOGW(TAG, "STA disconnected, retrying in 5s");
        esp_timer_start_once(s_reconnect_timer, RECONNECT_BACKOFF_US);
      }
      break;
    case WIFI_EVENT_AP_STACONNECTED:
      s_ap_sta_count++;
      s_rescan_when_idle = true;  // refresh the network list once they leave
      ESP_LOGI(TAG, "station joined SoftAP (%d)", s_ap_sta_count);
      break;
    case WIFI_EVENT_AP_STADISCONNECTED: {
      wifi_event_ap_stadisconnected_t *d =
          (wifi_event_ap_stadisconnected_t *)data;
      if (s_ap_sta_count > 0) {
        s_ap_sta_count--;
      }
      ESP_LOGW(TAG, "station left SoftAP (%d) reason=%u", s_ap_sta_count,
               d ? d->reason : 0);
      break;
    }
    default:
      break;
  }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id,
                      void *data) {
  (void)arg;
  (void)base;
  (void)id;
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
  ESP_LOGI(TAG, "STA uplink up, IP " IPSTR, IP2STR(&event->ip_info.ip));
  s_sta_has_ip = true;
  status_led_set_error(false);
  // With the uplink up, give SoftAP clients real internet: NAT their traffic
  // out the STA side (lwIP NAPT, CONFIG_LWIP_IPV4_NAPT). Weather consoles
  // need it for NTP / vendor clouds; WU uploads are still DNS-spoofed to the
  // bridge and captured. Harmless to re-run on every reconnect.
  esp_err_t napt = config_wifi_interception_enabled()
                       ? esp_netif_napt_enable(s_ap_netif)
                       : ESP_OK;
  if (napt != ESP_OK) {
    ESP_LOGW(TAG, "NAPT enable failed: %s", esp_err_to_name(napt));
  }
  if (s_test_state == WIFI_TEST_RUNNING) {
    s_test_state = WIFI_TEST_OK;  // a got-IP means the test creds worked
    // Stay connected (the link is proven), but reload the saved creds so any
    // later reconnect doesn't reuse the unsaved test ones.
    restore_saved_sta_config();
  }
}

// Put the SoftAP on an uncommon subnet (not the ESP default 192.168.4.0/24, and
// not the usual home 192.168.0/1.x) so it is very unlikely to overlap the home
// LAN the STA side joins. An overlap would make 10.41.0.x ambiguous between the
// two interfaces and could misroute the bridge's own uplink to Rainlog/WU. The
// DHCP server derives the gateway + DNS it hands clients from this IP, so the
// captive-DNS/redirect to the bridge keep working at the new address.
static void set_ap_subnet(void) {
  esp_netif_ip_info_t ip = {0};
  esp_netif_str_to_ip4(WIFI_AP_IP, &ip.ip);
  esp_netif_str_to_ip4(WIFI_AP_IP, &ip.gw);
  esp_netif_str_to_ip4("255.255.255.0", &ip.netmask);
  // Stop the auto-started DHCP server, move the netif, restart it on the new
  // subnet. The stop may report "already stopped" before first start; ignore.
  esp_netif_dhcps_stop(s_ap_netif);
  ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));

  // Always advertise the setup page as the captive-portal URL via DHCP option
  // 114 (RFC 8910), so a phone is steered to it on connect without depending on
  // the DNS-probe heuristic - even once the bridge is provisioned. The weather
  // station ignores the option, so its DNS is unaffected. String is static (the
  // option stores the pointer).
  static const char portal_uri[] = WIFI_AP_PORTAL_URI;
  esp_err_t err = esp_netif_dhcps_option(
      s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
      (void *)portal_uri, strlen(portal_uri));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "captive-portal DHCP option not set: %s",
             esp_err_to_name(err));
  }

  ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
}

void wifi_link_start(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_sta_netif = esp_netif_create_default_wifi_sta();
  s_ap_netif = esp_netif_create_default_wifi_ap();
  set_ap_subnet();

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

  const esp_timer_create_args_t reconnect_args = {
      .callback = reconnect_timer_cb,
      .name = "wifi_reconnect",
  };
  ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));
  const esp_timer_create_args_t test_args = {
      .callback = test_timeout_cb,
      .name = "wifi_test_timeout",
  };
  ESP_ERROR_CHECK(esp_timer_create(&test_args, &s_test_timer));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

  const bridge_config_t *cfg = config_get();

  wifi_config_t ap_cfg = {0};
  copy_str(ap_cfg.ap.ssid, cfg->ap_ssid, sizeof(ap_cfg.ap.ssid));
  ap_cfg.ap.ssid_len = strlen(cfg->ap_ssid);
  copy_str(ap_cfg.ap.password, cfg->ap_pass, sizeof(ap_cfg.ap.password));
  ap_cfg.ap.max_connection = 4;
  ap_cfg.ap.authmode =
      (strlen(cfg->ap_pass) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  // Valid SoftAP channel (1). When the STA later associates, AP+STA coexistence
  // moves the AP to the STA's channel automatically; channel 0 is not a valid
  // SoftAP channel and can leave the AP unable to accept associations.
  ap_cfg.ap.channel = 1;

  ESP_ERROR_CHECK(esp_wifi_set_mode(
      config_wifi_interception_enabled() ? WIFI_MODE_APSTA : WIFI_MODE_STA));
  apply_sta_config(cfg->sta_ssid, cfg->sta_pass);
  if (config_wifi_interception_enabled())
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  // STA modem sleep: let the radio nap between beacons when the uplink is idle.
  // (SoftAP keeps the radio on overall, but this still trims STA-side power.)
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

  s_scan_lock = xSemaphoreCreateMutex();
  xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 2, NULL);
}

void wifi_link_test_start(const char *ssid, const char *pass) {
  // A pending reconnect would otherwise fire mid-test and connect with the
  // test creds outside the test's control.
  esp_timer_stop(s_reconnect_timer);
  s_test_state = WIFI_TEST_RUNNING;
  esp_wifi_disconnect();
  apply_sta_config(ssid, pass);
  esp_wifi_connect();
  esp_timer_stop(s_test_timer);
  esp_timer_start_once(s_test_timer, TEST_TIMEOUT_US);
}

wifi_test_state_t wifi_link_test_status(void) { return s_test_state; }

// Run one active scan and fetch up to `max` AP records into `out`. Returns the
// count, or 0 on failure. Blanks the single radio for ~1-2s, so callers must be
// idle. Shared by the cache refresh and the live picker scan.
static int do_scan(wifi_ap_record_t *out, int max) {
  wifi_scan_config_t scan_cfg = {
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time = {.active = {.min = 30, .max = 90}},
  };
  if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
    return 0;
  }
  uint16_t num = (uint16_t)max;
  if (esp_wifi_scan_get_ap_records(&num, out) != ESP_OK) {
    return 0;
  }
  return num;
}

// Live scan into the cache. Disruptive (blanks the AP ~1s), so only called by
// the scan task while idle (no AP clients, no uplink). The records buffer goes
// on the heap: ~2.2KB of wifi_ap_record_t would eat most of this task's stack.
static void scan_into_cache(void) {
  ESP_LOGI(TAG, "scan starting (AP will blank briefly)");
  wifi_ap_record_t *tmp = malloc(SCAN_CACHE_MAX * sizeof(wifi_ap_record_t));
  if (tmp == NULL) {
    return;
  }
  int num = do_scan(tmp, SCAN_CACHE_MAX);
  if (num > 0) {
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    memcpy(s_scan_cache, tmp, (size_t)num * sizeof(wifi_ap_record_t));
    s_scan_count = num;
    xSemaphoreGive(s_scan_lock);
    ESP_LOGI(TAG, "scan done (%d APs)", num);
  }
  free(tmp);
}

static bool wifi_idle(void) {
  return s_ap_sta_count == 0 && !s_sta_has_ip &&
         s_test_state != WIFI_TEST_RUNNING;
}

static void scan_task(void *arg) {
  (void)arg;
  // Refresh the picker's network list when a scan is wanted (boot, or a client
  // just left) AND the bridge is idle. NEVER scan while anyone is connected: a
  // live scan blanks the single radio for ~1-2s and would drop them. The
  // pre-scan settle + recheck lets a fast (re)connect cancel the scan.
  while (true) {
    if (s_rescan_when_idle && wifi_idle()) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      if (wifi_idle()) {
        scan_into_cache();
        s_rescan_when_idle = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

int wifi_link_cached_aps(wifi_ap_record_t *out, int max) {
  xSemaphoreTake(s_scan_lock, portMAX_DELAY);
  int n = s_scan_count < max ? s_scan_count : max;
  memcpy(out, s_scan_cache, (size_t)n * sizeof(wifi_ap_record_t));
  xSemaphoreGive(s_scan_lock);
  return n;
}

int wifi_link_scan_live(wifi_ap_record_t *out, int max) {
  return do_scan(out, max);
}

void wifi_link_ap_ip_str(char *out, size_t len) {
  esp_netif_ip_info_t ip = {0};
  if (s_ap_netif != NULL && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK &&
      ip.ip.addr != 0) {
    snprintf(out, len, IPSTR, IP2STR(&ip.ip));
  } else {
    snprintf(out, len, "%s", WIFI_AP_IP);
  }
}

bool wifi_link_sta_ip_str(char *out, size_t len) {
  esp_netif_ip_info_t ip = {0};
  if (s_sta_has_ip && s_sta_netif != NULL &&
      esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
    snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    return true;
  }
  snprintf(out, len, "--");
  return false;
}

bool wifi_link_sta_ap_info(char *ssid, size_t ssid_len, int *rssi) {
  wifi_ap_record_t rec;
  if (esp_wifi_sta_get_ap_info(&rec) != ESP_OK) {
    return false;
  }
  if (ssid != NULL && ssid_len > 0) {
    snprintf(ssid, ssid_len, "%s", (const char *)rec.ssid);
  }
  if (rssi != NULL) {
    *rssi = rec.rssi;
  }
  return true;
}

bool wifi_link_sta_has_ip(void) { return s_sta_has_ip; }

int wifi_link_ap_station_count(void) { return s_ap_sta_count; }

esp_netif_t *wifi_link_sta_netif(void) { return s_sta_netif; }

esp_netif_t *wifi_link_ap_netif(void) { return s_ap_netif; }

bool wifi_link_ap_enabled(void) {
  wifi_mode_t mode;
  return esp_wifi_get_mode(&mode) == ESP_OK &&
         (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
}
