/*
 * Gateway log output over UDP, for a receiver anywhere on the home network.
 *
 * The gateway listens on UDP port CONFIG_GATEWAY_NET_LOG_PORT (Thread interface;
 * the border router routes it to the home network). Nothing is sent until a
 * receiver subscribes; scripts/netlog.py (make netlog) finds the gateway through
 * its Matter mDNS records and does that. Text protocol, one command per datagram:
 *
 *   "blegw-log hello"            -> "blegw-log gateway <serial> <version> <board>"
 *   "blegw-log subscribe"        -> "blegw-log ok"; log lines (info and above) go to
 *   "blegw-log subscribe debug"     the sender's address and port, debug lines too
 *   "blegw-log stop"             -> "blegw-log stopped"
 *
 * A subscription lasts CONFIG_GATEWAY_NET_LOG_LEASE_S; the receiver renews it.
 * There is one subscriber at a time: the last one to subscribe.
 *
 * The output is taken from the printk hook, so it is exactly what the console
 * shows (minimal log mode). Log lines are batched into datagrams of up to
 * CONFIG_GATEWAY_NET_LOG_DATAGRAM bytes; a full buffer drops data and says so.
 */

#pragma once

namespace Gateway::NetLog
{

/* Opens the UDP port and hooks the console output. Call once Thread is initialized. */
void Init();

} /* namespace Gateway::NetLog */
