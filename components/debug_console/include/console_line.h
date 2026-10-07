// MeshCore-style bounded newline parser, shared by UART and USB transports.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char bytes[2052];  // Three-byte command prefix plus 2048 bytes and NUL.
  size_t used;
  bool dropping;
} console_line_t;

typedef enum {
  CONSOLE_LINE_NONE,
  CONSOLE_LINE_READY,
  CONSOLE_LINE_INVALID
} console_line_result_t;

static inline console_line_result_t console_line_feed(console_line_t *line,
                                                      uint8_t byte) {
  if (byte == '\r' || byte == '\n') {
    console_line_result_t result = line->dropping ? CONSOLE_LINE_INVALID
                                   : line->used   ? CONSOLE_LINE_READY
                                                  : CONSOLE_LINE_NONE;
    line->bytes[line->used] = 0;
    line->used = 0;
    line->dropping = false;
    return result;
  }
  if (!line->dropping) {
    if (byte && line->used + 1 < sizeof(line->bytes))
      line->bytes[line->used++] = byte;
    else
      line->dropping = true;
  }
  return CONSOLE_LINE_NONE;
}
