/*
 * T7 Bridge for ESP32-S3
 *
 * Replaces the Raspberry Pi bridge between a Yamaha Tenere 700 CCU and an
 * iPhone, in one board:
 *   CCU side (central):  connect, pair, Yamaha session-init, 0x60/0x61/0x62
 *                        status frames, handlebar buttons.
 *   iPhone side (peripheral + client): advertise as "t7bridge", pair, then
 *                        AMS (music), ANCS (calls), battery and clock.
 *   Learn mode: while the bike's pairing code is unknown, pose as the CCU so
 *                        the Y-Connect app hands it over (learn_mode.c).
 *   Updates: 5x volume down starts the "t7bridge-update" Wi-Fi for uploading
 *                        new firmware from a phone (ota_update.c).
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "bridge.h"
#include "ota_update.h"
#include "creds.h"
#include "learn_mode.h"

static const char *TAG = "main";

void ble_store_config_init(void);

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

static void on_sync(void)
{
    bridge_on_ble_sync();
}

static void host_task(void *param)
{
    nimble_port_run();              /* returns only when nimble_port_stop() runs */
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    ESP_LOGI(TAG, "T7 Bridge ESP32 - CCU + iPhone");

    esp_err_t err = nvs_flash_init();   /* also stores BLE pairing keys */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    creds_init();

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %d", err);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Security: bond, no keyboard/display ("Just Works"), like a phone pairing
     * with the CCU. Secure Connections offered; the CCU may choose legacy. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_device_name_set("t7bridge");
    ble_store_config_init();
    learn_mode_register_gatt();   /* CCU-style UART service for learn mode */

    bridge_start();
    ota_update_init();
    nimble_port_freertos_init(host_task);
}
