/*
 * BLE central link to the Yamaha CCU (Nordic UART Service).
 *
 * Sequence (same as the proven Pi bridge / Android app):
 *   scan for "YCCU_..." -> connect (30 ms interval, 5 s supervision timeout)
 *   -> encrypt (pair the first time, stored key afterwards) -> MTU exchange
 *   -> discover NUS RX/TX -> enable TX notifications -> send 0xAA session-init
 *   -> wait for the exact 5A ACK -> session active.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCU_IDLE = 0,       /* waiting to scan                                   */
    CCU_SCANNING,
    CCU_CONNECTING,
    CCU_SECURING,       /* pairing / encrypting                              */
    CCU_DISCOVERING,    /* MTU, NUS service, characteristics, CCCD           */
    CCU_AUTHENTICATING, /* session-init sent, waiting for 5A ACK             */
    CCU_ACTIVE,         /* ACK received; periodic frames may be sent         */
} ccu_state_t;

typedef struct {
    void (*session_active)(void);                       /* ACK validated       */
    void (*session_ended)(void);                        /* link lost / closed  */
    void (*rx_chunk)(const uint8_t *data, size_t len);  /* NUS data after ACK  */
} ccu_link_callbacks_t;

void ccu_link_init(const ccu_link_callbacks_t *callbacks);

/* Call from the NimBLE host sync callback. */
void ccu_link_on_sync(void);

/* Call regularly (every ~20 ms) from the bridge task: starts scans, runs
 * session-init fallback and timeouts. */
void ccu_link_tick(int64_t now_ms);

/* Write one complete Yamaha frame (chunked to the ATT MTU). Only works in
 * CCU_ACTIVE. Returns false if the write could not be queued. */
bool ccu_link_send(const uint8_t *frame, size_t len);

ccu_state_t ccu_link_state(void);
const char *ccu_state_name(ccu_state_t state);

#ifdef __cplusplus
}
#endif
