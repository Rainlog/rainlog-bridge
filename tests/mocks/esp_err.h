#pragma once
typedef int esp_err_t;
#define ESP_OK 0
static inline const char *esp_err_to_name(esp_err_t err) {
  (void)err;
  return "mock";
}
