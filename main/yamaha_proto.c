/*
 * Yamaha SCCU2 phone/media/call protocol: pure C implementation.
 * See yamaha_proto.h. Every rule here mirrors the Python t7bridge protocol
 * core; the host tests check byte-for-byte equality with its reference frames.
 */
#include "yamaha_proto.h"

#include <string.h>

/* ===================================================================== */
/* Checksum                                                               */
/* ===================================================================== */

uint8_t yam_checksum(const uint8_t *data, size_t len)
{
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(-(int)sum);
}

bool yam_frame_valid(const uint8_t *frame, size_t len)
{
    if (len < 4) {
        return false;
    }
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += frame[i];
    }
    return (sum & 0xFF) == 0;
}

/* ===================================================================== */
/* Session-init (0xAA) and ACK                                            */
/* ===================================================================== */

const uint8_t YAM_ACK[YAM_LEN_ACK] = {0x5A, 0x01, 0x7F, 0x01, 0x01, 0x01, 0x00, 0x23};
static const uint8_t AUTH_PREFIX[5] = {0xAA, 0x01, 0x7F, 0x00, 0x35};

static bool printable_ascii_exact(const char *s, size_t want)
{
    if (s == NULL || strlen(s) != want) {
        return false;
    }
    for (size_t i = 0; i < want; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

bool yam_build_auth(uint8_t out[YAM_LEN_AUTH], yam_auth_mode_t mode,
                    const char *token, const char *ccu_id, const char *pairing_code)
{
    const bool initial = (mode == YAM_AUTH_INITIAL_PAIR);
    if (!printable_ascii_exact(token, YAM_TOKEN_LEN)) {
        return false;
    }
    if (initial && (!printable_ascii_exact(ccu_id, YAM_CCU_ID_LEN) ||
                    !printable_ascii_exact(pairing_code, YAM_PAIRING_CODE_LEN))) {
        return false;
    }
    size_t o = 0;
    memcpy(out + o, AUTH_PREFIX, sizeof AUTH_PREFIX);
    o += sizeof AUTH_PREFIX;
    if (initial) {
        memcpy(out + o, ccu_id, YAM_CCU_ID_LEN);
        memcpy(out + o + YAM_CCU_ID_LEN, pairing_code, YAM_PAIRING_CODE_LEN);
    } else {
        memset(out + o, '0', YAM_CCU_ID_LEN + YAM_PAIRING_CODE_LEN);
    }
    o += YAM_CCU_ID_LEN + YAM_PAIRING_CODE_LEN;
    memcpy(out + o, token, YAM_TOKEN_LEN);
    o += YAM_TOKEN_LEN;
    out[o++] = initial ? 0x01 : 0x00;
    out[o++] = 0x00;
    out[o] = yam_checksum(out, o);
    return true;
}

bool yam_parse_auth(const uint8_t *f, size_t len, yam_auth_info_t *out)
{
    if (len != YAM_LEN_AUTH || memcmp(f, AUTH_PREFIX, sizeof AUTH_PREFIX) != 0 ||
        !yam_frame_valid(f, len)) {
        return false;
    }
    size_t o = sizeof AUTH_PREFIX;
    memcpy(out->ccu_id, f + o, YAM_CCU_ID_LEN);
    out->ccu_id[YAM_CCU_ID_LEN] = 0;
    o += YAM_CCU_ID_LEN;
    memcpy(out->pairing_code, f + o, YAM_PAIRING_CODE_LEN);
    out->pairing_code[YAM_PAIRING_CODE_LEN] = 0;
    o += YAM_PAIRING_CODE_LEN;
    memcpy(out->token, f + o, YAM_TOKEN_LEN);
    out->token[YAM_TOKEN_LEN] = 0;
    o += YAM_TOKEN_LEN;
    out->mode = f[o] == 0x01 ? YAM_AUTH_INITIAL_PAIR : YAM_AUTH_RECONNECT;
    return true;
}

void yam_ack_reset(yam_ack_recognizer_t *r)
{
    r->len = 0;
}

yam_ack_result_t yam_ack_feed(yam_ack_recognizer_t *r, const uint8_t *chunk,
                              size_t len, size_t *consumed)
{
    *consumed = 0;
    if (r->len == 0 && (len == 0 || chunk[0] != YAM_ACK[0])) {
        return YAM_ACK_NONE;
    }
    size_t needed = YAM_LEN_ACK - r->len;
    size_t take = len < needed ? len : needed;
    memcpy(r->buf + r->len, chunk, take);
    r->len += take;
    *consumed = take;
    if (memcmp(r->buf, YAM_ACK, r->len) != 0) {
        r->len = 0;
        return YAM_ACK_INVALID;
    }
    if (r->len == YAM_LEN_ACK) {
        r->len = 0;
        return YAM_ACK_ACCEPTED;
    }
    return YAM_ACK_PARTIAL;
}

/* ===================================================================== */
/* State                                                                  */
/* ===================================================================== */

static void copy_text(char dst[YAM_TEXT_MAX], const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= YAM_TEXT_MAX) {
        n = YAM_TEXT_MAX - 1;
        /* Do not leave a cut UTF-8 sequence at the end. */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void yam_state_init(yam_state_t *s)
{
    memset(s, 0, sizeof *s);
}

void yam_state_set_media(yam_state_t *s, const char *artist, const char *album,
                         const char *title, bool playing)
{
    copy_text(s->artist, artist);
    copy_text(s->album, album);
    copy_text(s->title, title);
    s->playing = playing;
}

void yam_state_set_indicators(yam_state_t *s, uint8_t battery_percent,
                              uint8_t mobile_signal, uint8_t gnss_signal)
{
    s->battery_percent = battery_percent > 100 ? 100 : battery_percent;
    s->mobile_signal = mobile_signal & 0x0F;
    s->gnss_signal = gnss_signal & 0x0F;
}

void yam_state_set_call_ringing(yam_state_t *s, const char *number, const char *name)
{
    s->call_state = YAM_CALL_RINGING;
    copy_text(s->caller_number, number);
    copy_text(s->caller_name, name);
    s->slots[0] = 1;
    s->slots[1] = 0;
    s->slots[7] = 0;
}

void yam_state_set_call_active(yam_state_t *s)
{
    s->call_state = YAM_CALL_ACTIVE;
    s->slots[0] = 2;
    s->slots[1] = 0;
    s->slots[7] = 1;
}

yam_call_end_t yam_state_end_call(yam_state_t *s)
{
    yam_call_state_t previous = s->call_state;
    if (previous == YAM_CALL_IDLE) {
        return YAM_CALL_END_NONE;
    }
    s->call_state = YAM_CALL_IDLE;
    s->caller_name[0] = '\0';
    s->caller_number[0] = '\0';
    if (previous == YAM_CALL_RINGING) {
        s->slots[0] = 2;
        s->slots[1] = 1;
        s->slots[7] = 0;
        return YAM_CALL_END_MISSED;
    }
    s->slots[0] = 0;
    s->slots[1] = 0;
    s->slots[7] = 2;
    return YAM_CALL_END_ENDED;
}

void yam_state_post_notification(yam_state_t *s, const char *title, const char *text,
                                 const char *sender, int slot3, int slot4, int slot5)
{
    uint8_t seq = (uint8_t)(s->notif_sequence + 1);
    s->notif_sequence = seq > 15 ? 1 : seq;
    copy_text(s->notif_title, title);
    copy_text(s->notif_text, text);
    copy_text(s->notif_sender, sender);
    if (slot3 >= 0) s->slots[2] = (uint8_t)slot3;
    if (slot4 >= 0) s->slots[3] = (uint8_t)slot4;
    if (slot5 >= 0) s->slots[4] = (uint8_t)slot5;
}

/* ===================================================================== */
/* Text rule: 30-byte UTF-16BE, max 15 units, >15 units cut to 14         */
/* ===================================================================== */

/* Decode one UTF-8 code point; invalid input yields U+FFFD for one byte. */
static uint32_t utf8_next(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    uint32_t cp;
    int extra;
    if (p[0] < 0x80) {
        *pp = p + 1;
        return p[0];
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F; extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F; extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07; extra = 3;
    } else {
        *pp = p + 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {   /* also stops at the NUL terminator */
            *pp = p + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp = p + 1 + extra;
    static const uint32_t min_for_len[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < min_for_len[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0xFFFD;   /* overlong, out of range, or encoded surrogate */
    }
    return cp;
}

void yam_fixed_utf16be(uint8_t out[30], const char *utf8)
{
    uint16_t units[17];
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : "");
    while (*p && n < 16) {             /* 16 units is enough to know ">15" */
        uint32_t cp = utf8_next(&p);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            units[n++] = (uint16_t)(0xD800 + (cp >> 10));
            units[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            units[n++] = (uint16_t)cp;
        }
    }
    if (n > 15) {
        n = 14;
        if (units[13] >= 0xD800 && units[13] <= 0xDBFF) {
            units[13] = 0xFFFD;        /* split surrogate pair, like Java */
        }
    }
    memset(out, 0, 30);
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = (uint8_t)(units[i] >> 8);
        out[2 * i + 1] = (uint8_t)(units[i] & 0xFF);
    }
}

/* ===================================================================== */
/* Outbound frames                                                        */
/* ===================================================================== */

typedef struct {
    uint8_t *buf;
    size_t off;
} writer_t;

static uint8_t *rec(writer_t *w, uint16_t local_id, uint8_t len)
{
    w->buf[w->off++] = (uint8_t)(local_id >> 8);
    w->buf[w->off++] = (uint8_t)(local_id & 0xFF);
    w->buf[w->off++] = len;
    uint8_t *payload = w->buf + w->off;
    w->off += len;
    return payload;
}

static void finish(writer_t *w, uint8_t counter)
{
    w->buf[w->off] = counter;
    w->off++;
    w->buf[w->off] = yam_checksum(w->buf, w->off);
    w->off++;
}

void yam_build_60(uint8_t out[YAM_LEN_60], const yam_state_t *s, yam_counters_t *c)
{
    static const uint8_t t0592[8] = {0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00};
    static const uint8_t t0594[8] = {0xFC, 0xFF, 0x00, 0xFE, 0x55, 0x55, 0x55, 0x55};
    writer_t w = {out, 0};
    out[w.off++] = 0x60;
    out[w.off++] = 0x05;
    memcpy(rec(&w, 0x0592, 8), t0592, 8);
    memset(rec(&w, 0x0593, 8), 0x00, 8);
    uint8_t *music = rec(&w, 0x0594, 8);
    memcpy(music, t0594, 8);
    music[0] = (uint8_t)((music[0] & 0xFC) | (s->playing ? 1 : 0));
    memset(rec(&w, 0x0595, 61), 0x00, 61);
    memset(rec(&w, 0x0598, 8), 0x55, 8);
    finish(&w, c->c60++);
}

void yam_build_61(uint8_t out[YAM_LEN_61], const yam_state_t *s, yam_counters_t *c)
{
    writer_t w = {out, 0};
    out[w.off++] = 0x61;
    out[w.off++] = 0x05;
    yam_fixed_utf16be(rec(&w, 0x058C, 30), s->caller_number);
    yam_fixed_utf16be(rec(&w, 0x058D, 30), s->caller_name);
    const struct { uint16_t id; const char *text; } notes[3] = {
        {0x058E, s->notif_title}, {0x058F, s->notif_text}, {0x0591, s->notif_sender},
    };
    for (int i = 0; i < 3; i++) {
        uint8_t *p = rec(&w, notes[i].id, 31);
        p[0] = s->notif_sequence;
        yam_fixed_utf16be(p + 1, notes[i].text);
    }
    finish(&w, c->c61++);
}

void yam_build_62(uint8_t out[YAM_LEN_62], yam_state_t *s, yam_counters_t *c,
                  const yam_time_t *now)
{
    static const uint8_t t059D[8] = {0x00, 0xF8, 0x30, 0x55, 0xFF, 0xFF, 0xFF, 0xFF};
    writer_t w = {out, 0};
    out[w.off++] = 0x62;
    out[w.off++] = 0x07;

    uint8_t *t = rec(&w, 0x058A, 8);
    t[0] = (uint8_t)(now->year >> 8);
    t[1] = (uint8_t)(now->year & 0xFF);
    t[2] = now->month;
    t[3] = now->day;
    t[4] = now->hour;
    t[5] = now->minute;
    t[6] = now->second;
    t[7] = (uint8_t)(s->battery_percent << 1);   /* low bit: unresolved flag, 0 */

    uint8_t *st = rec(&w, 0x058B, 8);
    st[0] = (uint8_t)(((s->slots[0] & 3) << 6) | ((s->slots[1] & 3) << 4) |
                      ((s->slots[2] & 3) << 2) | (s->slots[3] & 3));
    st[1] = (uint8_t)(((s->slots[4] & 3) << 6) | ((s->slots[5] & 3) << 4) |
                      ((s->slots[6] & 3) << 2) | (s->slots[7] & 3));
    st[2] = s->headset_present ? 0x80 : 0x00;
    st[3] = st[4] = st[5] = st[6] = 0x55;
    st[7] = (uint8_t)((s->mobile_signal << 4) | s->gnss_signal);
    memset(s->slots, 0, sizeof s->slots);   /* pulses are consumed */

    yam_fixed_utf16be(rec(&w, 0x059A, 30), s->artist);
    yam_fixed_utf16be(rec(&w, 0x059B, 30), s->album);
    yam_fixed_utf16be(rec(&w, 0x059C, 30), s->title);
    memcpy(rec(&w, 0x059D, 8), t059D, 8);
    memset(rec(&w, 0x059E, 8), 0xFF, 8);
    finish(&w, c->c62++);
}

/* ===================================================================== */
/* Scheduler                                                              */
/* ===================================================================== */

static const int64_t INTERVAL_MS[3] = {1000, 1000, 2000};

void yam_sched_start(yam_scheduler_t *t, int64_t now_ms)
{
    t->active = true;
    for (int i = 0; i < 3; i++) {
        t->next_ms[i] = now_ms;
    }
    t->immediate_62 = false;
    t->skip_next_periodic_62 = false;
}

void yam_sched_stop(yam_scheduler_t *t)
{
    t->active = false;
    t->immediate_62 = false;
}

void yam_sched_request_immediate_62(yam_scheduler_t *t)
{
    if (t->active) {
        t->immediate_62 = true;
    }
}

uint8_t yam_sched_due(yam_scheduler_t *t, int64_t now_ms)
{
    if (!t->active) {
        return 0;
    }
    uint8_t due = 0;
    for (int i = 0; i < 3; i++) {
        int64_t due_at = t->next_ms[i];
        if (now_ms < due_at) {
            continue;
        }
        /* Skip elapsed ticks without bursting old frames. */
        t->next_ms[i] = due_at + ((now_ms - due_at) / INTERVAL_MS[i] + 1) * INTERVAL_MS[i];
        if (i == 2 && t->skip_next_periodic_62) {
            t->skip_next_periodic_62 = false;
            continue;
        }
        due |= (uint8_t)(1u << i);
    }
    if (t->immediate_62) {
        t->immediate_62 = false;
        if (!(due & YAM_DUE_62)) {
            due |= YAM_DUE_62;
            t->skip_next_periodic_62 = true;
        }
    }
    return due;
}

/* ===================================================================== */
/* Inbound controls                                                       */
/* ===================================================================== */

#define LOCAL_VOLUME 0x011D
#define LOCAL_MEDIA  0x0117
#define LOCAL_CALL   0x0119

bool yam_decode_controls(const uint8_t *f, size_t len, uint8_t *events)
{
    *events = 0;
    if (!yam_frame_valid(f, len) || f[0] != 0x6B || (f[1] != 0x04 && f[1] != 0x05)) {
        return false;
    }
    if (f[1] == 0x04 && len != 23) {
        return false;
    }
    static const uint16_t ev_layout[4][2] = {
        {0x0048, 4}, {LOCAL_VOLUME, 1}, {LOCAL_MEDIA, 1}, {LOCAL_CALL, 1},
    };
    size_t end = len - 2;
    size_t off = 2;
    int index = 0;
    while (off < end) {
        if (off + 3 > end) {
            return false;
        }
        uint16_t id = (uint16_t)((f[off] << 8) | f[off + 1]);
        uint8_t rlen = f[off + 2];
        if (off + 3 + rlen > end) {
            return false;
        }
        if (f[1] == 0x04) {
            if (index >= 4 || id != ev_layout[index][0] || rlen != ev_layout[index][1]) {
                return false;
            }
        }
        index++;
        if (id == LOCAL_MEDIA || id == LOCAL_VOLUME || id == LOCAL_CALL) {
            if (rlen != 1) {
                return false;
            }
            uint8_t v = f[off + 3];
            if (id == LOCAL_MEDIA) {
                if (v == 1) *events |= YAM_EV_MEDIA_NEXT;
                else if (v == 2) *events |= YAM_EV_MEDIA_PREVIOUS;
                else if (v == 3 || v == 4) *events |= YAM_EV_MEDIA_TOGGLE;
            } else if (id == LOCAL_VOLUME) {
                if (v == 5) *events |= YAM_EV_VOLUME_UP;
                else if (v == 6) *events |= YAM_EV_VOLUME_DOWN;
            } else {
                if (v == 1) *events |= YAM_EV_CALL_ANSWER;
                else if (v == 0) *events |= YAM_EV_CALL_REJECT_END;
            }
        }
        off += 3 + rlen;
    }
    if (f[1] == 0x04 && index != 4) {
        return false;
    }
    return true;
}

void yam_ctrl_reset(yam_ctrl_parser_t *p)
{
    p->len = 0;
}

bool yam_ctrl_append(yam_ctrl_parser_t *p, const uint8_t *chunk, size_t len)
{
    if (p->len + len > YAM_CTRL_BUF) {
        p->len = 0;
        return false;
    }
    memcpy(p->buf + p->len, chunk, len);
    p->len += len;
    return true;
}

/* SCCU2 has no length field: its ordered map ends with local 0x0119. */
static long sccu2_end(const yam_ctrl_parser_t *p)
{
    size_t pos = 2;
    for (;;) {
        if (p->len < pos + 3) {
            return -1;
        }
        uint16_t id = (uint16_t)((p->buf[pos] << 8) | p->buf[pos + 1]);
        pos += 3 + p->buf[pos + 2];
        if (pos + 2 > YAM_CTRL_BUF) {
            return -2;
        }
        if (p->len < pos) {
            return -1;
        }
        if (id == LOCAL_CALL) {
            return (long)(pos + 2);
        }
    }
}

yam_ctrl_result_t yam_ctrl_next(yam_ctrl_parser_t *p, uint8_t *events)
{
    *events = 0;
    if (p->len < 2) {
        return YAM_CTRL_NEED_MORE;
    }
    if (p->buf[0] != 0x6B || (p->buf[1] != 0x04 && p->buf[1] != 0x05)) {
        p->len = 0;
        return YAM_CTRL_ERROR;
    }
    long end = 23;
    if (p->buf[1] == 0x05) {
        end = sccu2_end(p);
        if (end == -2) {
            p->len = 0;
            return YAM_CTRL_ERROR;
        }
        if (end < 0) {
            return YAM_CTRL_NEED_MORE;
        }
    }
    if (p->len < (size_t)end) {
        return YAM_CTRL_NEED_MORE;
    }
    bool ok = yam_decode_controls(p->buf, (size_t)end, events);
    memmove(p->buf, p->buf + end, p->len - (size_t)end);
    p->len -= (size_t)end;
    if (!ok) {
        p->len = 0;
        return YAM_CTRL_ERROR;
    }
    return YAM_CTRL_FRAME;
}

const char *yam_event_name(uint8_t bit)
{
    switch (bit) {
    case YAM_EV_MEDIA_NEXT:      return "MEDIA_NEXT";
    case YAM_EV_MEDIA_PREVIOUS:  return "MEDIA_PREVIOUS";
    case YAM_EV_MEDIA_TOGGLE:    return "MEDIA_TOGGLE_PLAYBACK";
    case YAM_EV_VOLUME_UP:       return "VOLUME_UP";
    case YAM_EV_VOLUME_DOWN:     return "VOLUME_DOWN";
    case YAM_EV_CALL_ANSWER:     return "CALL_ANSWER";
    case YAM_EV_CALL_REJECT_END: return "CALL_REJECT_OR_END";
    default:                     return "UNKNOWN";
    }
}
