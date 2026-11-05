/*
 * Bridge logic. See bridge.h.
 */
#include "bridge.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ccu_link.h"
#include "iphone_link.h"
#include "ota_update.h"
#include "learn_mode.h"
#include "yamaha_proto.h"

static const char *TAG = "bridge";

#define TICK_MS 20

static SemaphoreHandle_t s_lock;
static yam_state_t s_state;
static yam_counters_t s_counters;
static yam_scheduler_t s_sched;
static yam_ctrl_parser_t s_ctrl;
static bool s_ctrl_pending;
static bridge_control_hook_t s_control_hook;

/* Status text override for the TFT track fields (update mode). */
static bool s_status_on;
static char s_status_artist[YAM_TEXT_MAX], s_status_album[YAM_TEXT_MAX], s_status_title[YAM_TEXT_MAX];

/* Update-mode trigger: 5 volume-down presses within 4 seconds. */
#define COMBO_PRESSES 5
#define COMBO_WINDOW_MS 4000
static int64_t s_vol_down_times[COMBO_PRESSES];
static int s_vol_down_idx;

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---------------------------------------------------------------------- */
/* Clock for 0x62                                                          */
/* ---------------------------------------------------------------------- */
/* Until the iPhone provides real time (Current Time Service), use the
 * firmware build time plus uptime. All times are local wall-clock values;
 * no time zone is set, so mktime/gmtime_r are exact inverses. */
static time_t s_clock_base;     /* wall-clock time at uptime 0 */
static bool s_clock_from_phone;

static void clock_init(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2026, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *p = strstr(months, mon);
    struct tm t = {
        .tm_year = year - 1900,
        .tm_mon = p ? (int)(p - months) / 3 : 0,
        .tm_mday = day,
        .tm_hour = hh,
        .tm_min = mm,
        .tm_sec = ss,
    };
    s_clock_base = mktime(&t);
}

static void clock_set(uint16_t year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second)
{
    struct tm t = {
        .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = minute, .tm_sec = second,
    };
    s_clock_base = mktime(&t) - (time_t)(uptime_ms() / 1000);
    s_clock_from_phone = true;
}

static void clock_now(yam_time_t *out)
{
    time_t now = s_clock_base + (time_t)(uptime_ms() / 1000);
    struct tm t;
    gmtime_r(&now, &t);   /* base was built with mktime in UTC (no TZ set) */
    out->year = (uint16_t)(t.tm_year + 1900);
    out->month = (uint8_t)(t.tm_mon + 1);
    out->day = (uint8_t)t.tm_mday;
    out->hour = (uint8_t)t.tm_hour;
    out->minute = (uint8_t)t.tm_min;
    out->second = (uint8_t)t.tm_sec;
}

/* ---------------------------------------------------------------------- */
/* Logging helpers                                                         */
/* ---------------------------------------------------------------------- */

static void log_frame(const char *dir, const uint8_t *data, size_t len)
{
#if CONFIG_T7_LOG_FRAME_HEX
    char line[2 * 200 + 1];
    size_t n = len < 200 ? len : 200;
    for (size_t i = 0; i < n; i++) {
        snprintf(line + 2 * i, 3, "%02X", data[i]);
    }
    ESP_LOGI(TAG, "%s %s%s", dir, line, len > n ? "..." : "");
#else
    (void)dir; (void)data; (void)len;
#endif
}

static void log_events(uint8_t events)
{
    for (uint8_t bit = 1; bit != 0 && bit <= YAM_EV_CALL_REJECT_END; bit <<= 1) {
        if (events & bit) {
            ESP_LOGI(TAG, "button: %s", yam_event_name(bit));
        }
    }
}

/* ---------------------------------------------------------------------- */
/* CCU link callbacks (NimBLE host task)                                   */
/* ---------------------------------------------------------------------- */

static void on_session_active(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_counters, 0, sizeof s_counters);
    yam_ctrl_reset(&s_ctrl);
    s_ctrl_pending = false;
    yam_sched_start(&s_sched, uptime_ms());
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "scheduler active: 0x60/0x61 every 1 s, 0x62 every 2 s (battery %u%%)",
             s_state.battery_percent);
}

static void on_session_ended(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    yam_sched_stop(&s_sched);
    yam_ctrl_reset(&s_ctrl);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "scheduler stopped");
}

static const char *family_name(uint8_t id)
{
    switch (id) {
    case 0x6A: return "identity";
    case 0x59: return "startup_page";
    case 0x6B: return "control";
    case 0x6C: return "vehicle";
    default:   return "other";
    }
}

static void on_rx_chunk(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    ESP_LOGD(TAG, "RX %s chunk (%u bytes)", family_name(data[0]), (unsigned)len);
    log_frame("RX", data, len);
    if (data[0] != 0x6B && !s_ctrl_pending) {
        return;   /* only 0x6B carries handlebar controls */
    }
    uint8_t all_events = 0;
    int frames = 0;
    bool error = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!yam_ctrl_append(&s_ctrl, data, len)) {
        error = true;
    } else {
        for (;;) {
            uint8_t ev = 0;
            yam_ctrl_result_t r = yam_ctrl_next(&s_ctrl, &ev);
            if (r == YAM_CTRL_FRAME) {
                frames++;
                all_events |= ev;
            } else {
                error = (r == YAM_CTRL_ERROR);
                break;
            }
        }
    }
    s_ctrl_pending = !error && frames == 0;
    bridge_control_hook_t hook = s_control_hook;
    xSemaphoreGive(s_lock);

    if (error) {
        ESP_LOGW(TAG, "control frame parse failed; buffer reset");
    }
    if (all_events) {
        log_events(all_events);
        if (all_events & YAM_EV_VOLUME_DOWN) {
            int64_t t = uptime_ms();
            s_vol_down_times[s_vol_down_idx] = t;
            s_vol_down_idx = (s_vol_down_idx + 1) % COMBO_PRESSES;
            int64_t oldest = s_vol_down_times[s_vol_down_idx];   /* 5th press back */
            if (oldest != 0 && t - oldest <= COMBO_WINDOW_MS) {
                memset(s_vol_down_times, 0, sizeof s_vol_down_times);
                ESP_LOGI(TAG, "5x volume down: starting update Wi-Fi");
                ota_update_start();
            }
        }
        if (hook) {
            hook(all_events);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Bridge task: link supervision + periodic frames                         */
/* ---------------------------------------------------------------------- */

static void bridge_task(void *arg)
{
    static uint8_t f60[YAM_LEN_60], f61[YAM_LEN_61], f62[YAM_LEN_62];
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        int64_t now = uptime_ms();
        ccu_link_tick((int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        iphone_link_tick(now);
        ota_update_tick(now);
        learn_mode_tick(now);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint8_t due = yam_sched_due(&s_sched, now);
        if (due & YAM_DUE_60) {
            yam_build_60(f60, &s_state, &s_counters);
        }
        if (due & YAM_DUE_61) {
            yam_build_61(f61, &s_state, &s_counters);
        }
        if (due & YAM_DUE_62) {
            yam_time_t t;
            clock_now(&t);
            /* Build from a copy so the real media state stays untouched. */
            static yam_state_t shown;
            shown = s_state;
            if (s_status_on) {
                yam_state_set_media(&shown, s_status_artist, s_status_album, s_status_title,
                                    s_state.playing);
            }
            /* The CCU ignores empty text fields and keeps showing the old text
             * (seen on the bike: "UPDATE MODE" stayed after an update). Send a
             * blank space instead so every field is always overwritten. */
            if (shown.artist[0] == '\0') strcpy(shown.artist, " ");
            if (shown.album[0] == '\0')  strcpy(shown.album, " ");
            if (shown.title[0] == '\0')  strcpy(shown.title, " ");
            yam_build_62(f62, &shown, &s_counters, &t);
            memset(s_state.slots, 0, sizeof s_state.slots);   /* pulses were sent */
        }
        xSemaphoreGive(s_lock);

        if (due & YAM_DUE_60) {
            ccu_link_send(f60, sizeof f60);
            log_frame("TX", f60, sizeof f60);
        }
        if (due & YAM_DUE_61) {
            ccu_link_send(f61, sizeof f61);
            log_frame("TX", f61, sizeof f61);
        }
        if (due & YAM_DUE_62) {
            ccu_link_send(f62, sizeof f62);
            log_frame("TX", f62, sizeof f62);
        }
    }
}

static void apply_phone_defaults_locked(void)
{
    /* Without the iPhone, report a fixed battery level: the TFT only enables
     * its phone/media display when the level is non-zero. */
    yam_state_set_indicators(&s_state, CONFIG_T7_PLACEHOLDER_BATTERY, 0, 0);
#if CONFIG_T7_TEST_MEDIA
    yam_state_set_media(&s_state, "T7 BRIDGE", "ESP32", "HELLO FROM ESP", true);
#else
    yam_state_set_media(&s_state, "", "", "", false);
#endif
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

void bridge_set_media(const char *artist, const char *album, const char *title, bool playing)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    yam_state_set_media(&s_state, artist, album, title, playing);
    xSemaphoreGive(s_lock);
}

void bridge_set_battery(uint8_t percent)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    yam_state_set_indicators(&s_state, percent, s_state.mobile_signal, s_state.gnss_signal);
    xSemaphoreGive(s_lock);
}

void bridge_set_time_ymdhms(uint16_t year, uint8_t month, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clock_set(year, month, day, hour, minute, second);
    xSemaphoreGive(s_lock);
}

void bridge_phone_disconnected(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    apply_phone_defaults_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "iPhone gone: placeholder battery %d%%%s", CONFIG_T7_PLACEHOLDER_BATTERY,
             CONFIG_T7_TEST_MEDIA ? ", test track restored" : "");
}

void bridge_call_ringing(const char *number, const char *name)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    yam_state_set_call_ringing(&s_state, number, name);
    yam_sched_request_immediate_62(&s_sched);
    xSemaphoreGive(s_lock);
}

void bridge_call_active(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    yam_state_set_call_active(&s_state);
    yam_sched_request_immediate_62(&s_sched);
    xSemaphoreGive(s_lock);
}

void bridge_call_ended(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (yam_state_end_call(&s_state) != YAM_CALL_END_NONE) {
        yam_sched_request_immediate_62(&s_sched);
    }
    xSemaphoreGive(s_lock);
}

void bridge_set_status_text(const char *artist, const char *album, const char *title)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status_on = (title != NULL);
    if (s_status_on) {
        snprintf(s_status_artist, sizeof s_status_artist, "%s", artist ? artist : "");
        snprintf(s_status_album, sizeof s_status_album, "%s", album ? album : "");
        snprintf(s_status_title, sizeof s_status_title, "%s", title);
    }
    xSemaphoreGive(s_lock);
}

void bridge_set_control_hook(bridge_control_hook_t hook)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_control_hook = hook;
    xSemaphoreGive(s_lock);
}

void bridge_on_ble_sync(void)
{
    ccu_link_on_sync();
    iphone_link_on_sync();
    learn_mode_on_sync();
}

void bridge_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    yam_state_init(&s_state);
    clock_init();

    apply_phone_defaults_locked();
#if CONFIG_T7_TEST_MEDIA
    ESP_LOGI(TAG, "until the iPhone connects: battery %d%%, test track \"HELLO FROM ESP\"",
             CONFIG_T7_PLACEHOLDER_BATTERY);
#endif
    iphone_link_init();
    s_control_hook = iphone_link_handle_controls;

    static const ccu_link_callbacks_t cb = {
        .session_active = on_session_active,
        .session_ended = on_session_ended,
        .rx_chunk = on_rx_chunk,
    };
    ccu_link_init(&cb);
    xTaskCreate(bridge_task, "bridge", 6144, NULL, 5, NULL);
}
