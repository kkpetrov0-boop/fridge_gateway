#pragma once

#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

typedef struct{
    uint8_t hum_int;
    uint8_t hum_dec;
    uint8_t temp_int;
    uint8_t temp_dec;
    uint8_t parity_bit;
} DHT11;

esp_err_t dht11_read(DHT11* dht11, gpio_num_t sensor_pin);