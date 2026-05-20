#include "tmc2208.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "tmc2208";

#define TMC_SYNC         0x05
#define TMC_MASTER_ADDR  0xFF
#define UART_TIMEOUT_MS  50

#define RX_BUF_SIZE 256
#define TX_BUF_SIZE 0    // no TX ring buffer; uart_write_bytes blocks until queued

// TMC's custom CRC8: poly 0x07, init 0, LSB-first bit shift (per datasheet C reference).
static uint8_t tmc_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t cur = data[i];
        for (int b = 0; b < 8; b++) {
            if ((crc >> 7) ^ (cur & 0x01)) {
                crc = (uint8_t)((crc << 1) ^ 0x07);
            } else {
                crc = (uint8_t)(crc << 1);
            }
            cur >>= 1;
        }
    }
    return crc;
}

esp_err_t tmc2208_init(tmc2208_t *dev, const tmc2208_config_t *cfg)
{
    if (!dev || !cfg) return ESP_ERR_INVALID_ARG;
    memset(dev, 0, sizeof(*dev));
    dev->cfg = *cfg;

    uart_config_t uart_cfg = {
        .baud_rate  = cfg->baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(cfg->uart_port, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(cfg->uart_port, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        uart_driver_delete(cfg->uart_port);
        return err;
    }
    err = uart_set_pin(cfg->uart_port, cfg->tx_pin, cfg->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        uart_driver_delete(cfg->uart_port);
        return err;
    }

    dev->initialized = true;
    ESP_LOGI(TAG, "UART%d ready (TX=%d RX=%d @ %d baud, slave=0x%02x)",
             cfg->uart_port, cfg->tx_pin, cfg->rx_pin, cfg->baud, cfg->slave_addr);
    return ESP_OK;
}

esp_err_t tmc2208_deinit(tmc2208_t *dev)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_STATE;
    uart_driver_delete(dev->cfg.uart_port);
    dev->initialized = false;
    return ESP_OK;
}

esp_err_t tmc2208_read_register(tmc2208_t *dev, uint8_t reg, uint32_t *value)
{
    if (!dev || !value || !dev->initialized) return ESP_ERR_INVALID_ARG;
    uart_port_t port = dev->cfg.uart_port;

    uint8_t req[4];
    req[0] = TMC_SYNC;
    req[1] = dev->cfg.slave_addr;
    req[2] = (uint8_t)(reg & 0x7F);  // bit 7 = 0 → read
    req[3] = tmc_crc8(req, 3);

    uart_flush_input(port);

    int written = uart_write_bytes(port, (const char *)req, 4);
    if (written != 4) {
        ESP_LOGE(TAG, "uart_write_bytes wrote %d/4", written);
        return ESP_FAIL;
    }
    uart_wait_tx_done(port, pdMS_TO_TICKS(UART_TIMEOUT_MS));

    // Single-wire bus: our 4-byte TX echoes back on RX. Consume it.
    uint8_t echo[4];
    int n = uart_read_bytes(port, echo, 4, pdMS_TO_TICKS(UART_TIMEOUT_MS));
    if (n != 4) {
        ESP_LOGD(TAG, "echo read got %d/4 -- wiring or RX issue", n);
        return ESP_ERR_TIMEOUT;
    }

    // Float the TX pin before the chip drives the bus. Without this, GPIO 26's
    // idle-HIGH push-pull driver fights the chip's LOW through the 1 kOhm
    // series resistor, and V_bus during the chip's "0" bits may stay above the
    // ESP32 input LOW threshold -- chip replies look like all-1s and decode as
    // nothing. gpio_reset_pin reroutes the IO MUX from UART back to GPIO so the
    // UART hardware no longer controls the pin's output enable; we then
    // explicitly disable both pull-up and pull-down for true high-Z.
    gpio_reset_pin(dev->cfg.tx_pin);
    gpio_set_direction(dev->cfg.tx_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(dev->cfg.tx_pin, GPIO_FLOATING);

    // Chip reply: 8 bytes, within ~250 µs of last TX byte per datasheet.
    uint8_t reply[8];
    n = uart_read_bytes(port, reply, 8, pdMS_TO_TICKS(UART_TIMEOUT_MS));

    // Reattach the TX pin to UART for subsequent transactions.
    uart_set_pin(port, dev->cfg.tx_pin, dev->cfg.rx_pin,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (n != 8) {
        ESP_LOGD(TAG, "reply read got %d/8 -- chip not responding (check VIO, jumper, GND)", n);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t expected_crc = tmc_crc8(reply, 7);
    if (expected_crc != reply[7]) {
        ESP_LOGW(TAG, "reply CRC mismatch: got 0x%02x, expected 0x%02x", reply[7], expected_crc);
        return ESP_ERR_INVALID_CRC;
    }
    if (reply[0] != TMC_SYNC || reply[1] != TMC_MASTER_ADDR || reply[2] != (reg & 0x7F)) {
        ESP_LOGW(TAG, "bad reply header: %02x %02x %02x", reply[0], reply[1], reply[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *value = ((uint32_t)reply[3] << 24) | ((uint32_t)reply[4] << 16)
           | ((uint32_t)reply[5] << 8)  | (uint32_t)reply[6];
    return ESP_OK;
}

esp_err_t tmc2208_write_register(tmc2208_t *dev, uint8_t reg, uint32_t value)
{
    if (!dev || !dev->initialized) return ESP_ERR_INVALID_ARG;
    uart_port_t port = dev->cfg.uart_port;

    uint8_t req[8];
    req[0] = TMC_SYNC;
    req[1] = dev->cfg.slave_addr;
    req[2] = (uint8_t)(reg | 0x80);  // bit 7 = 1 → write
    req[3] = (uint8_t)((value >> 24) & 0xFF);
    req[4] = (uint8_t)((value >> 16) & 0xFF);
    req[5] = (uint8_t)((value >> 8)  & 0xFF);
    req[6] = (uint8_t)(value & 0xFF);
    req[7] = tmc_crc8(req, 7);

    uart_flush_input(port);

    int written = uart_write_bytes(port, (const char *)req, 8);
    if (written != 8) {
        ESP_LOGE(TAG, "uart_write_bytes wrote %d/8", written);
        return ESP_FAIL;
    }
    uart_wait_tx_done(port, pdMS_TO_TICKS(UART_TIMEOUT_MS));

    // Writes have no reply, but our 8 TX bytes still echo back. Consume.
    uint8_t echo[8];
    int n = uart_read_bytes(port, echo, 8, pdMS_TO_TICKS(UART_TIMEOUT_MS));
    if (n != 8) {
        ESP_LOGW(TAG, "echo read got %d/8 after write", n);
    }
    return ESP_OK;
}

esp_err_t tmc2208_get_version(tmc2208_t *dev, uint8_t *version)
{
    if (!version) return ESP_ERR_INVALID_ARG;
    uint32_t ioin = 0;
    esp_err_t err = tmc2208_read_register(dev, TMC2208_REG_IOIN, &ioin);
    if (err != ESP_OK) return err;
    *version = (uint8_t)((ioin >> 24) & 0xFF);
    return ESP_OK;
}

esp_err_t tmc2208_get_ifcnt(tmc2208_t *dev, uint8_t *ifcnt)
{
    if (!ifcnt) return ESP_ERR_INVALID_ARG;
    uint32_t v = 0;
    esp_err_t err = tmc2208_read_register(dev, TMC2208_REG_IFCNT, &v);
    if (err != ESP_OK) return err;
    *ifcnt = (uint8_t)(v & 0xFF);
    return ESP_OK;
}
