/*
 * BLE central link to the Yamaha CCU. See ccu_link.h for the sequence.
 */
#include "ccu_link.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "sdkconfig.h"

#include "creds.h"
#include "yamaha_proto.h"

static const char *TAG = "ccu";

/* NUS UUIDs (little-endian byte order as NimBLE expects). */
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX = BLE_UUID128_INIT(   /* phone -> CCU (write)  */
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX = BLE_UUID128_INIT(   /* CCU -> phone (notify) */
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

#define CCCD_UUID16          0x2902
#define CHR_DECL_UUID16      0x2803

#define RESCAN_DELAY_MS      1500    /* after a disconnect or failure            */
#define RECONNECT_AUTH_MS    6000    /* reconnect-mode AA, then initial-pair AA   */
#define AUTH_GIVE_UP_MS      16000   /* no ACK at all: drop the link and retry    */

static ccu_link_callbacks_t s_cb;
static volatile ccu_state_t s_state = CCU_IDLE;
static bool s_synced;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static int64_t s_next_scan_ms;

/* Discovery results */
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_rx_handle, s_tx_handle, s_tx_cccd;

/* Session-init progress */
static yam_ack_recognizer_t s_ack;
static int64_t s_auth_started_ms;
static bool s_initial_sent;
static volatile bool s_auth_pending_send;   /* set by host task, sent by tick */
static int s_initial_failures;              /* initial-pair attempts without ACK */

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_discovery(void);

/* --------------------------------------------------------------------- */

const char *ccu_state_name(ccu_state_t state)
{
    switch (state) {
    case CCU_IDLE:           return "idle";
    case CCU_SCANNING:       return "scanning";
    case CCU_CONNECTING:     return "connecting";
    case CCU_SECURING:       return "securing";
    case CCU_DISCOVERING:    return "discovering";
    case CCU_AUTHENTICATING: return "authenticating";
    case CCU_ACTIVE:         return "active";
    default:                 return "?";
    }
}

static void set_state(ccu_state_t st)
{
    if (s_state != st) {
        ESP_LOGI(TAG, "state: %s -> %s", ccu_state_name(s_state), ccu_state_name(st));
        s_state = st;
    }
}

ccu_state_t ccu_link_state(void)
{
    return s_state;
}

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void fail_and_rescan(const char *why)
{
    ESP_LOGW(TAG, "%s; retrying in %d ms", why, RESCAN_DELAY_MS);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        /* DISCONNECT event will finish the cleanup. */
        return;
    }
    s_next_scan_ms = now_ms() + RESCAN_DELAY_MS;
    set_state(CCU_IDLE);
}

/* ---------------------------------------------------------------------- */
/* Scanning                                                                */
/* ---------------------------------------------------------------------- */

#define YCCU_PREFIX "YCCU_"
#define YCCU_PREFIX_LEN 5

/* Our CCU, or (while the CCU ID is not known yet) any Yamaha CCU. */
static bool name_matches(const uint8_t *name, uint8_t len, char found_id[YAM_CCU_ID_LEN + 1])
{
    if (len != YCCU_PREFIX_LEN + YAM_CCU_ID_LEN || memcmp(name, YCCU_PREFIX, YCCU_PREFIX_LEN) != 0) {
        return false;
    }
    memcpy(found_id, name + YCCU_PREFIX_LEN, YAM_CCU_ID_LEN);
    found_id[YAM_CCU_ID_LEN] = 0;
    if (!creds_have_ccu_id()) {
        return creds_valid_ccu_id(found_id);
    }
    return strcmp(found_id, creds_ccu_id()) == 0;
}

/* Without a pairing code there is no point connecting: wait for learn mode. */
static bool parked(void)
{
    return creds_have_ccu_id() && !creds_have_code();
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {0};
    params.passive = 0;             /* active: also get scan responses */
    params.filter_duplicates = 1;
    params.itvl = 0x50;             /* 50 ms */
    params.window = 0x30;           /* 30 ms */
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed rc=%d", rc);
        s_next_scan_ms = now_ms() + RESCAN_DELAY_MS;
        return;
    }
    set_state(CCU_SCANNING);
    if (creds_have_ccu_id()) {
        ESP_LOGI(TAG, "scanning for YCCU_%s", creds_ccu_id());
    } else {
        ESP_LOGI(TAG, "scanning for any Yamaha CCU (YCCU_...) to learn its ID; "
                      "turn the ignition on");
    }
}

static void connect_to(const ble_addr_t *addr)
{
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan cancel rc=%d", rc);
    }
    /* Same link settings the Android app used: 30 ms interval, no latency,
     * 5 s supervision timeout. Standard 1M PHY (we never request 2M). */
    struct ble_gap_conn_params cp = {
        .scan_itvl = 0x30,
        .scan_window = 0x30,
        .itvl_min = 24,
        .itvl_max = 24,
        .latency = 0,
        .supervision_timeout = CONFIG_T7_SUPERVISION_TIMEOUT_MS / 10,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    rc = ble_gap_connect(s_own_addr_type, addr, 15000, &cp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "connect failed to start rc=%d", rc);
        s_next_scan_ms = now_ms() + RESCAN_DELAY_MS;
        set_state(CCU_IDLE);
        return;
    }
    set_state(CCU_CONNECTING);
}

/* ---------------------------------------------------------------------- */
/* Discovery and subscription                                              */
/* ---------------------------------------------------------------------- */

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        ESP_LOGE(TAG, "enabling TX notifications failed status=%d", err->status);
        fail_and_rescan("subscription failed");
        return 0;
    }
    ESP_LOGI(TAG, "GATT: NUS RX/TX discovered; TX notifications enabled (MTU %u)",
             ble_att_mtu(conn));
    yam_ack_reset(&s_ack);
    s_initial_sent = false;
    s_auth_started_ms = now_ms();
    s_auth_pending_send = true;     /* the bridge task sends the AA frame */
    set_state(CCU_AUTHENTICATING);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        if (s_tx_cccd == 0 && ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID16) {
            s_tx_cccd = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_and_rescan("descriptor discovery failed");
        return 0;
    }
    if (s_tx_cccd == 0) {
        fail_and_rescan("TX notify descriptor (CCCD) not found");
        return 0;
    }
    static const uint8_t enable[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn, s_tx_cccd, enable, sizeof enable, on_cccd_written, NULL);
    if (rc != 0) {
        fail_and_rescan("CCCD write could not start");
    }
    return 0;
}

static uint16_t s_tx_end;   /* last handle that can belong to the TX characteristic */

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        /* Track where the TX characteristic ends: next characteristic's declaration. */
        if (s_tx_handle != 0 && s_tx_end == 0 && chr->def_handle > s_tx_handle) {
            s_tx_end = chr->def_handle - 1;
        }
        if (ble_uuid_cmp(&chr->uuid.u, &NUS_RX.u) == 0) {
            s_rx_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &NUS_TX.u) == 0) {
            s_tx_handle = chr->val_handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_and_rescan("characteristic discovery failed");
        return 0;
    }
    if (s_rx_handle == 0 || s_tx_handle == 0) {
        fail_and_rescan("NUS RX/TX characteristics not found");
        return 0;
    }
    if (s_tx_end == 0) {
        s_tx_end = s_svc_end;
    }
    int rc = ble_gattc_disc_all_dscs(conn, s_tx_handle, s_tx_end, on_dsc, NULL);
    if (rc != 0) {
        fail_and_rescan("descriptor discovery could not start");
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_and_rescan("service discovery failed");
        return 0;
    }
    if (s_svc_start == 0) {
        fail_and_rescan("NUS service not found on CCU");
        return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    if (rc != 0) {
        fail_and_rescan("characteristic discovery could not start");
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status != 0) {
        ESP_LOGW(TAG, "MTU exchange failed status=%d; continuing with default", err->status);
    }
    s_svc_start = s_svc_end = 0;
    s_rx_handle = s_tx_handle = s_tx_cccd = s_tx_end = 0;
    int rc = ble_gattc_disc_svc_by_uuid(conn, &NUS_SVC.u, on_svc, NULL);
    if (rc != 0) {
        fail_and_rescan("service discovery could not start");
    }
    return 0;
}

static void start_discovery(void)
{
    set_state(CCU_DISCOVERING);
    int rc = ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
    if (rc != 0) {
        struct ble_gatt_error e = {.status = rc};
        on_mtu(s_conn, &e, 23, NULL);
    }
}

/* ---------------------------------------------------------------------- */
/* Writes                                                                  */
/* ---------------------------------------------------------------------- */

static bool write_chunked(const uint8_t *data, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_rx_handle == 0) {
        return false;
    }
    size_t chunk = ble_att_mtu(s_conn);
    chunk = chunk > 3 ? chunk - 3 : 20;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off < chunk ? len - off : chunk;
        int rc;
        int tries = 0;
        do {
            rc = ble_gattc_write_no_rsp_flat(s_conn, s_rx_handle, data + off, (uint16_t)n);
            if (rc == BLE_HS_ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(10));   /* host buffers busy; retry */
            }
        } while (rc == BLE_HS_ENOMEM && ++tries < 20);
        if (rc != 0) {
            ESP_LOGW(TAG, "write failed rc=%d", rc);
            return false;
        }
        off += n;
    }
    return true;
}

bool ccu_link_send(const uint8_t *frame, size_t len)
{
    if (s_state != CCU_ACTIVE) {
        return false;
    }
    return write_chunked(frame, len);
}

static void send_auth(yam_auth_mode_t mode)
{
    uint8_t frame[YAM_LEN_AUTH];
    if (!yam_build_auth(frame, mode, creds_token(), creds_ccu_id(), creds_pairing_code())) {
        ESP_LOGE(TAG, "session-init NOT sent: stored credentials incomplete");
        return;
    }
    ESP_LOGI(TAG, "TX session-init (0xAA) mode=%s length=60",
             mode == YAM_AUTH_INITIAL_PAIR ? "initial-pair" : "reconnect");
    if (!write_chunked(frame, sizeof frame)) {
        ESP_LOGW(TAG, "session-init write failed");
    }
}

/* ---------------------------------------------------------------------- */
/* Periodic supervision (bridge task context)                              */
/* ---------------------------------------------------------------------- */

void ccu_link_tick(int64_t now)
{
    if (!s_synced) {
        return;
    }
    switch (s_state) {
    case CCU_IDLE:
        if (!parked() && now >= s_next_scan_ms) {
            start_scan();
        }
        break;
    case CCU_AUTHENTICATING:
        if (s_auth_pending_send) {
            s_auth_pending_send = false;
            send_auth(YAM_AUTH_RECONNECT);
        }
        /* The Android app sends reconnect mode first and, if the CCU does not
         * answer within ~6 s, the initial-pair form with CCU ID + code. */
        if (!s_initial_sent && now - s_auth_started_ms >= RECONNECT_AUTH_MS) {
            s_initial_sent = true;
            ESP_LOGW(TAG, "no ACK to reconnect session-init; trying initial-pair form");
            send_auth(YAM_AUTH_INITIAL_PAIR);
        }
        if (now - s_auth_started_ms >= AUTH_GIVE_UP_MS) {
            if (++s_initial_failures >= 3) {
                ESP_LOGE(TAG, "the CCU ignored our pairing code %d times in a row. If this "
                              "persists the code is probably wrong: set it again on the setup "
                              "page or hold BOOT for 5 s to learn it again", s_initial_failures);
            }
            fail_and_rescan("Yamaha ACK not received");
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* GAP events (NimBLE host task context)                                   */
/* ---------------------------------------------------------------------- */

static void handle_notification(const uint8_t *data, size_t len)
{
    if (s_state == CCU_AUTHENTICATING) {
        ESP_LOGI(TAG, "RX during session-init (%u bytes):", (unsigned)len);
        ESP_LOG_BUFFER_HEX(TAG, data, len < 16 ? len : 16);
        size_t used = 0;
        yam_ack_result_t r = yam_ack_feed(&s_ack, data, len, &used);
        if (r == YAM_ACK_ACCEPTED) {
            ESP_LOGI(TAG, "RX Yamaha ACK 5A017F0101010023; checksum valid; session active");
            s_initial_failures = 0;
            set_state(CCU_ACTIVE);
            if (s_cb.session_active) {
                s_cb.session_active();
            }
            if (used < len && s_cb.rx_chunk) {
                s_cb.rx_chunk(data + used, len - used);
            }
        } else if (r == YAM_ACK_INVALID) {
            ESP_LOGE(TAG, "invalid 5A reply from CCU");
            fail_and_rescan("authorization rejected");
        }
        return;
    }
    if (s_state == CCU_ACTIVE && s_cb.rx_chunk) {
        s_cb.rx_chunk(data, len);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        char id[YAM_CCU_ID_LEN + 1];
        if (s_state != CCU_SCANNING ||
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        if (fields.name != NULL && name_matches(fields.name, fields.name_len, id)) {
            ESP_LOGI(TAG, "found %.*s RSSI %d", fields.name_len, (const char *)fields.name,
                     event->disc.rssi);
            if (!creds_have_ccu_id()) {
                creds_set_ccu_id(id);
                ESP_LOGI(TAG, "learned CCU ID %s", id);
            }
            if (!creds_have_code()) {
                ble_gap_disc_cancel();
                set_state(CCU_IDLE);
                ESP_LOGW(TAG, "no pairing code yet: learn mode (see the setup page)");
                return 0;
            }
            connect_to(&event->disc.addr);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_state == CCU_SCANNING) {
            set_state(CCU_IDLE);
            s_next_scan_ms = now_ms();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection attempt failed status=%d", event->connect.status);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_next_scan_ms = now_ms() + RESCAN_DELAY_MS;
            set_state(CCU_IDLE);
            return 0;
        }
        s_conn = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected (handle %u)", s_conn);
        set_state(CCU_SECURING);
        /* Encrypt straight away, like the Android app: pairs ("Just Works")
         * the first time, uses the stored key afterwards. */
        if (ble_gap_security_initiate(s_conn) != 0) {
            fail_and_rescan("could not start encryption");
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "link encrypted");
            start_discovery();
            return 0;
        }
        ESP_LOGW(TAG, "encryption failed status=0x%x", event->enc_change.status);
        if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) &&
            ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            /* The CCU no longer knows our key (e.g. it was reset or paired
             * elsewhere). Forget ours so the next attempt pairs fresh. */
            ESP_LOGW(TAG, "CCU lost our pairing; deleting local bond so we re-pair");
            ble_store_util_delete_peer(&desc.peer_id_addr);
        } else {
            ESP_LOGW(TAG, "if this is the first pairing, open the Bluetooth/pairing "
                          "screen on the TFT and let it retry");
        }
        fail_and_rescan("encryption failed");
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* CCU wants to pair again although we hold a bond: replace the old one. */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_tx_handle) {
            uint8_t buf[520];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > sizeof buf) {
                len = sizeof buf;
            }
            if (os_mbuf_copydata(event->notify_rx.om, 0, len, buf) == 0) {
                handle_notification(buf, len);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        bool was_active = (s_state == CCU_ACTIVE);
        ESP_LOGW(TAG, "disconnected reason=0x%x%s", event->disconnect.reason,
                 event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO)
                     ? " (supervision timeout: bike off or out of range)" : "");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_rx_handle = s_tx_handle = s_tx_cccd = 0;
        s_auth_pending_send = false;
        s_next_scan_ms = now_ms() + RESCAN_DELAY_MS;
        set_state(CCU_IDLE);
        if (was_active && s_cb.session_ended) {
            s_cb.session_ended();
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------- */

void ccu_link_init(const ccu_link_callbacks_t *callbacks)
{
    s_cb = *callbacks;
}

void ccu_link_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address rc=%d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "bridge BLE address %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    s_next_scan_ms = now_ms();
    s_synced = true;
}
