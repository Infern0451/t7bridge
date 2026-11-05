/*
 * BLE link to the iPhone. See iphone_link.h.
 *
 * After the iPhone connects and the link is encrypted (paired), we discover
 * ANCS, AMS, Battery and Current Time on the iPhone, subscribe to their
 * notifications, then translate:
 *   AMS track/playback  -> bridge_set_media      (TFT track info, play state)
 *   Battery level       -> bridge_set_battery    (TFT battery icon)
 *   Current Time        -> bridge_set_time_ymdhms (0x62 clock)
 *   ANCS calls          -> bridge_call_*         (TFT caller ID / call screen)
 *   TFT buttons         -> AMS remote commands / ANCS answer, decline, end
 */
#include "iphone_link.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "sdkconfig.h"

#include "apple_proto.h"
#include "bridge.h"
#include "yamaha_proto.h"

static const char *TAG = "iphone";

/* ---------------------------------------------------------------------- */
/* UUIDs (little-endian, as NimBLE expects)                                 */
/* ---------------------------------------------------------------------- */
static const ble_uuid128_t ANCS_SVC = BLE_UUID128_INIT(
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
static const ble_uuid128_t ANCS_NS = BLE_UUID128_INIT(
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
static const ble_uuid128_t ANCS_CP = BLE_UUID128_INIT(
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
static const ble_uuid128_t ANCS_DS = BLE_UUID128_INIT(
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);
static const ble_uuid128_t AMS_SVC = BLE_UUID128_INIT(
    0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E, 0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89);
static const ble_uuid128_t AMS_RC = BLE_UUID128_INIT(
    0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8, 0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B);
static const ble_uuid128_t AMS_EU = BLE_UUID128_INIT(
    0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A, 0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F);
static const ble_uuid16_t BAS_SVC = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t BAS_LEVEL = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t CTS_SVC = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t CTS_TIME = BLE_UUID16_INIT(0x2A2B);

#define CCCD_UUID16 0x2902

/* ---------------------------------------------------------------------- */
/* Discovery tables                                                        */
/* ---------------------------------------------------------------------- */
enum { SVC_ANCS, SVC_AMS, SVC_BAS, SVC_CTS, SVC_COUNT };
static const ble_uuid_t *const SVC_UUIDS[SVC_COUNT] = {
    &ANCS_SVC.u, &AMS_SVC.u, &BAS_SVC.u, &CTS_SVC.u,
};
static const char *const SVC_NAMES[SVC_COUNT] = {"ANCS", "AMS", "Battery", "Current Time"};

typedef struct {
    uint16_t start, end;
} range_t;

enum { CH_NS, CH_CP, CH_DS, CH_RC, CH_EU, CH_BATT, CH_TIME, CH_COUNT };
typedef struct {
    const ble_uuid_t *uuid;
    int svc;
    bool needs_cccd;
    uint16_t val, end, cccd;
} chr_t;

static chr_t s_chr[CH_COUNT] = {
    [CH_NS]   = {&ANCS_NS.u, SVC_ANCS, true},
    [CH_CP]   = {&ANCS_CP.u, SVC_ANCS, false},
    [CH_DS]   = {&ANCS_DS.u, SVC_ANCS, true},
    [CH_RC]   = {&AMS_RC.u, SVC_AMS, true},
    [CH_EU]   = {&AMS_EU.u, SVC_AMS, true},
    [CH_BATT] = {&BAS_LEVEL.u, SVC_BAS, true},
    [CH_TIME] = {&CTS_TIME.u, SVC_CTS, true},
};

static range_t s_svc[SVC_COUNT];
static int s_disc_svc;      /* service whose characteristics are being found */
static int s_pending_end;   /* characteristic waiting for its end handle     */
static int s_disc_chr;      /* characteristic whose descriptors are found    */

/* ---------------------------------------------------------------------- */
/* Connection state                                                        */
/* ---------------------------------------------------------------------- */
static SemaphoreHandle_t s_lock;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_ready;        /* setup finished */

static call_tracker_t s_calls;
static ancs_ds_t s_ds;
static char s_pending_title[64];

static char s_artist[YAM_TEXT_MAX], s_album[YAM_TEXT_MAX], s_title[YAM_TEXT_MAX];
static bool s_playing;

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_advertising(void);

static void lock(void)   { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool iphone_link_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_ready;
}

/* ---------------------------------------------------------------------- */
/* Serial GATT operation queue (one request in flight at a time)           */
/* ---------------------------------------------------------------------- */
typedef enum { OP_WRITE, OP_READ } op_type_t;
typedef struct {
    op_type_t type;
    uint16_t handle;
    uint8_t data[8];
    uint8_t len;
    const char *what;
} op_t;

#define OPQ_SIZE 24
static op_t s_q[OPQ_SIZE];
static int s_q_head, s_q_count;
static bool s_op_busy;

static void handle_read_value(uint16_t handle, const uint8_t *data, size_t len);
static void op_kick(void);

static void op_done(const struct ble_gatt_error *err, const char *what)
{
    if (err->status != 0 && err->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "%s failed (status %d)%s", what, err->status,
                 err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
                         err->status == BLE_HS_ATT_ERR(0xA0 /* not authorized */)
                     ? ": allow notifications for t7bridge on the iPhone"
                     : "");
    }
}

static int on_write_done(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    lock();
    op_done(err, (const char *)arg);
    s_op_busy = false;
    op_kick();
    unlock();
    return 0;
}

static int on_read_done(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    lock();
    if (err->status == 0 && attr != NULL) {
        uint8_t buf[32];
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof buf) {
            len = sizeof buf;
        }
        if (os_mbuf_copydata(attr->om, 0, len, buf) == 0) {
            handle_read_value(attr->handle, buf, len);
        }
    } else {
        op_done(err, (const char *)arg);
    }
    s_op_busy = false;
    op_kick();
    unlock();
    return 0;
}

static void op_kick(void)
{
    while (!s_op_busy && s_q_count > 0 && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        op_t *op = &s_q[s_q_head];
        s_q_head = (s_q_head + 1) % OPQ_SIZE;
        s_q_count--;
        int rc;
        if (op->type == OP_WRITE) {
            rc = ble_gattc_write_flat(s_conn, op->handle, op->data, op->len, on_write_done,
                                      (void *)op->what);
        } else {
            rc = ble_gattc_read(s_conn, op->handle, on_read_done, (void *)op->what);
        }
        if (rc == 0) {
            s_op_busy = true;
        } else {
            ESP_LOGW(TAG, "%s could not start (rc %d)", op->what, rc);
        }
    }
}

static void op_enqueue(op_type_t type, uint16_t handle, const uint8_t *data, uint8_t len,
                       const char *what)
{
    if (handle == 0) {
        return;
    }
    lock();
    if (s_q_count == OPQ_SIZE) {
        ESP_LOGW(TAG, "command queue full; dropping %s", what);
    } else {
        op_t *op = &s_q[(s_q_head + s_q_count) % OPQ_SIZE];
        op->type = type;
        op->handle = handle;
        op->len = len;
        if (len) {
            memcpy(op->data, data, len);
        }
        op->what = what;
        s_q_count++;
        op_kick();
    }
    unlock();
}

/* ---------------------------------------------------------------------- */
/* Setup after discovery                                                   */
/* ---------------------------------------------------------------------- */
static void run_setup(void)
{
    static const uint8_t notify_on[2] = {0x01, 0x00};
    /* ANCS: Data Source before Notification Source (Apple's recommendation). */
    op_enqueue(OP_WRITE, s_chr[CH_DS].cccd, notify_on, 2, "subscribe ANCS data source");
    op_enqueue(OP_WRITE, s_chr[CH_NS].cccd, notify_on, 2, "subscribe ANCS notifications");
    /* AMS: subscribe, then register for the attributes we want. */
    op_enqueue(OP_WRITE, s_chr[CH_EU].cccd, notify_on, 2, "subscribe AMS entity updates");
    op_enqueue(OP_WRITE, s_chr[CH_RC].cccd, notify_on, 2, "subscribe AMS remote command");
    static const uint8_t player_attrs[] = {AMS_ENTITY_PLAYER, AMS_PLAYER_ATTR_PLAYBACK_INFO};
    static const uint8_t track_attrs[] = {AMS_ENTITY_TRACK, AMS_TRACK_ATTR_ARTIST,
                                          AMS_TRACK_ATTR_ALBUM, AMS_TRACK_ATTR_TITLE};
    op_enqueue(OP_WRITE, s_chr[CH_EU].val, player_attrs, sizeof player_attrs, "AMS player register");
    op_enqueue(OP_WRITE, s_chr[CH_EU].val, track_attrs, sizeof track_attrs, "AMS track register");
    /* Battery and clock: subscribe, then read the current value once. */
    op_enqueue(OP_WRITE, s_chr[CH_BATT].cccd, notify_on, 2, "subscribe battery");
    op_enqueue(OP_READ, s_chr[CH_BATT].val, NULL, 0, "read battery");
    op_enqueue(OP_WRITE, s_chr[CH_TIME].cccd, notify_on, 2, "subscribe current time");
    op_enqueue(OP_READ, s_chr[CH_TIME].val, NULL, 0, "read current time");
    s_ready = true;
    ESP_LOGI(TAG, "iPhone services: ANCS %s, AMS %s, Battery %s, Time %s",
             s_chr[CH_NS].val ? "yes" : "NO", s_chr[CH_EU].val ? "yes" : "NO",
             s_chr[CH_BATT].val ? "yes" : "no", s_chr[CH_TIME].val ? "yes" : "no");
    if (!s_chr[CH_NS].val || !s_chr[CH_EU].val) {
        ESP_LOGW(TAG, "ANCS/AMS missing: on the iPhone, open Settings > Bluetooth > t7bridge "
                      "and allow notifications, or forget t7bridge and pair again");
    }
}

/* ---------------------------------------------------------------------- */
/* Discovery chain                                                         */
/* ---------------------------------------------------------------------- */
static void disc_next_dsc(void);

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        chr_t *c = &s_chr[s_disc_chr];
        if (c->cccd == 0 && ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID16) {
            c->cccd = dsc->handle;
        }
        return 0;
    }
    s_disc_chr++;
    disc_next_dsc();
    return 0;
}

static void disc_next_dsc(void)
{
    while (s_disc_chr < CH_COUNT) {
        chr_t *c = &s_chr[s_disc_chr];
        if (c->needs_cccd && c->val != 0 && c->end > c->val) {
            if (ble_gattc_disc_all_dscs(s_conn, c->val, c->end, on_dsc, NULL) == 0) {
                return;
            }
        }
        s_disc_chr++;
    }
    run_setup();
}

static void disc_next_svc_chrs(void);

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        if (s_pending_end >= 0 && s_chr[s_pending_end].end == 0) {
            s_chr[s_pending_end].end = chr->def_handle - 1;
        }
        s_pending_end = -1;
        for (int i = 0; i < CH_COUNT; i++) {
            if (s_chr[i].svc == s_disc_svc && ble_uuid_cmp(&chr->uuid.u, s_chr[i].uuid) == 0) {
                s_chr[i].val = chr->val_handle;
                s_pending_end = i;
            }
        }
        return 0;
    }
    if (s_pending_end >= 0 && s_chr[s_pending_end].end == 0) {
        s_chr[s_pending_end].end = s_svc[s_disc_svc].end;
    }
    s_pending_end = -1;
    s_disc_svc++;
    disc_next_svc_chrs();
    return 0;
}

static void disc_next_svc_chrs(void)
{
    while (s_disc_svc < SVC_COUNT) {
        if (s_svc[s_disc_svc].start != 0) {
            s_pending_end = -1;
            if (ble_gattc_disc_all_chrs(s_conn, s_svc[s_disc_svc].start, s_svc[s_disc_svc].end,
                                        on_chr, NULL) == 0) {
                return;
            }
        }
        s_disc_svc++;
    }
    s_disc_chr = 0;
    disc_next_dsc();
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        for (int i = 0; i < SVC_COUNT; i++) {
            if (ble_uuid_cmp(&svc->uuid.u, SVC_UUIDS[i]) == 0) {
                s_svc[i].start = svc->start_handle;
                s_svc[i].end = svc->end_handle;
                ESP_LOGI(TAG, "found %s service", SVC_NAMES[i]);
            }
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "service discovery failed (status %d)", err->status);
    }
    s_disc_svc = 0;
    disc_next_svc_chrs();
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    memset(s_svc, 0, sizeof s_svc);
    for (int i = 0; i < CH_COUNT; i++) {
        s_chr[i].val = s_chr[i].end = s_chr[i].cccd = 0;
    }
    if (ble_gattc_disc_all_svcs(conn, on_svc, NULL) != 0) {
        ESP_LOGW(TAG, "could not start service discovery");
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Incoming data                                                           */
/* ---------------------------------------------------------------------- */
static void push_media(void)
{
    bridge_set_media(s_artist, s_album, s_title, s_playing);
}

static void handle_call_output(call_out_t out, uint32_t uid)
{
    switch (out) {
    case CALL_OUT_FETCH_TITLE: {
        uint8_t cmd[8];
        size_t n = ancs_build_get_title(cmd, uid, sizeof s_pending_title - 1);
        s_pending_title[0] = '\0';
        op_enqueue(OP_WRITE, s_chr[CH_CP].val, cmd, (uint8_t)n, "ANCS get caller");
        break;
    }
    case CALL_OUT_RINGING: {
        const char *t = s_pending_title;
        bool number = looks_like_phone_number(t);
        ESP_LOGI(TAG, "call: ringing \"%s\"", t[0] ? t : "(unknown)");
        bridge_call_ringing(number ? t : "", number ? "" : t);
        break;
    }
    case CALL_OUT_ACTIVE:
        ESP_LOGI(TAG, "call: active");
        bridge_call_active();
        break;
    case CALL_OUT_ENDED:
        ESP_LOGI(TAG, "call: ended");
        bridge_call_ended();
        break;
    default:
        break;
    }
}

static void handle_read_value(uint16_t handle, const uint8_t *data, size_t len)
{
    if (handle == s_chr[CH_BATT].val && len >= 1) {
        ESP_LOGI(TAG, "iPhone battery %u%%", data[0]);
        bridge_set_battery(data[0]);
    } else if (handle == s_chr[CH_TIME].val) {
        cts_time_t t;
        if (cts_parse_current_time(data, len, &t)) {
            ESP_LOGI(TAG, "iPhone time %04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day,
                     t.hour, t.minute, t.second);
            bridge_set_time_ymdhms(t.year, t.month, t.day, t.hour, t.minute, t.second);
        }
    }
}

static void handle_notification(uint16_t handle, const uint8_t *data, size_t len)
{
    lock();
    if (handle == s_chr[CH_NS].val) {
        ancs_notification_t n;
        if (ancs_parse_notification(data, len, &n)) {
            uint32_t uid = 0;
            handle_call_output(call_tracker_on_notification(&s_calls, &n, now_ms(), &uid), uid);
        }
    } else if (handle == s_chr[CH_DS].val) {
        uint32_t uid;
        char title[sizeof s_pending_title];
        if (ancs_ds_feed(&s_ds, data, len, &uid, title, sizeof title)) {
            memcpy(s_pending_title, title, sizeof title);
            handle_call_output(call_tracker_on_title(&s_calls, uid, title), uid);
        }
    } else if (handle == s_chr[CH_EU].val) {
        ams_update_t u;
        if (ams_parse_entity_update(data, len, &u)) {
            if (u.entity == AMS_ENTITY_PLAYER && u.attribute == AMS_PLAYER_ATTR_PLAYBACK_INFO) {
                s_playing = ams_playback_is_playing(u.value);
                ESP_LOGI(TAG, "AMS playback: %s", s_playing ? "playing" : "paused");
            } else if (u.entity == AMS_ENTITY_TRACK) {
                char *dst = u.attribute == AMS_TRACK_ATTR_ARTIST ? s_artist
                          : u.attribute == AMS_TRACK_ATTR_ALBUM  ? s_album
                          : u.attribute == AMS_TRACK_ATTR_TITLE  ? s_title : NULL;
                if (dst) {
                    strncpy(dst, u.value, YAM_TEXT_MAX - 1);
                    dst[YAM_TEXT_MAX - 1] = '\0';
                    ESP_LOGI(TAG, "AMS %s: %s",
                             u.attribute == AMS_TRACK_ATTR_ARTIST ? "artist"
                             : u.attribute == AMS_TRACK_ATTR_ALBUM ? "album" : "title", u.value);
                }
            }
            push_media();
        }
    } else {
        handle_read_value(handle, data, len);   /* battery / time notifications */
    }
    unlock();
}

/* ---------------------------------------------------------------------- */
/* Buttons and timers (bridge task context)                                */
/* ---------------------------------------------------------------------- */
void iphone_link_tick(int64_t now)
{
    (void)now;
    lock();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        handle_call_output(call_tracker_tick(&s_calls, now_ms()), 0);
    }
    unlock();
}

static void ams_command(uint8_t cmd, const char *what)
{
    op_enqueue(OP_WRITE, s_chr[CH_RC].val, &cmd, 1, what);
    ESP_LOGI(TAG, "-> AMS %s", what);
}

static void ancs_action(uint32_t uid, uint8_t action, const char *what)
{
    uint8_t cmd[6];
    size_t n = ancs_build_action(cmd, uid, action);
    op_enqueue(OP_WRITE, s_chr[CH_CP].val, cmd, (uint8_t)n, what);
    ESP_LOGI(TAG, "-> ANCS %s", what);
}

void iphone_link_handle_controls(uint8_t events)
{
    if (!iphone_link_connected()) {
        ESP_LOGI(TAG, "button ignored: iPhone not connected");
        return;
    }
    lock();
    uint32_t uid;
    if (events & YAM_EV_CALL_ANSWER) {
        if (call_tracker_answer_target(&s_calls, &uid)) {
            ancs_action(uid, ANCS_ACTION_POSITIVE, "answer call");
        }
    }
    if (events & YAM_EV_CALL_REJECT_END) {
        if (call_tracker_reject_target(&s_calls, &uid)) {
            ancs_action(uid, ANCS_ACTION_NEGATIVE,
                        s_calls.phase == CALLT_RINGING ? "decline call" : "end call");
        }
    }
    if (events & YAM_EV_MEDIA_TOGGLE)   ams_command(AMS_CMD_TOGGLE_PLAY_PAUSE, "toggle play/pause");
    if (events & YAM_EV_MEDIA_NEXT)     ams_command(AMS_CMD_NEXT_TRACK, "next track");
    if (events & YAM_EV_MEDIA_PREVIOUS) ams_command(AMS_CMD_PREVIOUS_TRACK, "previous track");
    if (events & YAM_EV_VOLUME_UP)      ams_command(AMS_CMD_VOLUME_UP, "volume up");
    if (events & YAM_EV_VOLUME_DOWN)    ams_command(AMS_CMD_VOLUME_DOWN, "volume down");
    unlock();
}

/* ---------------------------------------------------------------------- */
/* GAP events for the iPhone connection (NimBLE host task)                  */
/* ---------------------------------------------------------------------- */
static void reset_session(void)
{
    s_ready = false;
    s_q_head = s_q_count = 0;
    s_op_busy = false;
    bool in_call = s_calls.phase != CALLT_IDLE;
    call_tracker_reset(&s_calls);
    ancs_ds_reset(&s_ds);
    s_artist[0] = s_album[0] = s_title[0] = '\0';
    s_playing = false;
    if (in_call) {
        bridge_call_ended();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            start_advertising();
            return 0;
        }
        lock();
        s_conn = event->connect.conn_handle;
        reset_session();
        unlock();
        ESP_LOGI(TAG, "iPhone connected (handle %u); securing link", s_conn);
        /* Ask the iPhone to encrypt: it pairs the first time (iOS shows a
         * prompt) and uses the stored key afterwards. */
        ble_gap_security_initiate(s_conn);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "iPhone link encrypted; discovering ANCS/AMS");
            if (ble_gattc_exchange_mtu(event->enc_change.conn_handle, on_mtu, NULL) != 0) {
                struct ble_gatt_error e = {0};
                on_mtu(event->enc_change.conn_handle, &e, 23, NULL);
            }
        } else {
            ESP_LOGW(TAG, "iPhone encryption failed (0x%x); if this repeats, forget t7bridge "
                          "on the iPhone and pair again", event->enc_change.status);
            if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) &&
                ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(TAG, "pairing code %06" PRIu32 ": accept on the iPhone if it matches",
                     event->passkey.params.numcmp);
            struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1};
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == s_conn) {
            uint8_t buf[260];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > sizeof buf) {
                len = sizeof buf;
            }
            if (os_mbuf_copydata(event->notify_rx.om, 0, len, buf) == 0) {
                handle_notification(event->notify_rx.attr_handle, buf, len);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "iPhone disconnected (reason 0x%x)", event->disconnect.reason);
        lock();
        reset_session();
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        unlock();
        bridge_phone_disconnected();
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            start_advertising();
        }
        return 0;

    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Advertising                                                             */
/* ---------------------------------------------------------------------- */
static volatile bool s_adv_suspended;   /* learn mode is using the advertiser */

void iphone_link_suspend_advertising(bool suspend)
{
    s_adv_suspended = suspend;
    if (suspend) {
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
        }
        ESP_LOGI(TAG, "\"t7bridge\" advertising paused for learn mode");
    } else if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        start_advertising();
    }
}

static void start_advertising(void)
{
    if (s_adv_suspended || ble_gap_adv_active()) {
        return;
    }
    /* Advertising packet: flags + "solicits ANCS" + name = 31 bytes.
     * Soliciting ANCS is what makes iOS list t7bridge in Bluetooth settings. */
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.sol_uuids128 = &ANCS_SVC;
    f.sol_num_uuids128 = 1;
    f.name = (const uint8_t *)"t7bridge";
    f.name_len = 8;
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising data rejected (rc %d)", rc);
        return;
    }
    /* Scan response: also solicit AMS. */
    struct ble_hs_adv_fields r = {0};
    r.sol_uuids128 = &AMS_SVC;
    r.sol_num_uuids128 = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan response rejected (rc %d)", rc);
    }
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 160;   /* 100 ms */
    p.itvl_max = 240;   /* 150 ms */
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising start failed (rc %d)", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"t7bridge\" for the iPhone");
}

void iphone_link_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    call_tracker_reset(&s_calls);
}

void iphone_link_on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    start_advertising();
}
