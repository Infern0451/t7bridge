/*
 * Learn mode: get the bike's pairing code from the Yamaha Y-Connect app.
 *
 * The CCU only accepts a new phone (or bridge) that sends the right 6-digit
 * pairing code, and that code comes from Yamaha's servers via the Y-Connect
 * app. So, while the bridge has no code, it pretends to be the CCU:
 *
 *   1. It learns the CCU ID from the CCU's name (ignition on), or the rider
 *      types it in on the setup page (S/N on the CCU label).
 *   2. It advertises exactly like the CCU ("YCCU_<id>") with the same Nordic
 *      UART service. Keep the real CCU off (ignition off) or out of range.
 *   3. The rider pairs the bike in Y-Connect. The app connects to the bridge
 *      and sends its session-init frame, which contains the pairing code.
 *   4. The bridge saves the code, answers with the CCU's ACK, and goes back to
 *      normal: it then pairs with the real CCU by itself.
 *
 * Everything the app sends is logged, so a failed attempt still tells us why.
 *
 * Also here: holding the BOOT button for 5 seconds forgets the bike and all
 * Bluetooth pairings, then restarts (back into learn mode).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void learn_mode_register_gatt(void);   /* before the NimBLE host task starts */
void learn_mode_on_sync(void);
void learn_mode_tick(int64_t now_ms);  /* bridge task */
bool learn_mode_active(void);

/* Recent learn-mode events as text lines, for the setup page. */
size_t learn_mode_events(char *buf, size_t cap);

/* Option: ask the phone to pair over Bluetooth when the app connects. */
bool learn_mode_pair_request(void);
void learn_mode_set_pair_request(bool on);

/* From the CCU link while scanning without a pairing code: record the real
 * CCU's advertising data / scan response so learn mode can replay it. */
void learn_mode_capture(const uint8_t *data, uint8_t len, bool scan_rsp);
bool learn_mode_capture_complete(void);

/* Forget the bike and every Bluetooth pairing, then restart. */
void learn_mode_factory_reset(void);

#ifdef __cplusplus
}
#endif
