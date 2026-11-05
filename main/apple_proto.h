/*
 * Apple accessory protocols, parsing only (no Bluetooth calls):
 *   ANCS - Apple Notification Center Service (incoming/active calls)
 *   AMS  - Apple Media Service (track info, play state, remote commands)
 *   CTS  - Current Time Service (iPhone clock)
 *
 * Pure C so it can be unit-tested on a PC (test/host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- ANCS ---------------- */

#define ANCS_EVENT_ADDED     0
#define ANCS_EVENT_MODIFIED  1
#define ANCS_EVENT_REMOVED   2

#define ANCS_FLAG_SILENT          0x01
#define ANCS_FLAG_IMPORTANT       0x02
#define ANCS_FLAG_PREEXISTING     0x04
#define ANCS_FLAG_POSITIVE_ACTION 0x08
#define ANCS_FLAG_NEGATIVE_ACTION 0x10

#define ANCS_CAT_INCOMING_CALL  1
#define ANCS_CAT_MISSED_CALL    2
/* Not in Apple's original spec, but confirmed by Apple DTS: an ongoing call. */
#define ANCS_CAT_ACTIVE_CALL    12

#define ANCS_ATTR_TITLE    1
#define ANCS_ACTION_POSITIVE 0   /* answer */
#define ANCS_ACTION_NEGATIVE 1   /* decline / end */

typedef struct {
    uint8_t event_id;
    uint8_t flags;
    uint8_t category;
    uint8_t count;
    uint32_t uid;
} ancs_notification_t;

/* Notification Source packet (8 bytes). */
bool ancs_parse_notification(const uint8_t *data, size_t len, ancs_notification_t *out);

/* Control Point commands. Return the command length. */
size_t ancs_build_get_title(uint8_t out[8], uint32_t uid, uint16_t max_len);
size_t ancs_build_action(uint8_t out[6], uint32_t uid, uint8_t action_id);

/* Data Source reassembly for a Get Notification Attributes(Title) reply. */
#define ANCS_DS_BUF 300
typedef struct {
    uint8_t buf[ANCS_DS_BUF];
    size_t len;
} ancs_ds_t;

void ancs_ds_reset(ancs_ds_t *d);
/* Returns true when a complete Title reply is assembled; fills uid and title
 * (UTF-8, NUL-terminated, truncated to title_size-1). */
bool ancs_ds_feed(ancs_ds_t *d, const uint8_t *data, size_t len,
                  uint32_t *uid, char *title, size_t title_size);

/* ---------------- Call tracking ----------------
 * Turns ANCS call notifications into the three events the Yamaha side needs.
 *   Incoming Call added           -> request caller name (title) -> RINGING
 *   Active Call added after ring  -> ACTIVE (outgoing calls are ignored)
 *   Active Call removed           -> ENDED (after a short grace: hold/resume)
 *   Incoming removed, no active   -> ENDED (after grace) = missed/declined
 */
typedef enum {
    CALL_OUT_NONE = 0,
    CALL_OUT_FETCH_TITLE,   /* send ancs_build_get_title for out_uid */
    CALL_OUT_RINGING,       /* caller title in out_title */
    CALL_OUT_ACTIVE,
    CALL_OUT_ENDED,
} call_out_t;

typedef enum { CALLT_IDLE = 0, CALLT_RINGING, CALLT_ACTIVE } call_phase_t;

#define CALL_GRACE_MS 1500

typedef struct {
    call_phase_t phase;
    bool has_incoming;
    uint32_t incoming_uid;
    uint8_t incoming_flags;
    bool has_active;
    uint32_t active_uid;
    uint8_t active_flags;
    bool ringing_reported;
    int64_t end_deadline_ms;     /* 0 = none */
    int64_t title_deadline_ms;   /* report RINGING even if no title by then */
} call_tracker_t;

void call_tracker_reset(call_tracker_t *t);
/* Feed one Notification Source event. *out_uid is set for FETCH_TITLE. */
call_out_t call_tracker_on_notification(call_tracker_t *t, const ancs_notification_t *n,
                                        int64_t now_ms, uint32_t *out_uid);
/* Feed the title reply (or a failed fetch with title NULL). */
call_out_t call_tracker_on_title(call_tracker_t *t, uint32_t uid, const char *title);
/* Call periodically; may report ENDED after the grace period. */
call_out_t call_tracker_tick(call_tracker_t *t, int64_t now_ms);

/* TFT buttons -> which ANCS action to perform. Returns false if none applies. */
bool call_tracker_answer_target(const call_tracker_t *t, uint32_t *uid);
bool call_tracker_reject_target(const call_tracker_t *t, uint32_t *uid);

/* Caller title is either a contact name or a phone number. */
bool looks_like_phone_number(const char *s);

/* ---------------- AMS ---------------- */

#define AMS_ENTITY_PLAYER 0
#define AMS_ENTITY_QUEUE  1
#define AMS_ENTITY_TRACK  2

#define AMS_PLAYER_ATTR_PLAYBACK_INFO 1
#define AMS_TRACK_ATTR_ARTIST 0
#define AMS_TRACK_ATTR_ALBUM  1
#define AMS_TRACK_ATTR_TITLE  2

#define AMS_CMD_TOGGLE_PLAY_PAUSE 2
#define AMS_CMD_NEXT_TRACK        3
#define AMS_CMD_PREVIOUS_TRACK    4
#define AMS_CMD_VOLUME_UP         5
#define AMS_CMD_VOLUME_DOWN       6

typedef struct {
    uint8_t entity;
    uint8_t attribute;
    uint8_t flags;
    char value[128];   /* UTF-8, NUL-terminated (may be truncated by iOS) */
} ams_update_t;

bool ams_parse_entity_update(const uint8_t *data, size_t len, ams_update_t *out);
/* PlaybackInfo is "state,rate,elapsed"; state 1 = playing. */
bool ams_playback_is_playing(const char *playback_info);

/* ---------------- CTS ---------------- */

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} cts_time_t;

bool cts_parse_current_time(const uint8_t *data, size_t len, cts_time_t *out);

#ifdef __cplusplus
}
#endif
