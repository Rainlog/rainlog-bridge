#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_8BIT 2
extern size_t framebuffer_allocation;
#define MALLOC_CAP_DEFAULT 0
static inline void *heap_caps_malloc(size_t bytes, int caps) {
  (void)caps;
  framebuffer_allocation = bytes;
  return malloc(bytes);
}
static inline size_t heap_caps_get_free_size(int caps) {
  (void)caps;
  return 0;
}
