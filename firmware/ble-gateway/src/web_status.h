/*
 * Status JSON of the BLE gateway for the web page (src/web/index.html):
 * system, Thread, Matter, bridged BTHome sensors and eQ-3 thermostats, devices heard.
 */

#pragma once

#include <stddef.h>

namespace Gateway
{

/* A Gateway::Web::StatusWriter: Matter thread, returns 0 if the JSON does not fit. */
size_t WriteWebStatus(char *buf, size_t size);

} /* namespace Gateway */
