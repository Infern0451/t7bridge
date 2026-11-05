/*
 * BLE link to the iPhone. The ESP32 advertises as "t7bridge" (soliciting
 * Apple's notification and media services), the iPhone connects and pairs,
 * and the ESP32 then acts as a client of the iPhone's:
 *   ANCS (calls), AMS (track info + remote commands), Battery, Current Time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup, before the bridge task runs. */
void iphone_link_init(void);

/* Call from the NimBLE host sync callback (starts advertising). */
void iphone_link_on_sync(void);

/* Call regularly from the bridge task (call-tracking timers). */
void iphone_link_tick(int64_t now_ms);

/* Handlebar buttons (YAM_EV_* bitmask) -> AMS / ANCS commands. */
void iphone_link_handle_controls(uint8_t events);

bool iphone_link_connected(void);

/* Learn mode borrows the (single) advertiser: stop/resume "t7bridge". */
void iphone_link_suspend_advertising(bool suspend);

#ifdef __cplusplus
}
#endif
