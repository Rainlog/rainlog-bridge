#include "net/wifi_idle.h"

#include <assert.h>
int main(void) {
  const int64_t start = 9000000, end = start + BRIDGE_WIFI_IDLE_US;
  assert(wifi_idle_keep_awake(true, true, true, false, end - 1, start));
  assert(!wifi_idle_keep_awake(true, true, true, false, end, start));
  assert(!wifi_idle_keep_awake(true, true, true, false, end + 1, start));
  assert(wifi_idle_keep_awake(true, true, true, true, end + 1, start));
  assert(wifi_idle_keep_awake(false, true, true, false, end, start));
  assert(wifi_idle_keep_awake(true, false, true, false, end, start));
  assert(wifi_idle_keep_awake(true, true, false, false, end, start));
  assert(wifi_idle_keep_awake(true, true, true, false, end, end));
}
