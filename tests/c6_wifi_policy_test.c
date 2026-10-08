#include <assert.h>
#include <string.h>
#include "config_store.h"
int main(void) {
  bridge_config_t cfg = {0};
  assert(config_bridge_wifi_required(&cfg));
  cfg.provisioned = true;
  assert(config_bridge_wifi_required(&cfg));
  for (size_t i = 0; i < config_field_count; i++)
    assert(strcmp(config_fields[i].name, "bridge_wifi_auto_off"));
}
