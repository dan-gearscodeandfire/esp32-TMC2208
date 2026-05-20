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

// ---------------------------------------------------------------- HTML page
// Single-quoted HTML attributes + single-quoted JS strings (no apostrophes in
// hint text) so the whole page is one un-escaped C string. All dependency
// logic (linked fields, grey-out, hints) runs client-side in recompute().

static const char PAGE[] =
"<!doctype html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>shopsmartfan</title>"
"<style>"
"body{font-family:sans-serif;max-width:600px;margin:1em auto;padding:0 1em;color:#222}"
"h2,h3{margin:.5em 0 .2em}legend{font-weight:bold}"
".row{display:flex;align-items:center;gap:.5em;margin:.35em 0}"
".row>label:first-child{flex:0 0 11em}"
"input,select{font-size:1em;padding:.35em}input[type=number]{width:7em}"
"input:disabled,select:disabled{background:#e9e9f2;color:#999}"
"#st{background:#eee;padding:.6em;border-radius:6px;margin:.4em 0}"
".hint{color:#888;font-size:.85em}"
"button{font-size:1em;padding:.5em 1em;margin:.3em .2em 0 0}"
"fieldset{border:1px solid #ccc;border-radius:6px;margin:.7em 0;padding:.4em .8em}"
"</style></head><body>"
"<h2>shopsmartfan</h2>"
"<div id='st'>status...</div>"

"<fieldset><legend>Motor</legend>"
"<div class='hint'>12 V &middot; rated 1.5 A &middot; ~40 N-cm holding (est.) &middot; 2.3 ohm/phase &middot; NEMA 17, 39 mm</div>"
"</fieldset>"

"<fieldset><legend>Drive parameters</legend>"
"<div class='row'><label>Microsteps</label>"
"<select id='usteps' title='Sets CHOPCONF.MRES. One rev = 200 x microsteps pulses, so changing this rescales step-rate and acceleration.'>"
"<option>1</option><option>2</option><option>4</option><option>8</option><option selected>16</option></select></div>"

"<div class='row'><label><input type='checkbox' id='maxT'> Max torque</label>"
"<span class='hint'>pins current to rated 1.5 A and locks the field</span></div>"
"<div class='row'><label>Run current (mA)</label>"
"<input type='number' id='cur' value='1500' min='0' max='1770' "
"title='RMS run current. Torque is ~proportional to current up to rated 1.5 A. Hardware ceiling ~1770 mA (0.11 ohm sense resistor).'></div>"
"<div class='row'><label>Holding torque</label>"
"<input type='number' id='tq' disabled title='Derived: ~40 N-cm at rated 1.5 A, scaled by current / 1500.'>"
"<span class='hint'>N-cm (derived from current)</span></div>"

"<div class='row'><label>Speed input as</label>"
"<select id='spDrv' title='Pick which field you type. The other is computed from it and microsteps.'>"
"<option value='rps'>rev/s</option><option value='sps'>steps/s</option></select></div>"
"<div class='row'><label>Speed (rev/s)</label>"
"<input type='number' id='rps' step='0.05' value='1.25' title='Revolutions per second. step-rate = rev/s x 200 x microsteps.'></div>"
"<div class='row'><label>Step rate (steps/s)</label>"
"<input type='number' id='sps' title='STEP pulses per second = rev/s x 200 x microsteps.'></div>"

"<div class='row'><label>Accel input as</label>"
"<select id='acDrv' title='Pick which acceleration field you type; the other is computed from it and microsteps.'>"
"<option value='rps'>rev/s^2</option><option value='sps'>steps/s^2</option></select></div>"
"<div class='row'><label>Accel (rev/s^2)</label>"
"<input type='number' id='arps' step='0.1' value='2.5' title='Used by the move ramp (full action builder lands in Phase 3).'></div>"
"<div class='row'><label>Accel (steps/s^2)</label>"
"<input type='number' id='asps' title='= rev/s^2 x 200 x microsteps.'></div>"

"<div class='row'><label>Chopper</label>"
"<select id='chop' title='SpreadCycle = more torque under load. StealthChop = quieter, softer.'>"
"<option value='spread' selected>SpreadCycle</option><option value='stealth'>StealthChop</option></select></div>"

"<button onclick='applyCfg()'>Apply to driver</button>"
"<span class='hint'>greyed fields are computed from others &mdash; hover any field for its formula</span>"
"</fieldset>"

"<fieldset><legend>Move</legend>"
"<div class='row'><label>Distance</label>"
"<input type='number' id='dist' step='0.05' value='2.75'>"
"<select id='unit'><option value='rev'>rev</option><option value='deg'>deg</option><option value='rad'>rad</option></select></div>"
"<button onclick='go(1)'>Move CW</button><button onclick='go(-1)'>Move CCW</button>"
"<div class='hint'>uses the speed/microsteps applied above</div>"
"</fieldset>"

"<script>"
"const FULL=200;"
"function us(){return parseInt(document.getElementById('usteps').value);}"
"function n(id){return parseFloat(document.getElementById(id).value)||0;}"
"function setv(id,v){document.getElementById(id).value=Math.round(v*1000)/1000;}"
"function lock(id,on){let e=document.getElementById(id);e.disabled=on;}"
"function recompute(){"
" let m=us();"
" let sd=document.getElementById('spDrv').value;"
" lock('rps',sd!='rps');lock('sps',sd!='sps');"
" if(sd=='rps')setv('sps',n('rps')*FULL*m);else setv('rps',n('sps')/(FULL*m));"
" let ad=document.getElementById('acDrv').value;"
" lock('arps',ad!='rps');lock('asps',ad!='sps');"
" if(ad=='rps')setv('asps',n('arps')*FULL*m);else setv('arps',n('asps')/(FULL*m));"
" let mt=document.getElementById('maxT').checked;"
" lock('cur',mt);if(mt)document.getElementById('cur').value=1500;"
" setv('tq',40*n('cur')/1500);"
"}"
"['usteps','spDrv','acDrv','maxT','rps','sps','arps','asps','cur'].forEach(function(id){"
" let e=document.getElementById(id);e.addEventListener('input',recompute);e.addEventListener('change',recompute);});"
"async function st(){try{let j=await(await fetch('/api/status')).json();"
"document.getElementById('st').textContent='position (pulses): '+j.position_pulses;}catch(e){}}"
"async function applyCfg(){let q='usteps='+us()+'&current='+Math.round(n('cur'))"
"+'&cruise_sps='+Math.round(n('sps'))+'&chop='+document.getElementById('chop').value;"
"await fetch('/api/config?'+q,{method:'POST'});document.getElementById('st').textContent='config applied';setTimeout(st,400);}"
"async function go(sign){let d=n('dist'),u=document.getElementById('unit').value;"
"let rev=u=='deg'?d/360:(u=='rad'?d/(2*Math.PI):d);rev*=sign;"
"document.getElementById('st').textContent='moving '+rev.toFixed(3)+' rev...';"
"await fetch('/api/move?revs='+rev,{method:'POST'});st();}"
"recompute();st();setInterval(st,3000);"
"</script></body></html>";

// ---------------------------------------------------------------- handlers

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

// POST /api/config?usteps=&current=&cruise_sps=&chop=spread|stealth
// Rebuilds a motor_config_t from defaults + posted params and pushes it.
static esp_err_t config_post(httpd_req_t *req)
{
    motor_config_t cfg = MOTOR_CONFIG_DEFAULT;
    char q[192], v[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "usteps", v, sizeof(v)) == ESP_OK) cfg.microsteps = (uint8_t)atoi(v);
        if (httpd_query_key_value(q, "current", v, sizeof(v)) == ESP_OK) cfg.run_current_ma = (uint16_t)atoi(v);
        if (httpd_query_key_value(q, "cruise_sps", v, sizeof(v)) == ESP_OK) cfg.cruise_sps = (uint16_t)atoi(v);
        if (httpd_query_key_value(q, "start_sps", v, sizeof(v)) == ESP_OK) cfg.start_sps = (uint16_t)atoi(v);
        if (httpd_query_key_value(q, "chop", v, sizeof(v)) == ESP_OK)
            cfg.chop = (strcmp(v, "stealth") == 0) ? MOTOR_CHOP_STEALTH : MOTOR_CHOP_SPREAD;
    }
    motor_configure(&cfg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t move_post(httpd_req_t *req)
{
    float revs = 0.0f;
    char q[64], v[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "revs", v, sizeof(v)) == ESP_OK) {
        revs = strtof(v, NULL);
    }
    motor_move_revs(revs);   // Phase 1/2: blocks the handler during the move

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

    httpd_uri_t root   = { .uri = "/",            .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t status = { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t config = { .uri = "/api/config",  .method = HTTP_POST, .handler = config_post };
    httpd_uri_t move   = { .uri = "/api/move",    .method = HTTP_POST, .handler = move_post };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &config);
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
