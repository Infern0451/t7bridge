/*
 * Learn mode. See learn_mode.h.
 */
#include "learn_mode.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"

#include "creds.h"
#include "iphone_link.h"
#include "ota_update.h"
#include "yamaha_proto.h"

static const char *TAG = "learn";

#define BOOT_GPIO          GPIO_NUM_0
#define RESET_HOLD_MS      5000
#define FINISH_DELAY_MS    8000     /* keep the app link a while after the ACK */

/* Nordic UART Service, same UUIDs as the CCU. */
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static uint16_t s_tx_val;
static volatile bool s_synced;
static volatile bool s_running;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile int64_t s_finish_at;      /* 0 = not finishing */
static uint8_t s_own_addr_type;
static char s_name[32];

static uint8_t s_buf[YAM_LEN_AUTH];
static size_t s_buf_len;
static int s_other_writes;

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool learn_mode_active(void)
{
    return s_running;
}

/* ---------------------------------------------------------------------- */
/* GATT server: what the app sees as the CCU's UART                         */
/* ---------------------------------------------------------------------- */

static void send_ack(uint16_t conn)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(YAM_ACK, YAM_LEN_ACK);
    if (om == NULL || ble_gatts_notify_custom(conn, s_tx_val, om) != 0) {
        ESP_LOGW(TAG, "could not send the ACK to the app (code is saved anyway)");
    } else {
        ESP_LOGI(TAG, "sent the CCU's ACK to the app");
    }
}

static void handle_auth_frame(uint16_t conn, const uint8_t *f)
{
    yam_auth_info_t info;
    if (!yam_parse_auth(f, YAM_LEN_AUTH, &info)) {
        ESP_LOGW(TAG, "app sent a 0xAA frame with a bad checksum; ignored");
        return;
    }
    if (info.mode == YAM_AUTH_RECONNECT) {
        ESP_LOGW(TAG, "app sent the RECONNECT form: it thinks it is already paired. Waiting "
                      "for the pairing form; if none comes, remove the bike in Y-Connect and "
                      "pair it again.");
        return;   /* no ACK: the app then sends the initial-pair form */
    }
    ESP_LOGI(TAG, "app sent the PAIRING form: CCU ID %s, pairing code ******",
             info.ccu_id);
    if (strcmp(info.ccu_id, creds_ccu_id()) != 0) {
        ESP_LOGW(TAG, "the app's CCU ID differs from ours (%s); using the app's", creds_ccu_id());
        if (!creds_set_ccu_id(info.ccu_id)) {
            ESP_LOGE(TAG, "app's CCU ID is not valid; not saved");
            return;
        }
    }
    /* Set the finish time first: the bridge task ends learn mode as soon as
     * it sees a stored code. */
    s_finish_at = now_ms() + FINISH_DELAY_MS;
    if (!creds_set_pairing_code(info.pairing_code)) {
        s_finish_at = 0;
        ESP_LOGE(TAG, "app's pairing code is not valid; not saved");
        return;
    }
    ESP_LOGI(TAG, "*** pairing code learned and saved (shown on the setup page) ***");
    send_ack(conn);
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t data[256];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof data) {
        len = sizeof data;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, data, len, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!s_running) {
        return 0;   /* not in learn mode: ignore quietly */
    }
    if (s_buf_len == 0 && (len == 0 || data[0] != 0xAA)) {
        if (s_other_writes++ < 20) {
            ESP_LOGI(TAG, "app wrote %u bytes (not session-init):", len);
            ESP_LOG_BUFFER_HEX(TAG, data, len);
        }
        return 0;
    }
    /* Session-init carries the pairing code and the app's token: log only
     * the length, so logs are safe to share. */
    ESP_LOGI(TAG, "app wrote %u bytes of session-init", len);
    for (uint16_t i = 0; i < len; i++) {
        s_buf[s_buf_len++] = data[i];
        if (s_buf_len == YAM_LEN_AUTH) {
            handle_auth_frame(conn, s_buf);
            s_buf_len = 0;
        }
    }
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &NUS_RX.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &NUS_TX.u,
                .access_cb = tx_access,
                .val_handle = &s_tx_val,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

void learn_mode_register_gatt(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "UART service registration failed (rc %d)", rc);
    }
}

/* ---------------------------------------------------------------------- */
/* Advertising as the CCU                                                    */
/* ---------------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    if (!s_running || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return;
    }
    /* Same as the real CCU: flags + complete name, nothing else. */
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising data rejected (rc %d)", rc);
        return;
    }
    struct ble_hs_adv_fields r = {0};
    ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 160;
    p.itvl_max = 240;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising start failed (rc %d)", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\": pair the bike in Y-Connect now", s_name);
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
        s_conn = event->connect.conn_handle;
        s_buf_len = 0;
        s_other_writes = 0;
        if (ble_gap_conn_find(s_conn, &desc) == 0) {
            ESP_LOGI(TAG, "app connected (handle %u, from %02X:%02X:%02X:%02X:%02X:%02X)", s_conn,
                     desc.peer_ota_addr.val[5], desc.peer_ota_addr.val[4], desc.peer_ota_addr.val[3],
                     desc.peer_ota_addr.val[2], desc.peer_ota_addr.val[1], desc.peer_ota_addr.val[0]);
        }
        /* No Bluetooth pairing here: the app sends the code without it, and a
         * bond made under the CCU's name would only confuse iOS later, when the
         * same phone pairs with "t7bridge" (same Bluetooth address). */
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "app link encryption: %s (0x%x)",
                 event->enc_change.status == 0 ? "on" : "failed", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "app %s notifications on handle %u",
                 event->subscribe.cur_notify ? "enabled" : "disabled", event->subscribe.attr_handle);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "app MTU %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "app disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------- */
/* Start / stop                                                              */
/* ---------------------------------------------------------------------- */

static void start(void)
{
    snprintf(s_name, sizeof s_name, "YCCU_%s", creds_ccu_id());
    s_running = true;
    s_finish_at = 0;
    iphone_link_suspend_advertising(true);
    ble_svc_gap_device_name_set(s_name);
    ESP_LOGW(TAG, "LEARN MODE. Turn the ignition OFF (or move away from the bike), then pair "
                  "the bike in the Yamaha Y-Connect app. It will connect to this bridge.");
    start_advertising();
}

static void stop(void)
{
    s_running = false;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    ble_svc_gap_device_name_set("t7bridge");
    iphone_link_suspend_advertising(false);
    ESP_LOGI(TAG, "learn mode finished; the bridge will now pair with the real CCU. "
                  "Turn the ignition on. You can delete Y-Connect from the phone you used.");
}

void learn_mode_on_sync(void)
{
    /* Pose as the CCU under a separate random address. iOS remembers the
     * "CCU" by address and Y-Connect keeps reconnecting to it; with the real
     * address it would land on "t7bridge" afterwards and pop up pairing
     * requests. This address simply disappears when learn mode ends. */
    ble_addr_t rnd;
    if (ble_hs_id_gen_rnd(0, &rnd) == 0 && ble_hs_id_set_rnd(rnd.val) == 0) {
        s_own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    s_synced = true;
}

void learn_mode_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: forgetting the bike and all Bluetooth pairings");
    creds_forget_bike();
    ble_store_clear();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void check_boot_button(int64_t now)
{
    static bool configured;
    static int64_t pressed_since;
    if (!configured) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << BOOT_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&io);
        configured = true;
    }
    if (gpio_get_level(BOOT_GPIO) == 0) {
        if (pressed_since == 0) {
            pressed_since = now;
            ESP_LOGI(TAG, "BOOT pressed: keep holding 5 s to forget the bike");
        } else if (now - pressed_since >= RESET_HOLD_MS) {
            learn_mode_factory_reset();
        }
    } else {
        pressed_since = 0;
    }
}

void learn_mode_tick(int64_t now)
{
    (void)now;
    int64_t t = now_ms();
    check_boot_button(t);

    /* Keep the setup Wi-Fi on for as long as the bike's code is missing. */
    ota_update_hold(!creds_have_code());

    if (!s_synced) {
        return;
    }
    bool needed = creds_have_ccu_id() && !creds_have_code();
    if (needed && !s_running) {
        start();
    } else if (s_running && creds_have_code() &&
               (s_finish_at == 0 || t >= s_finish_at)) {
        /* Code from the app (after a short pause for the ACK) or typed in. */
        stop();
    }
}
