// Rainlog Wireless Bridge - normal status screen (shown once configured):
// 24h processed count, WiFi/station status, last forward result, config hint.
#pragma once

// Snapshot dynamic status once before rendering the frame in strips.
void screen_status_prepare(void);
void screen_status_draw(void);
