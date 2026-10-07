#include "console_line.h"

#include <assert.h>
#include <string.h>

int main(void) {
  console_line_t line = {0};
  const char *command = "js 1+2";
  for (const char *p = command; *p; p++)
    assert(console_line_feed(&line, *p) == CONSOLE_LINE_NONE);
  assert(console_line_feed(&line, '\r') == CONSOLE_LINE_READY);
  assert(!strcmp(line.bytes, command));
  assert(console_line_feed(&line, '\n') == CONSOLE_LINE_NONE);
  for (size_t i = 0; i < sizeof(line.bytes) - 1; i++)
    console_line_feed(&line, 'x');
  assert(console_line_feed(&line, '\n') == CONSOLE_LINE_READY);
  assert(strlen(line.bytes) == sizeof(line.bytes) - 1);
  for (size_t i = 0; i < sizeof(line.bytes); i++) console_line_feed(&line, 'x');
  assert(console_line_feed(&line, '\n') == CONSOLE_LINE_INVALID);
  console_line_feed(&line, 'x');
  console_line_feed(&line, 0);
  console_line_feed(&line, 'y');
  assert(console_line_feed(&line, '\n') == CONSOLE_LINE_INVALID);
  console_line_feed(&line, 'z');
  assert(console_line_feed(&line, '\n') == CONSOLE_LINE_READY);
  assert(!strcmp(line.bytes, "z"));
}
