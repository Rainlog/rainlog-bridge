#include "upload_stats.h"

// Persist the rolling stats to LittleFS so the 24h counts + lifetime totals
// survive a reboot. ON by default now that they live on the wear-leveled
// `storage` partition (the old NVS home was off by default - too small to
// absorb a save every minute without wearing out in ~1 year). Build with
// -DSTATS_PERSIST=0 to disable (counts then reset to 0 on boot).
#ifndef STATS_PERSIST
#define STATS_PERSIST 1
#endif

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if STATS_PERSIST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "fs.h"
#endif

// One station reports at most every ~5 min, so 24h is < 300 events. 600 gives
// headroom for a couple of stations / faster reporters.
#define STATS_RING 600
#define DAY_US ((int64_t)24 * 60 * 60 * 1000000)

#if STATS_PERSIST
#define DAY_SECONDS (24 * 60 * 60)
#define STATS_FILE FS_BASE "/stats.bin"
#define STATS_TMP FS_BASE "/stats.tmp"
// Bump if the on-disk layout changes; an older/foreign file is then discarded.
#define STATS_MAGIC 0x31534C52u  // "RLS1"
#define STATS_VERSION 1
// Anything earlier than this means SNTP hasn't synced the wall clock yet.
#define WALL_CLOCK_MIN_UNIX 1600000000  // 2020-09-13
// Commit at most once every 5 minutes (cheap on a wear-leveled FS, but no
// point rewriting ~5KB more often). A write also only happens when a new
// success has actually been recorded since the last one (see s_persisted_total)
// - an unchanged total is never re-written just because ring entries aged.
#define SAVE_MIN_INTERVAL_US ((int64_t)5 * 60 * 1000 * 1000)

static const char *TAG = "upload_stats";

// On-disk format: a single fixed-size record for all targets, written
// atomically (temp file + rename). Each age is "seconds before saved_wall" for
// one success still inside the 24h window; ages are relative (not absolute
// monotonic times, which reset every boot) so the next boot can re-anchor them
// once SNTP provides a wall clock again.
typedef struct {
  uint32_t total;             // lifetime total at save time
  uint32_t count;             // number of valid ages
  uint32_t ages[STATS_RING];  // count valid, rest zero
} stats_target_blob_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t ring;       // STATS_RING the file was written with
  int64_t saved_wall;  // unix seconds when written
  stats_target_blob_t target[UPLOAD_TARGET_COUNT];
} stats_file_t;
#endif  // STATS_PERSIST

// Timestamps are esp_timer (monotonic since boot), not wall clock: entries
// recorded before the SNTP sync would otherwise sit at ~1970 and vanish from
// the 24h count the moment the clock jumps. Restored entries may re-anchor to
// negative values ("before this boot"); the cutoff comparison handles that.
typedef struct {
  int64_t when_us[STATS_RING];
  int head;        // next write slot
  int filled;      // entries written, capped at STATS_RING
  uint32_t total;  // lifetime (seeded from the file at boot when persisting)
} target_stats_t;

static target_stats_t s_targets[UPLOAD_TARGET_COUNT];
static SemaphoreHandle_t s_lock;  // one lock guards every target

#if STATS_PERSIST
static int64_t s_last_save_us;
// Combined lifetime total (RL+WU) as of the last write; a save is skipped when
// the current total still equals it (no new success to persist). Seeded from
// the loaded file so an idle device never rewrites unchanged data after boot.
static uint32_t s_persisted_total;
// The record loaded at boot, held until a valid wall clock lets us re-anchor
// its ring entries onto the monotonic timeline. Totals are applied at init.
static stats_file_t *s_pending;
#endif

// Caller holds s_lock.
static void insert_entry(target_stats_t *t, int64_t when_us) {
  t->when_us[t->head] = when_us;
  t->head = (t->head + 1) % STATS_RING;
  if (t->filled < STATS_RING) {
    t->filled++;
  }
}

#if STATS_PERSIST
static bool wall_clock_valid(void) {
  return time(NULL) >= (time_t)WALL_CLOCK_MIN_UNIX;
}

// Persist every target's lifetime total + current 24h window to one file.
// Throttled; needs a valid wall clock (without one the ages have no anchor for
// the next boot). In practice a success implies internet, which implies SNTP
// has synced. Written to a temp file then renamed (atomic on LittleFS) so a
// power loss mid-write can't corrupt the live file.
static void save_to_file(void) {
  if (!wall_clock_valid()) {
    return;
  }
  int64_t now_us = esp_timer_get_time();
  if (s_last_save_us != 0 && now_us - s_last_save_us < SAVE_MIN_INTERVAL_US) {
    return;
  }

  stats_file_t *f = calloc(1, sizeof(*f));
  if (f == NULL) {
    return;
  }
  f->magic = STATS_MAGIC;
  f->version = STATS_VERSION;
  f->ring = STATS_RING;

  int64_t cutoff = now_us - DAY_US;
  uint32_t cur_total = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int tg = 0; tg < UPLOAD_TARGET_COUNT; tg++) {
    target_stats_t *t = &s_targets[tg];
    f->target[tg].total = t->total;
    cur_total += t->total;
    uint32_t count = 0;
    for (int i = 0; i < t->filled; i++) {
      if (t->when_us[i] >= cutoff) {
        f->target[tg].ages[count++] =
            (uint32_t)((now_us - t->when_us[i]) / 1000000);
      }
    }
    f->target[tg].count = count;
  }
  xSemaphoreGive(s_lock);

  // Nothing new accepted since the last write: don't rewrite (ring entries
  // aging out doesn't count - they re-anchor from saved_wall on the next boot).
  if (cur_total == s_persisted_total) {
    free(f);
    return;
  }
  f->saved_wall = (int64_t)time(NULL);

  bool ok = false;
  FILE *fp = fopen(STATS_TMP, "wb");
  if (fp != NULL) {
    ok = fwrite(f, sizeof(*f), 1, fp) == 1;
    ok = (fclose(fp) == 0) && ok;
  }
  free(f);
  if (ok && rename(STATS_TMP, STATS_FILE) == 0) {
    s_last_save_us = now_us;
    s_persisted_total = cur_total;
  } else {
    ESP_LOGW(TAG, "stats save failed");
    remove(STATS_TMP);
  }
}

// Read STATS_FILE into a freshly malloc'd record, validating the header. NULL
// if absent, unreadable, or not our current format (then we start fresh).
static stats_file_t *load_from_file(void) {
  FILE *fp = fopen(STATS_FILE, "rb");
  if (fp == NULL) {
    return NULL;  // first boot / no file
  }
  stats_file_t *f = malloc(sizeof(*f));
  bool ok = f != NULL && fread(f, sizeof(*f), 1, fp) == 1;
  fclose(fp);
  if (!ok) {
    free(f);
    return NULL;
  }
  if (f->magic != STATS_MAGIC || f->version != STATS_VERSION ||
      f->ring != STATS_RING) {
    ESP_LOGW(TAG, "discarding stats file (magic/version/ring mismatch)");
    free(f);
    return NULL;
  }
  for (int tg = 0; tg < UPLOAD_TARGET_COUNT; tg++) {
    if (f->target[tg].count > STATS_RING) {
      ESP_LOGW(TAG, "discarding stats file (bad count)");
      free(f);
      return NULL;
    }
  }
  return f;
}
#endif  // STATS_PERSIST

void upload_stats_init(void) {
  s_lock = xSemaphoreCreateMutex();

#if STATS_PERSIST
  s_pending = load_from_file();
  if (s_pending != NULL) {
    // Totals need no clock; restore them now. Ring entries wait for SNTP.
    for (int tg = 0; tg < UPLOAD_TARGET_COUNT; tg++) {
      s_targets[tg].total = s_pending->target[tg].total;
      s_persisted_total +=
          s_pending->target[tg].total;  // matches what's on disk
    }
    ESP_LOGI(TAG,
             "restored totals rl=%lu wu=%lu; %u/%u ring entries await sync",
             (unsigned long)s_targets[UPLOAD_TARGET_RL].total,
             (unsigned long)s_targets[UPLOAD_TARGET_WU].total,
             (unsigned)s_pending->target[UPLOAD_TARGET_RL].count,
             (unsigned)s_pending->target[UPLOAD_TARGET_WU].count);
  }
#endif  // STATS_PERSIST
}

void upload_stats_clock_synced(void) {
#if STATS_PERSIST
  if (s_pending == NULL || !wall_clock_valid()) {
    return;
  }
  // How long the device was without a clock (powered off + boot-to-sync).
  int64_t offline_s = (int64_t)time(NULL) - s_pending->saved_wall;
  if (offline_s < 0) {
    offline_s = 0;
  }
  int64_t now_us = esp_timer_get_time();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int tg = 0; tg < UPLOAD_TARGET_COUNT; tg++) {
    const stats_target_blob_t *b = &s_pending->target[tg];
    int restored = 0;
    for (uint32_t i = 0; i < b->count; i++) {
      int64_t age_s = (int64_t)b->ages[i] + offline_s;
      if (age_s < DAY_SECONDS) {
        insert_entry(&s_targets[tg], now_us - age_s * 1000000);
        restored++;
      }
    }
    ESP_LOGI(TAG, "target %d 24h re-anchored: %d of %lu entries still current",
             tg, restored, (unsigned long)b->count);
  }
  xSemaphoreGive(s_lock);
  free(s_pending);
  s_pending = NULL;
#endif  // STATS_PERSIST
}

void upload_stats_record_success(upload_target_t target) {
  target_stats_t *t = &s_targets[target];
  int64_t now = esp_timer_get_time();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  insert_entry(t, now);
  t->total++;
  xSemaphoreGive(s_lock);
#if STATS_PERSIST
  save_to_file();
#endif
}

int upload_stats_count_last_24h(upload_target_t target) {
  target_stats_t *t = &s_targets[target];
  int64_t cutoff = esp_timer_get_time() - DAY_US;
  int count = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < t->filled; i++) {
    if (t->when_us[i] >= cutoff) {
      count++;
    }
  }
  xSemaphoreGive(s_lock);
  return count;
}

uint32_t upload_stats_total(upload_target_t target) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  uint32_t total = s_targets[target].total;
  xSemaphoreGive(s_lock);
  return total;
}

void upload_stats_clear(void) {
#if STATS_PERSIST
  // Factory reset: drop the persisted file (the caller reboots right after,
  // which zeroes the in-RAM counts).
  remove(STATS_FILE);
  remove(STATS_TMP);
  s_persisted_total = 0;  // nothing on disk now
  ESP_LOGW(TAG, "stats cleared (factory reset)");
#endif  // STATS_PERSIST
}
