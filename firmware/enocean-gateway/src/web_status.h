/*
 * Status JSON of the EnOcean gateway for the web page (src/web/index.html):
 * system, Thread, Matter, TCM, bridged actuators and the senders heard.
 */

#pragma once

#include <stddef.h>

namespace Gateway
{

/* A Gateway::Web::StatusWriter: Matter thread, returns 0 if the JSON does not fit. */
size_t WriteWebStatus(char *buf, size_t size);

} /* namespace Gateway */
