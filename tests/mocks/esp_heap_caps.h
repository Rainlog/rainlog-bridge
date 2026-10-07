#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_DEFAULT 0
static inline void *heap_caps_malloc(size_t bytes, int caps) {
  (void)caps;
  return malloc(bytes);
}
static inline size_t heap_caps_get_free_size(int caps) {
  (void)caps;
  return 0;
}
