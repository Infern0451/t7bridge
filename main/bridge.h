/*
 * Bridge logic: owns the shared media/call state, runs the Yamaha frame
 * schedule while the CCU session is active, and decodes handlebar controls.
 *
 * The iPhone side (AMS/ANCS, phase 4) will feed state in through the
 * bridge_set_* functions and receive button presses via the control hook.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the CCU link and the bridge task. Call after NimBLE is initialised. */
void bridge_start(void);

/* NimBLE host sync callback forwards here. */
void bridge_on_ble_sync(void);

/* ---- Phone-side inputs (thread-safe) ---- */
void bridge_set_media(const char *artist, const char *album, const char *title, bool playing);
void bridge_set_battery(uint8_t percent);
/* Real wall-clock time from the iPhone (Current Time Service). */
void bridge_set_time_ymdhms(uint16_t year, uint8_t month, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second);
/* iPhone gone: clear its media, return to the placeholder battery level. */
void bridge_phone_disconnected(void);
void bridge_call_ringing(const char *number, const char *name);
void bridge_call_active(void);
void bridge_call_ended(void);

/* Temporarily show a status message in the TFT track fields (e.g. update
 * mode). Pass NULLs to return to normal media display. */
void bridge_set_status_text(const char *artist, const char *album, const char *title);

/* ---- Handlebar button hook ----
 * Called (from the bridge's receive path) with a YAM_EV_* bitmask whenever
 * the CCU reports a button press. Phase 4 maps these to AMS/ANCS commands. */
typedef void (*bridge_control_hook_t)(uint8_t events);
void bridge_set_control_hook(bridge_control_hook_t hook);

#ifdef __cplusplus
}
#endif
