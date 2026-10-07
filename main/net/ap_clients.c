#include "ap_clients.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "oui_table.h"
#include "wifi_link.h"

static const char *TAG = "ap_clients";

// User-given device names, persisted per MAC. Separate from "bridge_cfg" so a
// config rewrite never touches them; wiped by factory reset.
#define NS_NAMES "dev_names"

// NVS key for a device's name: 'n' + 12 hex digits (under the 15-char limit).
static void name_key(const uint8_t *mac, char *key, size_t len) {
  snprintf(key, len, "n%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

static ap_client_t s_clients[AP_CLIENTS_MAX];
static int s_count;
static SemaphoreHandle_t s_lock;

// Vendor for a MAC: "Private MAC" for locally administered addresses (phone
// MAC randomization sets that bit, so an OUI lookup would be meaningless),
// else a binary search of the curated OUI table, else "Unknown".
static const char *vendor_name(const uint8_t *mac) {
  if (mac[0] & 0x02) {
    return "Private MAC";
  }
  int lo = 0;
  int hi = (int)(sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0])) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int cmp = memcmp(mac, OUI_TABLE[mid].prefix, 3);
    if (cmp == 0) {
      return OUI_TABLE[mid].name;
    }
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return "Unknown";
}

// Find the entry for `mac`, adding one if absent. When the table is full, the
// longest-gone disconnected entry is recycled (with 8 slots vs the AP's
// 4-connection limit there is always one). Caller holds s_lock.
static ap_client_t *find_or_add(const uint8_t *mac) {
  for (int i = 0; i < s_count; i++) {
    if (memcmp(s_clients[i].mac, mac, 6) == 0) {
      return &s_clients[i];
    }
  }
  ap_client_t *e;
  if (s_count < AP_CLIENTS_MAX) {
    e = &s_clients[s_count++];
  } else {
    e = NULL;
    for (int i = 0; i < AP_CLIENTS_MAX; i++) {
      if (s_clients[i].connected) {
        continue;
      }
      if (e == NULL || s_clients[i].last_seen_us < e->last_seen_us) {
        e = &s_clients[i];
      }
    }
    if (e == NULL) {
      return NULL;  // unreachable while AP_CLIENTS_MAX > max_connection
    }
  }
  memset(e, 0, sizeof(*e));
  memcpy(e->mac, mac, 6);
  // Recall the device's saved name, if it was ever named.
  nvs_handle_t h;
  if (nvs_open(NS_NAMES, NVS_READONLY, &h) == ESP_OK) {
    char key[16];
    name_key(mac, key, sizeof(key));
    size_t len = sizeof(e->name);
    nvs_get_str(h, key, e->name, &len);  // leaves name empty on miss
    nvs_close(h);
  }
  return e;
}

static void mark(const uint8_t *mac, bool connected) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ap_client_t *e = find_or_add(mac);
  if (e != NULL) {
    e->connected = connected;
    e->last_seen_us = esp_timer_get_time();
  }
  xSemaphoreGive(s_lock);
}

static void on_sta_connected(void *arg, esp_event_base_t base, int32_t id,
                             void *data) {
  (void)arg;
  (void)base;
  (void)id;
  mark(((wifi_event_ap_staconnected_t *)data)->mac, true);
}

static void on_sta_disconnected(void *arg, esp_event_base_t base, int32_t id,
                                void *data) {
  (void)arg;
  (void)base;
  (void)id;
  mark(((wifi_event_ap_stadisconnected_t *)data)->mac, false);
}

// DHCP lease granted on our SoftAP: record the client's IP right away and the
// hostname it announced (option 12, already sanitized by esp-netif).
static void on_ip_assigned(void *arg, esp_event_base_t base, int32_t id,
                           void *data) {
  (void)arg;
  (void)base;
  (void)id;
  const ip_event_assigned_ip_to_client_t *ev =
      (const ip_event_assigned_ip_to_client_t *)data;
  if (ev->esp_netif != wifi_link_ap_netif()) {
    return;  // not our SoftAP's DHCP server
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ap_client_t *e = find_or_add(ev->mac);
  if (e != NULL) {
    e->connected = true;
    e->last_seen_us = esp_timer_get_time();
    e->ip = ev->ip;
    snprintf(e->hostname, sizeof(e->hostname), "%s", ev->hostname);
  }
  xSemaphoreGive(s_lock);
}

// Find the entry currently holding ip4, or NULL. Caller holds s_lock.
static ap_client_t *find_by_ip(uint32_t ip4) {
  for (int i = 0; i < s_count; i++) {
    if (s_clients[i].ip.addr == ip4) {
      return &s_clients[i];
    }
  }
  return NULL;
}

void ap_clients_note_upload(uint32_t ip4, uint32_t gauge_id) {
  if (s_lock == NULL || ip4 == 0) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ap_client_t *e = find_by_ip(ip4);
  if (e != NULL) {
    e->last_upload_us = esp_timer_get_time();
    e->rx_count++;
    if (gauge_id) e->gauge_id = gauge_id;
  }
  xSemaphoreGive(s_lock);
}

void ap_clients_note_forwarded(uint32_t ip4) {
  if (s_lock == NULL || ip4 == 0) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ap_client_t *e = find_by_ip(ip4);
  if (e != NULL) {
    e->fwd_count++;
  }
  xSemaphoreGive(s_lock);
}

void ap_clients_note_wu_forwarded(uint32_t ip4) {
  if (s_lock == NULL || ip4 == 0) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ap_client_t *e = find_by_ip(ip4);
  if (e != NULL) {
    e->wu_count++;
  }
  xSemaphoreGive(s_lock);
}

// Copy `src` into a fixed buffer, trimming leading/trailing whitespace (the
// reject bodies often carry a trailing newline).
static void copy_trimmed(char *dst, size_t dstsize, const char *src) {
  while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') {
    src++;
  }
  size_t n = 0;
  while (src[n] != '\0' && n + 1 < dstsize) {
    n++;
  }
  while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' ||
                   src[n - 1] == '\r' || src[n - 1] == '\n')) {
    n--;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

void ap_clients_note_error(uint32_t ip4, uint8_t flags, const char *detail) {
  if (s_lock == NULL || ip4 == 0) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ap_client_t *e = find_by_ip(ip4);
  if (e != NULL) {
    e->err_flags |= flags;
    if (detail != NULL) {
      if (flags & AP_CLIENT_ERR_RL_REJECT) {
        copy_trimmed(e->rl_reject, sizeof(e->rl_reject), detail);
      }
      if (flags & AP_CLIENT_ERR_WU_REJECT) {
        copy_trimmed(e->wu_reject, sizeof(e->wu_reject), detail);
      }
    }
  }
  xSemaphoreGive(s_lock);
}

void ap_clients_clear_error(uint32_t ip4, uint8_t flags) {
  if (s_lock == NULL || ip4 == 0) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  ap_client_t *e = find_by_ip(ip4);
  if (e != NULL) {
    e->err_flags &= ~flags;
    if (flags & AP_CLIENT_ERR_RL_REJECT) {
      e->rl_reject[0] = '\0';
    }
    if (flags & AP_CLIENT_ERR_WU_REJECT) {
      e->wu_reject[0] = '\0';
    }
  }
  xSemaphoreGive(s_lock);
}

int ap_clients_snapshot(ap_client_t *out, int max) {
  if (s_lock == NULL) {
    return 0;  // the UI task starts drawing before ap_clients_init has run
  }
  // Live associated stations + their DHCP leases, fetched outside the lock.
  wifi_sta_list_t stas = {0};
  wifi_sta_mac_ip_list_t ips = {0};
  bool live = esp_wifi_ap_get_sta_list(&stas) == ESP_OK;
  if (live && esp_wifi_ap_get_sta_list_with_ip(&stas, &ips) != ESP_OK) {
    ips.num = 0;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (live) {
    // Reconcile connected flags against the live list (self-heals any missed
    // join/leave event; the events still matter to log devices that come and
    // go between polls, with an accurate leave time).
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_count; i++) {
      bool assoc = false;
      for (int j = 0; j < stas.num; j++) {
        if (memcmp(s_clients[i].mac, stas.sta[j].mac, 6) == 0) {
          assoc = true;
          break;
        }
      }
      s_clients[i].connected = assoc;
    }
    for (int j = 0; j < stas.num; j++) {
      ap_client_t *e = find_or_add(stas.sta[j].mac);
      if (e == NULL) {
        continue;
      }
      e->connected = true;
      e->last_seen_us = now;
      e->rssi = stas.sta[j].rssi;
    }
    for (int j = 0; j < ips.num; j++) {
      for (int i = 0; i < s_count; i++) {
        if (memcmp(s_clients[i].mac, ips.sta[j].mac, 6) == 0) {
          if (ips.sta[j].ip.addr != 0) {
            s_clients[i].ip = ips.sta[j].ip;
          }
          break;
        }
      }
    }
  }
  int n = s_count < max ? s_count : max;
  for (int i = 0; i < n; i++) {
    out[i] = s_clients[i];
    out[i].vendor = vendor_name(out[i].mac);
  }
  xSemaphoreGive(s_lock);
  return n;
}

esp_err_t ap_clients_set_name(const uint8_t mac[6], const char *name) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NS_NAMES, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    return err;
  }
  char key[16];
  name_key(mac, key, sizeof(key));
  if (name[0] == '\0') {
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      err = ESP_OK;  // clearing a never-named device is fine
    }
  } else {
    err = nvs_set_str(h, key, name);
  }
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  if (err != ESP_OK) {
    return err;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < s_count; i++) {
    if (memcmp(s_clients[i].mac, mac, 6) == 0) {
      snprintf(s_clients[i].name, sizeof(s_clients[i].name), "%s", name);
      break;
    }
  }
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

void ap_clients_clear_names(void) {
  nvs_handle_t h;
  if (nvs_open(NS_NAMES, NVS_READWRITE, &h) == ESP_OK) {
    if (nvs_erase_all(h) == ESP_OK) {
      nvs_commit(h);
    }
    nvs_close(h);
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < s_count; i++) {
    s_clients[i].name[0] = '\0';
  }
  xSemaphoreGive(s_lock);
  ESP_LOGW(TAG, "device names cleared (factory reset)");
}

void ap_clients_init(void) {
  s_lock = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_sta_connected, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_sta_disconnected, NULL,
      NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, on_ip_assigned, NULL, NULL));
  ESP_LOGI(TAG, "tracking SoftAP clients (%d slots)", AP_CLIENTS_MAX);
}
