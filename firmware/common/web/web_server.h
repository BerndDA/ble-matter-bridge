/*
 * Read-only status page over HTTP, on OpenThread's TCP (the gateway has no Zephyr IP stack).
 *
 * The gateway listens on TCP port CONFIG_GATEWAY_WEB_PORT of its Thread addresses; the border
 * router routes the home network to them, so a browser opens http://[<global IPv6>]/.
 *
 *   GET /             the app's src/web/index.html: gzip-compressed into the firmware at build
 *                     time (common.cmake), sent as is with "Content-Encoding: gzip"
 *   GET /api/status   JSON from the app's writer, which runs on the Matter thread
 *
 * One request per connection (Connection: close), CONFIG_GATEWAY_WEB_CONNECTIONS at a time; a
 * connection that is not done after 15 s is dropped. Needs OpenThread built from source with TCP
 * (the prebuilt Nordic library has no TCP).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Gateway::Web
{

/* Writes the status JSON into @p buf; returns its length, or 0 if it doesn't fit. Called on the
 * Matter thread (stack lock held), without the OpenThread lock. */
using StatusWriter = size_t (*)(char *buf, size_t size);

/* Starts listening. Call once Thread is initialized. */
void Init(StatusWriter writer);

} /* namespace Gateway::Web */
