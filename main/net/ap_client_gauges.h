#pragma once
#include <stdbool.h>
#include <stdint.h>
// Whether a Wi-Fi station has uploaded for this gauge during the current boot.
bool ap_clients_has_gauge(uint32_t gauge);
