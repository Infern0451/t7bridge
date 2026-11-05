/*
 * Apple accessory protocols (ANCS, AMS, CTS): parsing and call tracking.
 * See apple_proto.h.
 */
#include "apple_proto.h"

#include <string.h>

#define TITLE_WAIT_MS 1000

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Copy at most dst_size-1 bytes, never ending inside a UTF-8 sequence. */
static void copy_utf8(char *dst, size_t dst_size, const uint8_t *src, size_t n)
{
    if (dst_size == 0) {
        return;
    }
    if (n >= dst_size) {
        n = dst_size - 1;
        while (n > 0 && (src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ===================================================================== */
/* ANCS                                                                   */
/* ===================================================================== */

bool ancs_parse_notification(const uint8_t *d, size_t len, ancs_notification_t *out)
{
    if (len < 8) {
        return false;
    }
    out->event_id = d[0];
    out->flags = d[1];
    out->category = d[2];
    out->count = d[3];
    out->uid = rd32(d + 4);
    return out->event_id <= ANCS_EVENT_REMOVED;
}

size_t ancs_build_get_title(uint8_t out[8], uint32_t uid, uint16_t max_len)
{
    out[0] = 0x00;                       /* CommandIDGetNotificationAttributes */
    wr32(out + 1, uid);
    out[5] = ANCS_ATTR_TITLE;
    out[6] = (uint8_t)(max_len & 0xFF);  /* Title takes a 2-byte max length */
    out[7] = (uint8_t)(max_len >> 8);
    return 8;
}

size_t ancs_build_action(uint8_t out[6], uint32_t uid, uint8_t action_id)
{
    out[0] = 0x02;                       /* CommandIDPerformNotificationAction */
    wr32(out + 1, uid);
    out[5] = action_id;
    return 6;
}

void ancs_ds_reset(ancs_ds_t *d)
{
    d->len = 0;
}

bool ancs_ds_feed(ancs_ds_t *d, const uint8_t *data, size_t len,
                  uint32_t *uid, char *title, size_t title_size)
{
    if (d->len + len > ANCS_DS_BUF) {
        d->len = 0;
        return false;
    }
    memcpy(d->buf + d->len, data, len);
    d->len += len;
    if (d->buf[0] != 0x00) {             /* not a Get Notification Attributes reply */
        d->len = 0;
        return false;
    }
    if (d->len < 8) {
        return false;
    }
    if (d->buf[5] != ANCS_ATTR_TITLE) {
        d->len = 0;
        return false;
    }
    size_t alen = (size_t)d->buf[6] | ((size_t)d->buf[7] << 8);
    if (d->len < 8 + alen) {
        return false;                    /* continues in the next notification */
    }
    *uid = rd32(d->buf + 1);
    copy_utf8(title, title_size, d->buf + 8, alen);
    d->len = 0;
    return true;
}

/* ===================================================================== */
/* Call tracking                                                          */
/* ===================================================================== */

void call_tracker_reset(call_tracker_t *t)
{
    memset(t, 0, sizeof *t);
}

call_out_t call_tracker_on_notification(call_tracker_t *t, const ancs_notification_t *n,
                                        int64_t now_ms, uint32_t *out_uid)
{
    if (n->category == ANCS_CAT_INCOMING_CALL) {
        if (n->event_id == ANCS_EVENT_REMOVED) {
            if (t->has_incoming && n->uid == t->incoming_uid) {
                t->has_incoming = false;
                if (!t->has_active) {
                    t->end_deadline_ms = now_ms + CALL_GRACE_MS;
                }
            }
            return CALL_OUT_NONE;
        }
        bool same = t->has_incoming && n->uid == t->incoming_uid;
        t->has_incoming = true;
        t->incoming_uid = n->uid;
        t->incoming_flags = n->flags;
        t->end_deadline_ms = 0;
        if (same || t->phase == CALLT_ACTIVE) {
            /* Modified, or a second call waiting during an active one. */
            return CALL_OUT_NONE;
        }
        t->phase = CALLT_RINGING;
        t->ringing_reported = false;
        t->title_deadline_ms = now_ms + TITLE_WAIT_MS;
        *out_uid = n->uid;
        return CALL_OUT_FETCH_TITLE;
    }

    if (n->category == ANCS_CAT_ACTIVE_CALL) {
        if (n->event_id == ANCS_EVENT_REMOVED) {
            if (t->has_active && n->uid == t->active_uid) {
                t->has_active = false;
                if (!t->has_incoming) {
                    t->end_deadline_ms = now_ms + CALL_GRACE_MS;   /* hold/resume */
                }
            }
            return CALL_OUT_NONE;
        }
        if (t->phase == CALLT_IDLE) {
            /* No incoming call before it: an outgoing call. The TFT's phone
             * screen is built around incoming calls (and iOS offers no hang-up
             * action here), so outgoing calls are left alone, like the Pi did. */
            return CALL_OUT_NONE;
        }
        t->has_active = true;
        t->active_uid = n->uid;
        t->active_flags = n->flags;
        t->end_deadline_ms = 0;
        if (t->phase != CALLT_ACTIVE) {
            t->phase = CALLT_ACTIVE;
            t->title_deadline_ms = 0;
            return CALL_OUT_ACTIVE;
        }
        return CALL_OUT_NONE;
    }
    return CALL_OUT_NONE;
}

call_out_t call_tracker_on_title(call_tracker_t *t, uint32_t uid, const char *title)
{
    (void)title;
    if (t->phase == CALLT_RINGING && t->has_incoming && uid == t->incoming_uid &&
        !t->ringing_reported) {
        t->ringing_reported = true;
        t->title_deadline_ms = 0;
        return CALL_OUT_RINGING;
    }
    return CALL_OUT_NONE;
}

call_out_t call_tracker_tick(call_tracker_t *t, int64_t now_ms)
{
    if (t->title_deadline_ms && now_ms >= t->title_deadline_ms) {
        t->title_deadline_ms = 0;
        if (t->phase == CALLT_RINGING && !t->ringing_reported) {
            t->ringing_reported = true;    /* no name in time: ring anyway */
            return CALL_OUT_RINGING;
        }
    }
    if (t->end_deadline_ms && now_ms >= t->end_deadline_ms) {
        t->end_deadline_ms = 0;
        if (!t->has_active && !t->has_incoming && t->phase != CALLT_IDLE) {
            bool was_reported = t->ringing_reported || t->phase == CALLT_ACTIVE;
            t->phase = CALLT_IDLE;
            t->ringing_reported = false;
            if (was_reported) {
                return CALL_OUT_ENDED;
            }
        }
    }
    return CALL_OUT_NONE;
}

bool call_tracker_answer_target(const call_tracker_t *t, uint32_t *uid)
{
    if (t->has_incoming) {
        *uid = t->incoming_uid;
        return true;
    }
    return false;
}

bool call_tracker_reject_target(const call_tracker_t *t, uint32_t *uid)
{
    if (t->phase == CALLT_RINGING && t->has_incoming) {
        *uid = t->incoming_uid;
        return true;
    }
    if (t->has_active) {
        *uid = t->active_uid;
        return true;
    }
    return false;
}

bool looks_like_phone_number(const char *s)
{
    int digits = 0;
    if (s == NULL || *s == '\0') {
        return false;
    }
    for (; *s; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            digits++;
        } else if (c != '+' && c != ' ' && c != '-' && c != '(' && c != ')' && c != '.') {
            return false;
        }
    }
    return digits >= 3;
}

/* ===================================================================== */
/* AMS                                                                    */
/* ===================================================================== */

bool ams_parse_entity_update(const uint8_t *d, size_t len, ams_update_t *out)
{
    if (len < 3) {
        return false;
    }
    out->entity = d[0];
    out->attribute = d[1];
    out->flags = d[2];
    copy_utf8(out->value, sizeof out->value, d + 3, len - 3);
    return true;
}

bool ams_playback_is_playing(const char *info)
{
    /* PlaybackState: 0 paused, 1 playing, 2 rewinding, 3 fast-forwarding. */
    if (info == NULL || info[0] < '0' || info[0] > '9') {
        return false;
    }
    int state = 0;
    for (const char *p = info; *p >= '0' && *p <= '9'; p++) {
        state = state * 10 + (*p - '0');
    }
    return state != 0;
}

/* ===================================================================== */
/* CTS                                                                    */
/* ===================================================================== */

bool cts_parse_current_time(const uint8_t *d, size_t len, cts_time_t *out)
{
    if (len < 7) {
        return false;
    }
    out->year = (uint16_t)(d[0] | (d[1] << 8));
    out->month = d[2];
    out->day = d[3];
    out->hour = d[4];
    out->minute = d[5];
    out->second = d[6];
    return out->year >= 2000 && out->month >= 1 && out->month <= 12 &&
           out->day >= 1 && out->day <= 31 && out->hour < 24 && out->minute < 60 &&
           out->second < 60;
}
