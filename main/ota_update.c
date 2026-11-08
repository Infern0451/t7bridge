/*
 * Firmware updates over Wi-Fi. See ota_update.h.
 */
#include "ota_update.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "creds.h"
#include "learn_mode.h"

static const char *TAG = "update";

#define UPDATE_SSID         "t7bridge-update"
#define CONFIRM_AFTER_MS    60000     /* new firmware counts as good after 60 s */

static volatile bool s_active;
static int64_t s_stop_at_ms;
static bool s_pending_verify;
static bool s_net_ready;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_server;
static volatile bool s_stop_requested;
static bool s_held;

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

const char *ota_firmware_version(void)
{
    return esp_app_get_description()->version;
}

bool ota_update_active(void)
{
    return s_active;
}

/* ---------------------------------------------------------------------- */
/* Web page                                                                 */
/* ---------------------------------------------------------------------- */

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>t7bridge setup</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;max-width:32em;margin:2em auto;"
    "padding:0 1em;background:#111;color:#eee}h1{font-size:1.4em}"
    "button,input{font-size:1.1em;margin:.5em 0}button{padding:.5em 1.2em}"
    "#bar{height:1em;background:#333;border-radius:.5em;overflow:hidden}"
    "#fill{height:100%;width:0;background:#4c9}#msg{margin-top:1em;min-height:3em}"
    "td{padding:.2em .8em .2em 0}input[type=text]{width:9em}.warn{color:#fc6}"
    "</style></head><body>";

static const char PAGE_TAIL[] =
    "<h1>Firmware update</h1><p>Choose the new <b>t7bridge.bin</b> file, then tap Upload. "
    "Keep this page open until it says the bridge is restarting.</p>"
    "<input type='file' id='f' accept='.bin'><br>"
    "<button id='go'>Upload</button>"
    "<div id='bar'><div id='fill'></div></div><div id='msg'></div>"
    "<script>"
    "document.getElementById('go').onclick=function(){"
    "var f=document.getElementById('f').files[0],m=document.getElementById('msg');"
    "if(!f){m.textContent='Choose a file first.';return;}"
    "var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=function(e){if(e.lengthComputable)"
    "document.getElementById('fill').style.width=(100*e.loaded/e.total)+'%';};"
    "x.onload=function(){m.textContent=x.responseText;};"
    "x.onerror=function(){m.textContent='Upload failed. Check you are still on the "
    "t7bridge-update Wi-Fi and try again.';};"
    "m.textContent='Uploading...';x.send(f);};"
    "</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    char info[1800];
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *state;
    if (!creds_have_ccu_id()) {
        state = "<p class='warn'>Looking for your bike: turn the ignition on near the bridge, "
                "or type the S/N from the CCU label below.</p>";
    } else if (!creds_have_code()) {
        state = "<p class='warn'><b>Learn mode.</b> Turn the ignition off (or take the bridge "
                "away from the bike), then pair the bike in the Yamaha Y-Connect app. The app "
                "will connect to this bridge and hand over the pairing code. Reload this page "
                "to check.</p>";
    } else {
        state = "<p>Ready. The bridge pairs with your bike by itself.</p>";
    }
    snprintf(info, sizeof info,
             "<h1>t7bridge</h1><p>Firmware <b>%s</b> (slot %s)</p>%s"
             "<table><tr><td>CCU ID</td><td><b>%s</b></td></tr>"
             "<tr><td>Pairing code</td><td><b>%s</b></td></tr>"
             "<tr><td>Bridge token</td><td>%.4s...</td></tr></table>"
             "<h1>Set manually</h1>"
             "<form method='post' action='/setup'>"
             "<p>CCU ID (14 characters, S/N on the CCU label):<br>"
             "<input type='text' name='ccu' maxlength='14' value='%s'></p>"
             "<p>Pairing code (6 characters, if you already know it):<br>"
             "<input type='text' name='code' maxlength='6'></p>"
             "<button>Save</button></form>"
             "<h1>Start over</h1><form method='post' action='/forget' "
             "onsubmit=\"return confirm('Forget the bike and all Bluetooth pairings?')\">"
             "<p>Forgets the bike and every Bluetooth pairing (the iPhone too), then restarts "
             "in learn mode. Same as holding BOOT for 5 seconds.</p>"
             "<button>Forget bike</button></form>",
             ota_firmware_version(), running ? running->label : "?", state,
             creds_have_ccu_id() ? creds_ccu_id() : "not found yet",
             creds_have_code() ? creds_pairing_code() : "not known yet",
             creds_token(), creds_ccu_id());
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, info);
    if (!creds_have_code()) {
        static char evbuf[24 * 100];
        learn_mode_events(evbuf, sizeof evbuf);
        httpd_resp_sendstr_chunk(req, "<h1>Learn mode activity</h1><pre style='white-space:pre-wrap;"
                                      "font-size:.85em'>");
        httpd_resp_sendstr_chunk(req, evbuf[0] ? evbuf : "(nothing yet)\n");
        httpd_resp_sendstr_chunk(req, "</pre><p>Reload this page after each try. If Y-Connect "
                                      "fails, send a screenshot of this list.</p>");
        httpd_resp_sendstr_chunk(req, learn_mode_pair_request()
            ? "<form method='post' action='/pairopt'><input type='hidden' name='on' value='0'>"
              "<p>Ask the phone to pair over Bluetooth: <b>ON</b></p><button>Turn off</button></form>"
            : "<form method='post' action='/pairopt'><input type='hidden' name='on' value='1'>"
              "<p>Ask the phone to pair over Bluetooth: <b>off</b>. If Y-Connect says it failed "
              "to pair, turn this on and try again (tap Pair if iOS asks).</p>"
              "<button>Turn on</button></form>");
    }
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));   /* let the browser receive the reply */
    esp_restart();
}

static void reset_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    learn_mode_factory_reset();
}

static esp_err_t reply_page(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>t7bridge</h1><p>");
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "</p><p><a href='/' style='color:#4c9'>Back</a></p></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t setup_post(httpd_req_t *req)
{
    char body[200] = {0};
    int n = req->content_len < sizeof body - 1 ? (int)req->content_len : (int)sizeof body - 1;
    int got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, body + got, n - got);
        if (r <= 0) {
            return reply_page(req, "Nothing received. Try again.");
        }
        got += r;
    }
    char ccu[20] = "", code[12] = "";
    httpd_query_key_value(body, "ccu", ccu, sizeof ccu);
    httpd_query_key_value(body, "code", code, sizeof code);
    char msg[200];
    msg[0] = 0;
    if (ccu[0]) {
        for (char *c = ccu; *c; c++) {
            if (*c >= 'a' && *c <= 'z') {
                *c -= 32;   /* the S/N is upper-case hex */
            }
        }
        if (!creds_set_ccu_id(ccu)) {
            return reply_page(req, "That CCU ID is not valid: it must be exactly 14 letters/digits.");
        }
        strcat(msg, "CCU ID saved. ");
    }
    if (code[0]) {
        if (!creds_valid_code(code)) {
            return reply_page(req, "That pairing code is not valid: it must be exactly 6 characters.");
        }
        if (!creds_have_ccu_id()) {
            return reply_page(req, "Enter the CCU ID too (or let the bridge find the bike first).");
        }
        creds_set_pairing_code(code);
        strcat(msg, "Pairing code saved: the bridge will now pair with your bike.");
    }
    return reply_page(req, msg[0] ? msg : "Nothing to save.");
}

static esp_err_t pairopt_post(httpd_req_t *req)
{
    char body[32] = {0};
    int n = req->content_len < sizeof body - 1 ? (int)req->content_len : (int)sizeof body - 1;
    if (n > 0 && httpd_req_recv(req, body, n) <= 0) {
        return reply_page(req, "Nothing received. Try again.");
    }
    char on[4] = "";
    httpd_query_key_value(body, "on", on, sizeof on);
    learn_mode_set_pair_request(on[0] == '1');
    return reply_page(req, on[0] == '1'
        ? "The bridge will now ask the phone to pair when Y-Connect connects. Try pairing in "
          "Y-Connect again and tap Pair if iOS asks."
        : "Pairing request turned off.");
}

static esp_err_t forget_post(httpd_req_t *req)
{
    xTaskCreate(reset_task, "reset", 3072, NULL, 5, NULL);
    return reply_page(req, "Forgetting the bike and restarting in learn mode. Rejoin this "
                           "Wi-Fi in a few seconds.");
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, bool begun, const char *why)
{
    if (begun) {
        esp_ota_abort(h);
    }
    ESP_LOGW(TAG, "update rejected: %s", why);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    char msg[160];
    snprintf(msg, sizeof msg, "Update failed: %s. The current firmware is unchanged.", why);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return fail(req, 0, false, "no spare firmware slot (flash once over USB first)");
    }
    if (req->content_len == 0 || req->content_len > target->size) {
        return fail(req, 0, false, "file is empty or too large");
    }
    ESP_LOGI(TAG, "receiving %u bytes into slot %s", (unsigned)req->content_len, target->label);
    s_stop_at_ms = now_ms() + 10 * 60 * 1000;   /* don't time out mid-upload */

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        return fail(req, 0, false, "could not prepare the firmware slot");
    }
    static char buf[4096];
    size_t remaining = req->content_len;
    size_t received = 0;
    bool checked_header = false;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < sizeof buf ? remaining : sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return fail(req, h, true, "connection lost during upload");
        }
        if (!checked_header) {
            /* Collect the start of the file: the app description sits right
             * after the image + first segment headers. Refuse anything that
             * is not t7bridge firmware before writing a single byte. */
            static uint8_t head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                                sizeof(esp_app_desc_t)];
            size_t take = sizeof head - received;
            if (take > (size_t)n) {
                take = (size_t)n;
            }
            memcpy(head + received, buf, take);
            if (received + take == sizeof head) {
                const esp_app_desc_t *d = (const esp_app_desc_t *)(head + sizeof(esp_image_header_t) +
                                                                   sizeof(esp_image_segment_header_t));
                if (d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                    strncmp(d->project_name, esp_app_get_description()->project_name,
                            sizeof d->project_name) != 0) {
                    return fail(req, h, true, "this is not a t7bridge firmware file");
                }
                ESP_LOGI(TAG, "new firmware version %s", d->version);
                checked_header = true;
            }
        }
        if (esp_ota_write(h, buf, (size_t)n) != ESP_OK) {
            return fail(req, h, true, "writing to flash failed");
        }
        received += (size_t)n;
        remaining -= (size_t)n;
    }
    if (!checked_header) {
        return fail(req, h, true, "file too small to be firmware");
    }
    if (esp_ota_end(h) != ESP_OK) {   /* verifies the image checksum/hash */
        return fail(req, 0, false, "the file is damaged or incomplete");
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        return fail(req, 0, false, "could not select the new firmware");
    }
    ESP_LOGI(TAG, "update written to %s; restarting", target->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Update installed. The bridge is restarting now; it will "
                            "reconnect to the bike and your iPhone by itself. You can close this page.");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/* Wi-Fi access point                                                       */
/* ---------------------------------------------------------------------- */

static void stop_network(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_active = false;
    bridge_set_status_text(NULL, NULL, NULL);
    ESP_LOGI(TAG, "update Wi-Fi off");
}

void ota_update_start(void)
{
    if (s_active) {
        s_stop_at_ms = now_ms() + (int64_t)CONFIG_T7_UPDATE_WINDOW_MIN * 60 * 1000;
        return;
    }
    if (strlen(CONFIG_T7_UPDATE_WIFI_PASSWORD) < 8) {
        ESP_LOGE(TAG, "update Wi-Fi password must be at least 8 characters (menuconfig)");
        return;
    }
    if (!s_net_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop failed: %d", e);
            return;
        }
        s_ap_netif = esp_netif_create_default_wifi_ap();
        s_net_ready = true;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed");
        return;
    }
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, UPDATE_SSID, strlen(UPDATE_SSID));
    ap.ap.ssid_len = strlen(UPDATE_SSID);
    strncpy((char *)ap.ap.password, CONFIG_T7_UPDATE_WIFI_PASSWORD, sizeof ap.ap.password - 1);
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed");
        esp_wifi_deinit();
        return;
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.recv_wait_timeout = 15;
    hc.stack_size = 6144;
    if (httpd_start(&s_server, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        esp_wifi_stop();
        esp_wifi_deinit();
        return;
    }
    static const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    static const httpd_uri_t upd = {.uri = "/update", .method = HTTP_POST, .handler = update_post};
    httpd_register_uri_handler(s_server, &page);
    httpd_register_uri_handler(s_server, &upd);
    static const httpd_uri_t setup = {.uri = "/setup", .method = HTTP_POST, .handler = setup_post};
    static const httpd_uri_t forget = {.uri = "/forget", .method = HTTP_POST, .handler = forget_post};
    httpd_register_uri_handler(s_server, &setup);
    httpd_register_uri_handler(s_server, &forget);
    static const httpd_uri_t pairopt = {.uri = "/pairopt", .method = HTTP_POST, .handler = pairopt_post};
    httpd_register_uri_handler(s_server, &pairopt);

    s_active = true;
    s_stop_at_ms = now_ms() + (int64_t)CONFIG_T7_UPDATE_WINDOW_MIN * 60 * 1000;
    if (!s_held) {
        bridge_set_status_text(UPDATE_SSID, "192.168.4.1", "UPDATE MODE");
    }
    ESP_LOGI(TAG, "update Wi-Fi ON: join \"%s\", open http://192.168.4.1 (%d min)",
             UPDATE_SSID, CONFIG_T7_UPDATE_WINDOW_MIN);
}

/* ---------------------------------------------------------------------- */
/* Startup check and periodic work                                          */
/* ---------------------------------------------------------------------- */

void ota_update_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGW(TAG, "running new firmware %s from %s; it will be confirmed after 60 s",
                 ota_firmware_version(), running->label);
    } else {
        ESP_LOGI(TAG, "firmware %s (slot %s)", ota_firmware_version(),
                 running ? running->label : "?");
    }
#if CONFIG_T7_UPDATE_AT_STARTUP_MIN > 0
    ota_update_start();
    s_stop_at_ms = now_ms() + (int64_t)CONFIG_T7_UPDATE_AT_STARTUP_MIN * 60 * 1000;
#endif
}

void ota_update_hold(bool hold)
{
    if (hold) {
        if (!s_held) {
            s_held = true;
            ESP_LOGI(TAG, "setup Wi-Fi stays on until the bike's pairing code is known");
        }
        if (!s_active) {
            ota_update_start();
        }
        s_stop_at_ms = now_ms() + (int64_t)CONFIG_T7_UPDATE_WINDOW_MIN * 60 * 1000;
    } else {
        s_held = false;
    }
}

void ota_update_tick(int64_t now)
{
    (void)now;
    int64_t t = now_ms();
    if (s_pending_verify && t >= CONFIRM_AFTER_MS) {
        s_pending_verify = false;
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "new firmware confirmed as good");
        }
    }
    if (s_active && t >= s_stop_at_ms) {
        stop_network();
    }
}
