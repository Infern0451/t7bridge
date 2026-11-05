/*
 * Firmware updates over Wi-Fi, no laptop or home network needed.
 *
 * On request (5 quick volume-down presses), the ESP32 starts its own
 * password-protected Wi-Fi network "t7bridge-update" for 10 minutes. Join it
 * from a phone or computer, open http://192.168.4.1 and upload t7bridge.bin.
 * The file is checked, written to the spare firmware slot, and the board
 * restarts into it. If the new firmware fails to start properly, the
 * bootloader automatically returns to the previous version.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup (after NVS init). Marks a freshly updated firmware as
 * good once it has run healthily for a while (see ota_update_tick). */
void ota_update_init(void);

/* Start the update Wi-Fi network (no effect if already running). */
void ota_update_start(void);

bool ota_update_active(void);

/* While held, the Wi-Fi (setup + update page) is kept on. When released, it
 * switches off after the normal time limit. Used while the bike's pairing
 * code is missing. */
void ota_update_hold(bool hold);

/* Call regularly (bridge task): handles the 10-minute timeout and confirms a
 * new firmware as good after it has run for 60 seconds. */
void ota_update_tick(int64_t now_ms);

/* Version string of the running firmware (e.g. "1.1.0"). */
const char *ota_firmware_version(void);

#ifdef __cplusplus
}
#endif
