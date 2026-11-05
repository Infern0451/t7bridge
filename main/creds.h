/*
 * The three values the bridge needs to talk to a CCU, kept in flash (NVS):
 *
 *   token         32 characters. Made up by the bridge on first start; the CCU
 *                 remembers it when it pairs. Any value works.
 *   CCU ID        14 characters, the part after "YCCU_" in the CCU's name (also
 *                 printed as S/N on the CCU label). Found automatically.
 *   pairing code  6 characters. Checked by the CCU on first pairing. Comes
 *                 from Yamaha via the Y-Connect app: caught in learn mode
 *                 (learn_mode.h) or typed in on the setup page.
 *
 * Values set in menuconfig (T7 Bridge) are used as starting values when
 * nothing is stored yet; public builds leave them empty.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void creds_init(void);                 /* after nvs_flash_init() */

const char *creds_token(void);
const char *creds_ccu_id(void);        /* "" if not known yet */
const char *creds_pairing_code(void);  /* "" if not known yet */

bool creds_have_ccu_id(void);
bool creds_have_code(void);

/* Validate and store. Return false (and store nothing) if the format is wrong. */
bool creds_set_ccu_id(const char *id);
bool creds_set_pairing_code(const char *code);

/* Forget the CCU ID and pairing code (keeps the token). */
void creds_forget_bike(void);

bool creds_valid_ccu_id(const char *id);
bool creds_valid_code(const char *code);

#ifdef __cplusplus
}
#endif
