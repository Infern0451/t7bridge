/*
 * Yamaha SCCU2 phone/media/call protocol over Nordic UART Service (NUS).
 *
 * Pure C, no ESP-IDF dependencies, so it can be unit-tested on a PC.
 * Ported from the clean-room Python t7bridge (protocol core), which was
 * proven against a real Tenere 700 CCU by the Raspberry Pi bridge v1.0.0.
 * Byte layouts follow phone-media-call-nus-3.9.0.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Frame lengths -------------------------------------------------- */
#define YAM_LEN_60   112   /* 0x60/0x05 navigation/status (music playing bit) */
#define YAM_LEN_61   172   /* 0x61/0x05 caller identity + notification text   */
#define YAM_LEN_62   147   /* 0x62/0x07 media strings, call status, battery    */
#define YAM_LEN_AUTH  60   /* 0xAA session-init / authorization request        */
#define YAM_LEN_ACK    8   /* 5A 01 7F 01 01 01 00 23                          */

#define YAM_TOKEN_LEN        32
#define YAM_CCU_ID_LEN       14
#define YAM_PAIRING_CODE_LEN  6

/* Text fields are stored as UTF-8; the builders apply Yamaha's rule
 * (max 15 UTF-16 units, >15 is cut to 14). 128 bytes is plenty. */
#define YAM_TEXT_MAX 128

/* ---- Checksum ------------------------------------------------------- */
uint8_t yam_checksum(const uint8_t *data, size_t len);      /* (-sum) & 0xFF */
bool    yam_frame_valid(const uint8_t *frame, size_t len);  /* sums to 0 mod 256 */

/* ---- Session-init (0xAA) -------------------------------------------- */
typedef enum {
    YAM_AUTH_RECONNECT = 0,      /* zeros for CCU ID and code, mode byte 00 */
    YAM_AUTH_INITIAL_PAIR = 1,   /* real CCU ID + pairing code, mode byte 01 */
} yam_auth_mode_t;

/* Returns false if any credential is not exactly the required printable-ASCII
 * length. ccu_id / pairing_code are ignored in reconnect mode. */
bool yam_build_auth(uint8_t out[YAM_LEN_AUTH], yam_auth_mode_t mode,
                    const char *token, const char *ccu_id, const char *pairing_code);

/* Parsed session-init frame, as sent by the Yamaha app (learn mode). */
typedef struct {
    yam_auth_mode_t mode;
    char ccu_id[YAM_CCU_ID_LEN + 1];
    char pairing_code[YAM_PAIRING_CODE_LEN + 1];
    char token[YAM_TOKEN_LEN + 1];
} yam_auth_info_t;

/* True if frame is a complete, checksum-valid 0xAA session-init frame. */
bool yam_parse_auth(const uint8_t *frame, size_t len, yam_auth_info_t *out);

extern const uint8_t YAM_ACK[YAM_LEN_ACK];

/* Recognizes the 8-byte ACK, which must start at a notification boundary. */
typedef struct {
    uint8_t buf[YAM_LEN_ACK];
    size_t len;
} yam_ack_recognizer_t;

typedef enum {
    YAM_ACK_NONE = 0,     /* chunk was not (the start of) an ACK            */
    YAM_ACK_PARTIAL,      /* fragment of an ACK received, need more         */
    YAM_ACK_ACCEPTED,     /* exact, checksum-valid ACK received             */
    YAM_ACK_INVALID,      /* a 0x5A reply arrived but is not the exact ACK  */
} yam_ack_result_t;

void yam_ack_reset(yam_ack_recognizer_t *r);
/* *consumed is set to how many bytes of chunk belonged to the ACK. */
yam_ack_result_t yam_ack_feed(yam_ack_recognizer_t *r, const uint8_t *chunk,
                              size_t len, size_t *consumed);

/* ---- Bridge state (what the phone side feeds in) --------------------- */
typedef enum { YAM_CALL_IDLE = 0, YAM_CALL_RINGING, YAM_CALL_ACTIVE } yam_call_state_t;
typedef enum { YAM_CALL_END_NONE = 0, YAM_CALL_END_MISSED, YAM_CALL_END_ENDED } yam_call_end_t;

typedef struct {
    char artist[YAM_TEXT_MAX];
    char album[YAM_TEXT_MAX];
    char title[YAM_TEXT_MAX];
    bool playing;

    yam_call_state_t call_state;
    char caller_name[YAM_TEXT_MAX];
    char caller_number[YAM_TEXT_MAX];

    char notif_title[YAM_TEXT_MAX];
    char notif_text[YAM_TEXT_MAX];
    char notif_sender[YAM_TEXT_MAX];
    uint8_t notif_sequence;          /* 0, then 1..15 wrapping to 1 */

    bool headset_present;
    uint8_t battery_percent;         /* 0..100; the TFT needs a real value */
    uint8_t mobile_signal;           /* 0..15 */
    uint8_t gnss_signal;             /* 0..15 */

    uint8_t slots[8];                /* status pulses, consumed by each 0x62 */
} yam_state_t;

void yam_state_init(yam_state_t *s);
void yam_state_set_media(yam_state_t *s, const char *artist, const char *album,
                         const char *title, bool playing);
void yam_state_set_indicators(yam_state_t *s, uint8_t battery_percent,
                              uint8_t mobile_signal, uint8_t gnss_signal);
void yam_state_set_call_ringing(yam_state_t *s, const char *number, const char *name);
void yam_state_set_call_active(yam_state_t *s);
yam_call_end_t yam_state_end_call(yam_state_t *s);   /* NONE if no call */
/* flags: slot numbers 3..5 with values 0..2, -1 = leave unchanged */
void yam_state_post_notification(yam_state_t *s, const char *title, const char *text,
                                 const char *sender, int slot3, int slot4, int slot5);

/* ---- Outbound frame builders ---------------------------------------- */
typedef struct {
    uint8_t c60, c61, c62;   /* independent wrapping counters per variant */
} yam_counters_t;

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} yam_time_t;

void yam_build_60(uint8_t out[YAM_LEN_60], const yam_state_t *s, yam_counters_t *c);
void yam_build_61(uint8_t out[YAM_LEN_61], const yam_state_t *s, yam_counters_t *c);
/* Consumes (clears) the status-slot pulses in s, like the Yamaha app. */
void yam_build_62(uint8_t out[YAM_LEN_62], yam_state_t *s, yam_counters_t *c,
                  const yam_time_t *now);

/* Yamaha's 30-byte UTF-16BE text field rule (exposed for tests). */
void yam_fixed_utf16be(uint8_t out[30], const char *utf8);

/* ---- Scheduler: 0x60 + 0x61 every 1 s, 0x62 every 2 s ---------------- */
typedef struct {
    bool active;
    int64_t next_ms[3];          /* index 0: 0x60, 1: 0x61, 2: 0x62 */
    bool immediate_62;
    bool skip_next_periodic_62;
} yam_scheduler_t;

#define YAM_DUE_60 0x01
#define YAM_DUE_61 0x02
#define YAM_DUE_62 0x04

void yam_sched_start(yam_scheduler_t *t, int64_t now_ms);
void yam_sched_stop(yam_scheduler_t *t);
void yam_sched_request_immediate_62(yam_scheduler_t *t);   /* on call change */
/* Returns a bitmask of YAM_DUE_* frames to send now (in 60, 61, 62 order). */
uint8_t yam_sched_due(yam_scheduler_t *t, int64_t now_ms);

/* ---- Inbound handlebar controls (0x6B/0x05 SCCU2, 0x6B/0x04 SCCU2EV) -- */
#define YAM_EV_MEDIA_NEXT        0x01
#define YAM_EV_MEDIA_PREVIOUS    0x02
#define YAM_EV_MEDIA_TOGGLE      0x04
#define YAM_EV_VOLUME_UP         0x08
#define YAM_EV_VOLUME_DOWN       0x10
#define YAM_EV_CALL_ANSWER       0x20
#define YAM_EV_CALL_REJECT_END   0x40

#define YAM_CTRL_BUF 512

typedef struct {
    uint8_t buf[YAM_CTRL_BUF];
    size_t len;
} yam_ctrl_parser_t;

typedef enum {
    YAM_CTRL_NEED_MORE = 0,   /* buffered a fragment                         */
    YAM_CTRL_FRAME,           /* one complete frame decoded; check *events   */
    YAM_CTRL_ERROR,           /* bad family/checksum/layout; buffer reset    */
} yam_ctrl_result_t;

void yam_ctrl_reset(yam_ctrl_parser_t *p);
/* Append a NUS chunk. Then call yam_ctrl_next() until it returns NEED_MORE. */
bool yam_ctrl_append(yam_ctrl_parser_t *p, const uint8_t *chunk, size_t len);
yam_ctrl_result_t yam_ctrl_next(yam_ctrl_parser_t *p, uint8_t *events);

/* Decode one complete, checksum-valid 0x6B frame (exposed for tests). */
bool yam_decode_controls(const uint8_t *frame, size_t len, uint8_t *events);

const char *yam_event_name(uint8_t single_event_bit);

#ifdef __cplusplus
}
#endif
