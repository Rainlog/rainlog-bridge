#pragma once
#ifdef RAINLOG_DEBUG_CONSOLE
void debug_console_start(void);
#else
static inline void debug_console_start(void) {}
#endif
