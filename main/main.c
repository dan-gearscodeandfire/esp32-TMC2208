#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "tmc2208.h"
#include "motor.h"
#include "web.h"

#define LED_PIN  GPIO_NUM_2
#define UART_TX  GPIO_NUM_26
#define UART_RX  GPIO_NUM_27

static const char *TAG = "app";
static tmc2208_t s_driver;
static motor_config_t s_motor_cfg = MOTOR_CONFIG_DEFAULT;

static bool probe_uart(void)
{
    tmc2208_config_t cfg = {
        .uart_port  = UART_NUM_2,
        .tx_pin     = UART_TX,
        .rx_pin     = UART_RX,
        .baud       = 19200,
        .slave_addr = 0x00,
    };
    if (tmc2208_init(&s_driver, &cfg) != ESP_OK) return false;

    uint8_t ver = 0;
    for (uint8_t a = 0; a <= 3; a++) {
        s_driver.cfg.slave_addr = a;
        if (tmc2208_get_version(&s_driver, &ver) == ESP_OK && ver == 0x20) {
            ESP_LOGI(TAG, "TMC2208 UART OK (slave 0x%02x, VERSION 0x%02x)", a, ver);
            return true;
        }
    }
    return false;
}

void app_main(void)
{
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    if (!probe_uart()) {
        ESP_LOGE(TAG, "TMC2208 not responding -- is VM (12 V) connected?");
    } else {
        motor_init(&s_driver, &s_motor_cfg);
        if (web_start() == ESP_OK) {
            ESP_LOGI(TAG, "Web control up -- open the printed IP in a browser.");
        } else {
            ESP_LOGW(TAG, "WiFi/web did not start; motor configured but not web-reachable.");
        }
    }

    int n = 0;
    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        if ((++n % 10) == 0) ESP_LOGI(TAG, "alive (%d s)", n);
    }
}
