#pragma once
#include <stdbool.h>
// Called only by the forwarder task. Network sends never block pulse capture.
void radio_upload_poll(bool (*submit)(const char *query));
