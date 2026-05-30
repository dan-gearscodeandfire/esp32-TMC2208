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
    // Capture the netif so we can set our hostname before DHCP starts -- this
    // is what the router's client list (and any mDNS scanner) will see.
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, "tmc_stepper_tester"));

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

static const char PAGE[] =
"<!doctype html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TMC2208/2209 Tester</title>"
"<style>"
"body{font-family:sans-serif;max-width:620px;margin:1em auto;padding:0 1em;color:#222}"
"h2{margin:.3em 0}legend{font-weight:bold}"
".row{display:flex;align-items:center;gap:.5em;margin:.35em 0;flex-wrap:wrap}"
".row>label:first-child{flex:0 0 11em}"
"input,select{font-size:1em;padding:.35em}input[type=number]{width:7em}"
"input:disabled,select:disabled{background:#e9e9f2;color:#999}"
"#st{background:#eee;padding:.6em;border-radius:6px;margin:.4em 0}"
"#prev{background:#eef;padding:.5em;border-radius:6px;margin:.4em 0}"
".hint{color:#888;font-size:.85em}"
"button{font-size:1em;padding:.5em 1em;margin:.3em .2em 0 0}"
"fieldset{border:1px solid #ccc;border-radius:6px;margin:.7em 0;padding:.4em .8em}"
"</style></head><body>"
"<h2>TMC2208/2209 Tester</h2>"
"<div id='st'>status...</div>"

"<fieldset><legend>Motor</legend>"
"<div class='hint'>12 V &middot; rated 1.5 A &middot; ~40 N-cm holding (est.) &middot; 2.3 ohm/phase &middot; NEMA 17, 39 mm</div>"
"</fieldset>"

"<fieldset><legend>Drive parameters</legend>"
"<div class='row'><label>Driver</label>"
"<select id='drv' title='Pick the driver board. TMC2208/2209 are configured over UART (current, microsteps, chopper all software-set). A4988/DRV8825 are STEP/DIR only -- set current with the board trimpot and microsteps with the MS jumpers.'>"
"<option value='tmc2208'>TMC2208 (UART)</option>"
"<option value='tmc2209'>TMC2209 (UART)</option>"
"<option value='a4988'>A4988 (STEP/DIR)</option>"
"<option value='drv8825'>DRV8825 (STEP/DIR)</option></select></div>"
"<div class='row'><label></label><span class='hint' id='drvHint'></span></div>"
"<div class='row'><label>Microsteps</label>"
"<select id='usteps' title='Sets CHOPCONF.MRES. One rev = 200 x microsteps pulses, so changing this rescales step-rate and acceleration.'>"
"<option>1</option><option>2</option><option>4</option><option>8</option><option selected>16</option></select></div>"
"<div class='row'><label><input type='checkbox' id='maxT'> Max torque</label>"
"<span class='hint'>pins current to rated 1.5 A</span></div>"
"<div class='row'><label>Run current (mA)</label>"
"<input type='number' id='cur' value='1500' min='0' max='1770' "
"title='RMS run current. Torque ~proportional to current up to rated 1.5 A. Hardware ceiling ~1770 mA.'></div>"
"<div class='row'><label>Holding torque</label>"
"<input type='number' id='tq' disabled title='Derived: ~40 N-cm at rated 1.5 A, scaled by current / 1500.'>"
"<span class='hint'>N-cm (derived)</span></div>"
"<div class='row'><label>Speed input as</label>"
"<select id='spDrv'><option value='rps'>rev/s</option><option value='sps'>steps/s</option></select></div>"
"<div class='row'><label>Speed (rev/s)</label>"
"<input type='number' id='rps' step='0.05' value='1.25' title='step-rate = rev/s x 200 x microsteps.'></div>"
"<div class='row'><label>Step rate (steps/s)</label>"
"<input type='number' id='sps' title='STEP pulses/s = rev/s x 200 x microsteps.'></div>"
"<div class='row'><label>Accel input as</label>"
"<select id='acDrv'><option value='rps'>rev/s^2</option><option value='sps'>steps/s^2</option></select></div>"
"<div class='row'><label>Accel (rev/s^2)</label>"
"<input type='number' id='arps' step='0.1' value='2.5' title='Used when Action acceleration = custom.'></div>"
"<div class='row'><label>Accel (steps/s^2)</label>"
"<input type='number' id='asps' title='= rev/s^2 x 200 x microsteps.'></div>"
"<div class='row'><label>Chopper</label>"
"<select id='chop'><option value='spread' selected>SpreadCycle</option><option value='stealth'>StealthChop</option></select></div>"
"<div class='row'><label>StallGuard threshold</label>"
"<input type='number' id='sgthrs' value='0' min='0' max='255' "
"title='TMC2209 only. 0 disables. Higher = more sensitive (stall declared at lighter load). Stall fires when SG_RESULT <= SGTHRS*2.'></div>"
"<div class='row'><label></label><span class='hint' id='sgHint'></span></div>"
"<span class='hint'>greyed = computed from other fields; hover for the formula</span>"
"</fieldset>"

"<fieldset><legend>Action</legend>"
"<div class='row'><label>Acceleration</label>"
"<label><input type='radio' name='acm' value='none'> none</label>"
"<label><input type='radio' name='acm' value='def' checked> default</label>"
"<label><input type='radio' name='acm' value='cus'> custom</label></div>"
"<div class='row'><label>Move type</label>"
"<select id='mtype' title='Relative: turn this far from here. To position: go to an absolute position from the zero you set.'>"
"<option value='rel'>relative distance</option><option value='abs'>to position</option></select></div>"
"<div class='row'><label id='dlbl'>Distance</label>"
"<input type='number' id='dist' step='0.05' value='2.75'>"
"<select id='unit'><option value='rev'>rev</option><option value='deg'>deg</option><option value='rad'>rad</option></select></div>"
"<div class='row' id='dirRow'><label>Direction</label>"
"<label><input type='radio' name='dir' value='1' checked> CW</label>"
"<label><input type='radio' name='dir' value='-1'> CCW</label></div>"
"<div id='prev'>preview...</div>"
"<button onclick='execAction()'>Execute</button>"
"<button onclick='zero()'>Set zero (closed)</button>"
"</fieldset>"

"<script>"
"const FULL=200;"
"function us(){return parseInt(document.getElementById('usteps').value);}"
"function n(id){return parseFloat(document.getElementById(id).value)||0;}"
"function setv(id,v){document.getElementById(id).value=Math.round(v*1000)/1000;}"
"function lock(id,on){document.getElementById(id).disabled=on;}"
"function rad(name){let e=document.querySelector('input[name='+name+']:checked');return e?e.value:null;}"
"function drv(){return document.getElementById('drv').value;}"
"function hasUart(){let d=drv();return d=='tmc2208'||d=='tmc2209';}"
// DRV8825 adds 1/32; everyone else tops out at 16. Rebuild the options on
// driver change, preserving the current pick where it still exists.
"function rebuildUsteps(){let s=document.getElementById('usteps'),cur=s.value;"
"let o=(drv()=='drv8825')?[1,2,4,8,16,32]:[1,2,4,8,16];s.innerHTML='';"
"o.forEach(function(v){let e=document.createElement('option');e.value=v;e.text=v;"
"if(''+v==cur)e.selected=true;s.appendChild(e);});if(!s.value)s.value=o[o.length-1];}"
"function distRev(){let d=n('dist'),u=document.getElementById('unit').value;"
"return u=='deg'?d/360:(u=='rad'?d/(2*Math.PI):d);}"
"function accel(){let m=rad('acm');if(m=='none')return Infinity;if(m=='cus')return n('asps');"
"return 5*FULL*us();}"  // default 5 rev/s^2
"function recompute(){"
" let m=us();"
// UART-only fields (current, max-torque, chopper) are greyed for STEP/DIR boards,
// where current is the trimpot's job and microsteps are jumper-set.
" let u=hasUart();"
" document.getElementById('maxT').disabled=!u;document.getElementById('chop').disabled=!u;"
" if(!u)document.getElementById('maxT').checked=false;"
" document.getElementById('drvHint').textContent=u"
"  ?'UART: current, microsteps and chopper are set in software.'"
"  :'STEP/DIR only: set current via the board trimpot, microsteps via the MS jumpers. Microsteps below just declares the jumper setting so the math matches.';"
// StallGuard is a TMC2209-only feature. Grey the field for everyone else and
// explain why; clear the value on non-2209 so a stale number doesn't get POSTed.
" let is2209=drv()=='tmc2209';"
" let sg=document.getElementById('sgthrs');sg.disabled=!is2209;if(!is2209)sg.value=0;"
" document.getElementById('sgHint').textContent=is2209"
"  ?'Sensorless stall: live SG_RESULT shows in status during motion. Tune threshold against it; values at rest or early ramp are noise. 0 = off.'"
"  :'StallGuard is a TMC2209-only feature.';"
" let sd=document.getElementById('spDrv').value;lock('rps',sd!='rps');lock('sps',sd!='sps');"
" if(sd=='rps')setv('sps',n('rps')*FULL*m);else setv('rps',n('sps')/(FULL*m));"
" let ad=document.getElementById('acDrv').value;lock('arps',ad!='rps');lock('asps',ad!='sps');"
" if(ad=='rps')setv('asps',n('arps')*FULL*m);else setv('arps',n('asps')/(FULL*m));"
" let mt=document.getElementById('maxT').checked;lock('cur',mt||!u);if(mt)document.getElementById('cur').value=1500;"
" setv('tq',40*n('cur')/1500);"
" let abs=document.getElementById('mtype').value=='abs';"
" document.getElementById('dlbl').textContent=abs?'Target position':'Distance';"
" document.getElementById('dirRow').style.display=abs?'none':'flex';"
" preview();"
"}"
"function preview(){"
" let m=us(),pulses=Math.abs(distRev())*FULL*m,v0=200,vc=n('sps'),a=accel();"
" let ramp=(a>0&&isFinite(a)&&vc>v0)?(vc*vc-v0*v0)/(2*a):0;if(ramp>pulses/2)ramp=pulses/2;"
" let peak=(ramp<pulses/2)?vc:Math.sqrt(v0*v0+2*a*(pulses/2));"
" let tr=(a>0&&isFinite(a))?2*(peak-v0)/a:0;"
" let cp=pulses-2*ramp,tc=cp>0?cp/vc:0,t=tr+tc;"
" document.getElementById('prev').textContent="
"  Math.round(pulses)+' pulses  |  ramp '+(ramp/(FULL*m)).toFixed(2)+' rev  |  peak '"
"  +(peak/(FULL*m)).toFixed(2)+' rev/s  |  ~'+t.toFixed(1)+' s';"
"}"
"['usteps','spDrv','acDrv','maxT','rps','sps','arps','asps','cur','dist','unit','mtype','sgthrs'].forEach(function(id){"
" let e=document.getElementById(id);e.addEventListener('input',recompute);e.addEventListener('change',recompute);});"
"document.getElementById('drv').addEventListener('change',function(){rebuildUsteps();recompute();});"
"document.querySelectorAll('input[name=acm]').forEach(function(e){e.addEventListener('change',preview);});"
"let drvSynced=false;"
"async function st(){try{let j=await(await fetch('/api/status')).json();"
"let s='position (pulses): '+j.position_pulses+'  |  driver: '+(j.driver||'?');"
// SG_RESULT only included on 2209 with SGTHRS > 0. Lower = higher load; stall flag is
// just (load <= sgthrs*2), evaluated on the chip side and reported by the firmware.
"if(j.sg_load!==undefined)s+='  |  load: '+j.sg_load+'/1023'+(j.sg_stalled?'  STALL':'');"
"document.getElementById('st').textContent=s;"
// Adopt the driver the firmware detected at boot, once, so the form starts correct.
"if(j.driver&&!drvSynced){drvSynced=true;document.getElementById('drv').value=j.driver;rebuildUsteps();recompute();}"
"}catch(e){}}"
"async function applyCfg(){let am=rad('acm');"
"let a=am=='none'?65535:(am=='cus'?Math.round(n('asps')):0);"
"let q='driver='+drv()+'&usteps='+us()+'&current='+Math.round(n('cur'))+'&cruise_sps='+Math.round(n('sps'))"
"+'&accel_sps2='+a+'&chop='+document.getElementById('chop').value"
"+'&sgthrs='+Math.round(n('sgthrs'));"
"await fetch('/api/config?'+q,{method:'POST'});}"
"async function execAction(){await applyCfg();let m=us();"
"if(document.getElementById('mtype').value=='abs'){"
" let tp=Math.round(distRev()*FULL*m);"
" document.getElementById('st').textContent='moving to '+tp+' pulses...';"
" await fetch('/api/moveto?pulses='+tp,{method:'POST'});}"
"else{let rev=distRev()*parseInt(rad('dir'));"
" document.getElementById('st').textContent='moving '+rev.toFixed(3)+' rev...';"
" await fetch('/api/move?revs='+rev,{method:'POST'});}st();}"
"async function zero(){await fetch('/api/zero',{method:'POST'});st();}"
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
    // motor_read_sg() returns ESP_ERR_NOT_SUPPORTED on any non-2209 driver or
    // when SG is disabled, in which case we omit sg_load/sg_stalled from the
    // payload and the web UI hides those fields.
    uint16_t sg_load = 0;
    bool sg_stalled = false;
    bool have_sg = (motor_read_sg(&sg_load, &sg_stalled) == ESP_OK);

    char buf[160];
    int n;
    if (have_sg) {
        n = snprintf(buf, sizeof(buf),
                     "{\"position_pulses\":%ld,\"driver\":\"%s\",\"sg_load\":%u,\"sg_stalled\":%s}",
                     (long)motor_position_pulses(),
                     motor_driver_token(motor_active_driver()),
                     (unsigned)sg_load, sg_stalled ? "true" : "false");
    } else {
        n = snprintf(buf, sizeof(buf), "{\"position_pulses\":%ld,\"driver\":\"%s\"}",
                     (long)motor_position_pulses(), motor_driver_token(motor_active_driver()));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t config_post(httpd_req_t *req)
{
    motor_config_t cfg = MOTOR_CONFIG_DEFAULT;
    cfg.driver = motor_active_driver();   // keep current driver if none supplied
    char q[224], v[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "driver",     v, sizeof(v)) == ESP_OK) cfg.driver         = motor_driver_from_token(v);
        if (httpd_query_key_value(q, "usteps",     v, sizeof(v)) == ESP_OK) cfg.microsteps     = (uint8_t)atoi(v);
        if (httpd_query_key_value(q, "current",    v, sizeof(v)) == ESP_OK) cfg.run_current_ma = (uint16_t)atoi(v);
        if (httpd_query_key_value(q, "cruise_sps", v, sizeof(v)) == ESP_OK) cfg.cruise_sps     = (uint16_t)atoi(v);
        if (httpd_query_key_value(q, "start_sps",  v, sizeof(v)) == ESP_OK) cfg.start_sps      = (uint16_t)atoi(v);
        if (httpd_query_key_value(q, "accel_sps2", v, sizeof(v)) == ESP_OK) cfg.accel_sps2     = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "chop",       v, sizeof(v)) == ESP_OK)
            cfg.chop = (strcmp(v, "stealth") == 0) ? MOTOR_CHOP_STEALTH : MOTOR_CHOP_SPREAD;
        if (httpd_query_key_value(q, "sgthrs",     v, sizeof(v)) == ESP_OK) cfg.sg_threshold   = (uint8_t)atoi(v);
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
    motor_move_revs(revs);
    httpd_resp_set_type(req, "application/json");
    char buf[112];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"position_pulses\":%ld}", (long)motor_position_pulses());
    return httpd_resp_send(req, buf, n);
}

static esp_err_t moveto_post(httpd_req_t *req)
{
    long target = 0;
    char q[64], v[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "pulses", v, sizeof(v)) == ESP_OK) {
        target = strtol(v, NULL, 10);
    }
    motor_move_to_pulses((int32_t)target);
    httpd_resp_set_type(req, "application/json");
    char buf[112];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"position_pulses\":%ld}", (long)motor_position_pulses());
    return httpd_resp_send(req, buf, n);
}

static esp_err_t zero_post(httpd_req_t *req)
{
    motor_zero();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"position_pulses\":0}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/move",   .method = HTTP_POST, .handler = move_post },
        { .uri = "/api/moveto", .method = HTTP_POST, .handler = moveto_post },
        { .uri = "/api/zero",   .method = HTTP_POST, .handler = zero_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

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
