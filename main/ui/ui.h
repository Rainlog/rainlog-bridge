// Rainlog Wireless Bridge - status UI.
//
// One fixed, non-scrolling status screen on the onboard LCD: headline 24h
// processed count, WiFi/station status, last forward result, firmware version
// + build date. Starts its own refresh task.
#pragma once

// Bring up the LCD and start the refresh task. No-op (logs) if the display
// fails to init, so the rest of the bridge keeps running headless.
void ui_start(void);
