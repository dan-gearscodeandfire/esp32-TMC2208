#include "web.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "wifi_secrets.h"
#include "motor.h"

static const char *TAG = "web";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     12

static EventGroupHandle_t s_wifi_eg;
static int s_retries;

// ---------------------------------------------------------------- WiFi (STA)

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "disconnected; retry %d/%d", ++s_retries, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected. Open  http://" IPSTR "  in a browser.", IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_sta_connect(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to SSID '%s'...", WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------- HTTP server

// Phase 1 page: status read-out + a test move. Single-quoted attributes so the
// whole thing is one clean C string (no escaping).
static const char PAGE[] =
"<!doctype html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>shopsmartfan</title>"
"<style>body{font-family:sans-serif;max-width:480px;margin:1em auto;padding:0 1em}"
"input,button{font-size:1em;padding:.4em;margin:.2em}"
"#st{background:#eee;padding:.6em;border-radius:6px;margin:.5em 0}</style>"
"</head><body>"
"<h2>shopsmartfan</h2>"
"<div id='st'>status...</div>"
"<h3>Test move (Phase 1)</h3>"
"<label>Revolutions: <input id='revs' type='number' step='0.05' value='2.75'></label><br>"
"<button onclick='mv(1)'>CW</button> <button onclick='mv(-1)'>CCW</button>"
"<script>"
"async function st(){try{let r=await fetch('/api/status');let j=await r.json();"
"document.getElementById('st').textContent='position (pulses): '+j.position_pulses;}catch(e){}}"
"async function mv(s){let v=s*parseFloat(document.getElementById('revs').value||0);"
"document.getElementById('st').textContent='moving '+v.toFixed(2)+' rev...';"
"await fetch('/api/move?revs='+v,{method:'POST'});st();}"
"st();setInterval(st,3000);"
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"position_pulses\":%ld}", (long)motor_position_pulses());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t move_post(httpd_req_t *req)
{
    float revs = 0.0f;
    char q[64], val[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "revs", val, sizeof(val)) == ESP_OK) {
        revs = strtof(val, NULL);
    }

    // Phase 1: the move blocks this handler for its duration (~seconds).
    // Fine for single-user bring-up; later moves go to a dedicated task.
    motor_move_revs(revs);

    char buf[112];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"moved_revs\":%.3f,\"position_pulses\":%ld}",
                     revs, (long)motor_position_pulses());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t start_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t root   = { .uri = "/",           .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t move   = { .uri = "/api/move",   .method = HTTP_POST, .handler = move_post };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &move);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t web_start(void)
{
    if (wifi_sta_connect() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        return ESP_FAIL;
    }
    return start_http();
}
