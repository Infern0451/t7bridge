/*
 * Bike credentials in NVS. See creds.h.
 */
#include "creds.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "yamaha_proto.h"

static const char *TAG = "creds";
static const char *NS = "t7creds";

static char s_token[YAM_TOKEN_LEN + 1];
static char s_ccu_id[YAM_CCU_ID_LEN + 1];
static char s_code[YAM_PAIRING_CODE_LEN + 1];
static SemaphoreHandle_t s_lock;

static bool all_chars(const char *s, size_t want, int (*ok)(int))
{
    if (s == NULL || strlen(s) != want) {
        return false;
    }
    for (size_t i = 0; i < want; i++) {
        if (!ok((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

bool creds_valid_ccu_id(const char *id)
{
    return all_chars(id, YAM_CCU_ID_LEN, isalnum);
}

bool creds_valid_code(const char *code)
{
    return all_chars(code, YAM_PAIRING_CODE_LEN, isgraph);
}

static bool valid_token(const char *t)
{
    return all_chars(t, YAM_TOKEN_LEN, isgraph);
}

static void load(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        dst[0] = 0;
    }
}

static bool save(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open storage");
        return false;
    }
    esp_err_t e = (val && val[0]) ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        e = ESP_OK;
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "saving %s failed (%d)", key, e);
    }
    return e == ESP_OK;
}

void creds_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    char seeded[2] = "";
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        load(h, "seeded", seeded, sizeof seeded);
        load(h, "token", s_token, sizeof s_token);
        load(h, "ccu_id", s_ccu_id, sizeof s_ccu_id);
        load(h, "code", s_code, sizeof s_code);
        nvs_close(h);
    }
    /* Starting values from menuconfig, used once on a board's first start
     * (so "forget bike" really forgets). */
    if (seeded[0]) {
        goto seeded_done;
    }
    if (!valid_token(s_token) && valid_token(CONFIG_T7_TOKEN)) {
        strcpy(s_token, CONFIG_T7_TOKEN);
        save("token", s_token);
    }
    if (!creds_valid_ccu_id(s_ccu_id) && creds_valid_ccu_id(CONFIG_T7_CCU_ID)) {
        strcpy(s_ccu_id, CONFIG_T7_CCU_ID);
        save("ccu_id", s_ccu_id);
    }
    if (!creds_valid_code(s_code) && creds_valid_code(CONFIG_T7_PAIRING_CODE)) {
        strcpy(s_code, CONFIG_T7_PAIRING_CODE);
        save("code", s_code);
    }
    save("seeded", "1");
seeded_done:
    if (!valid_token(s_token)) {
        /* New bridge: make up a token (32 hex digits, like the Yamaha app). */
        uint8_t r[16];
        esp_fill_random(r, sizeof r);
        for (int i = 0; i < 16; i++) {
            snprintf(s_token + 2 * i, 3, "%02x", r[i]);
        }
        save("token", s_token);
        ESP_LOGI(TAG, "new bridge: created token %.4s...", s_token);
    }
    if (!creds_valid_ccu_id(s_ccu_id)) {
        s_ccu_id[0] = 0;
    }
    if (!creds_valid_code(s_code)) {
        s_code[0] = 0;
    }
    ESP_LOGI(TAG, "token %.4s..., CCU ID %s, pairing code %s", s_token,
             s_ccu_id[0] ? s_ccu_id : "(not found yet)",
             s_code[0] ? "stored" : "(not known yet: learn mode)");
}

const char *creds_token(void)        { return s_token; }
const char *creds_ccu_id(void)       { return s_ccu_id; }
const char *creds_pairing_code(void) { return s_code; }
bool creds_have_ccu_id(void)         { return s_ccu_id[0] != 0; }
bool creds_have_code(void)           { return s_code[0] != 0; }

bool creds_set_ccu_id(const char *id)
{
    if (!creds_valid_ccu_id(id)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = strcmp(s_ccu_id, id) != 0;
    if (changed) {
        strcpy(s_ccu_id, id);
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        save("ccu_id", id);
        ESP_LOGI(TAG, "CCU ID saved: %s", id);
    }
    return true;
}

bool creds_set_pairing_code(const char *code)
{
    if (!creds_valid_code(code)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = strcmp(s_code, code) != 0;
    if (changed) {
        strcpy(s_code, code);
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        save("code", code);
        ESP_LOGI(TAG, "pairing code saved");
    }
    return true;
}

void creds_forget_bike(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ccu_id[0] = 0;
    s_code[0] = 0;
    xSemaphoreGive(s_lock);
    save("ccu_id", NULL);
    save("code", NULL);
    ESP_LOGW(TAG, "bike forgotten (CCU ID and pairing code erased)");
}
