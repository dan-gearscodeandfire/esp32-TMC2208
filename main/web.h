#pragma once
#include "esp_err.h"

// Connect to WiFi (STA, credentials from wifi_secrets.h) and start the HTTP
// control server. Returns ESP_OK once connected and serving; the assigned IP
// is logged. On failure the firmware keeps running (motor just isn't reachable
// over the web).
esp_err_t web_start(void);
