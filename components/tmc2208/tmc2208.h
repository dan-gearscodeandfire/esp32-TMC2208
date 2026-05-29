#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"

// TMC2208 register addresses (datasheet section 5)
#define TMC2208_REG_GCONF         0x00
#define TMC2208_REG_GSTAT         0x01
#define TMC2208_REG_IFCNT         0x02
#define TMC2208_REG_NODECONF      0x03
#define TMC2208_REG_OTP_PROG      0x04
#define TMC2208_REG_OTP_READ      0x05
#define TMC2208_REG_IOIN          0x06  // bits 31:24 = VERSION (0x20 for TMC2208)
#define TMC2208_REG_FACTORY_CONF  0x07
#define TMC2208_REG_IHOLD_IRUN    0x10
#define TMC2208_REG_TPOWERDOWN    0x11
#define TMC2208_REG_TSTEP         0x12
#define TMC2208_REG_TPWMTHRS      0x13
#define TMC2208_REG_VACTUAL       0x22
#define TMC2208_REG_MSCNT         0x6A
#define TMC2208_REG_MSCURACT      0x6B
#define TMC2208_REG_CHOPCONF      0x6C
#define TMC2208_REG_DRV_STATUS    0x6F
#define TMC2208_REG_PWMCONF       0x70
#define TMC2208_REG_PWM_SCALE     0x71
#define TMC2208_REG_PWM_AUTO      0x72

// TMC2209-only registers (StallGuard / CoolStep). The 2208 has no equivalents;
// writes are harmless but reads return 0, which would look like a constant
// stall. The motor module gates these behind a 2209 check.
#define TMC2209_REG_TCOOLTHRS     0x14  // 20-bit; non-zero enables SG/CoolStep when TSTEP <= TCOOLTHRS
#define TMC2209_REG_SGTHRS        0x40  // 8-bit StallGuard threshold; stall when SG_RESULT <= SGTHRS*2
#define TMC2209_REG_SG_RESULT     0x41  // 10-bit load reading; lower = higher mechanical load

typedef struct {
    uart_port_t uart_port;
    int tx_pin;
    int rx_pin;
    int baud;
    uint8_t slave_addr;  // 0x00 when MS1=MS2=0 (default unconnected)
} tmc2208_config_t;

typedef struct {
    tmc2208_config_t cfg;
    bool initialized;
} tmc2208_t;

esp_err_t tmc2208_init(tmc2208_t *dev, const tmc2208_config_t *cfg);
esp_err_t tmc2208_deinit(tmc2208_t *dev);

esp_err_t tmc2208_read_register(tmc2208_t *dev, uint8_t reg, uint32_t *value);
esp_err_t tmc2208_write_register(tmc2208_t *dev, uint8_t reg, uint32_t value);

// VERSION byte from IOIN[31:24]. Returns 0x20 for genuine TMC2208.
esp_err_t tmc2208_get_version(tmc2208_t *dev, uint8_t *version);

// IFCNT increments on every successful write — read before/after a write to verify
// the chip actually applied it.
esp_err_t tmc2208_get_ifcnt(tmc2208_t *dev, uint8_t *ifcnt);
