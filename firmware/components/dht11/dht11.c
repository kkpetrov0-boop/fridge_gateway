#include <stdio.h>
#include "dht11.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "hal/gpio_ll.h"

typedef enum {
    DHT11_STARTUP_LOW_TIME_US = 18000,
    DHT11_STARTUP_HIGH_TIME_US = 100,
    DHT11_RESPONSE_LOW_TIME_US = 150,
    DHT11_RESPONSE_HIGH_TIME_US = 150,
    DHT11_DATA_BIT_LOW_TIME_US = 100,
    DHT11_DATA_BIT_HIGH_TIME_0_US = 26,
    DHT11_DATA_BIT_HIGH_TIME_1_US = 70,
    DHT11_DATA_BIT_DIFF_TIME_1_US = 50,
}dht11_time_t;

typedef enum {
    DHT11_DATA_BIT_0 = 0,
    DHT11_DATA_BIT_1 = 1,
} dht11_data_bit_t;

static portMUX_TYPE my_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Cycles elapsed since 'start' (wraps safely on uint32_t overflow).
static inline IRAM_ATTR uint32_t cycles_since(uint32_t start){
    return esp_cpu_get_cycle_count() - start;
}

static IRAM_ATTR esp_err_t start_up_prot(dht11_time_t time_us, gpio_num_t sensor_pin, dht11_data_bit_t data_bit){
    uint32_t start = esp_cpu_get_cycle_count();
    uint32_t limit = time_us * esp_rom_get_cpu_ticks_per_us();
    while (gpio_ll_get_level(&GPIO, sensor_pin) == data_bit)
    {
        if (cycles_since(start) > limit) return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

#define DHT11_MAX_BITS 40
uint32_t dht11_bit_high_us[DHT11_MAX_BITS];
int dht11_bit_count;

static IRAM_ATTR esp_err_t dht11_data_read(uint8_t* data, gpio_num_t sensor_pin){
    uint32_t ticks_per_us = esp_rom_get_cpu_ticks_per_us();
    // Generous safety cap only - the actual 0/1 decision is made below from the
    // measured duration, not from where this loop happens to break.
    uint32_t hard_cap = 200 * ticks_per_us;

    for (int i = 0; i < 8; i++)
    {
        esp_err_t err = start_up_prot(DHT11_DATA_BIT_LOW_TIME_US, sensor_pin, DHT11_DATA_BIT_0);
        if (err != ESP_OK) return err;

        uint32_t start = esp_cpu_get_cycle_count();
        while (gpio_ll_get_level(&GPIO, sensor_pin) == DHT11_DATA_BIT_1)
        {
            if (cycles_since(start) > hard_cap) break;
        }
        uint32_t duration_us = cycles_since(start) / ticks_per_us;
        if (dht11_bit_count < DHT11_MAX_BITS) {
            dht11_bit_high_us[dht11_bit_count++] = duration_us;
        }

        *data <<= 1;
        *data |= (duration_us > DHT11_DATA_BIT_DIFF_TIME_1_US) ? 1 : 0;
    }
    return ESP_OK;
}

esp_err_t IRAM_ATTR dht11_read(DHT11* dht11, gpio_num_t sensor_pin){
    ESP_LOGI("DHT11", "Reading data from DHT11 sensor on GPIO %d", sensor_pin);

    gpio_set_direction(sensor_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(sensor_pin, 0);
    esp_rom_delay_us(DHT11_STARTUP_LOW_TIME_US);
    gpio_set_level(sensor_pin, 1);
    gpio_set_direction(sensor_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(sensor_pin, GPIO_PULLUP_ONLY);

    esp_err_t err;
    int failed_step = 0;
    dht11_bit_count = 0;

    portENTER_CRITICAL(&my_spinlock);

    failed_step = 1;
    err = start_up_prot(DHT11_STARTUP_HIGH_TIME_US, sensor_pin, DHT11_DATA_BIT_1);
    if (err != ESP_OK) goto cleanup;

    failed_step = 2;
    err = start_up_prot(DHT11_RESPONSE_LOW_TIME_US, sensor_pin, DHT11_DATA_BIT_0);
    if (err != ESP_OK) goto cleanup;

    failed_step = 3;
    err = start_up_prot(DHT11_RESPONSE_HIGH_TIME_US, sensor_pin, DHT11_DATA_BIT_1);
    if (err != ESP_OK) goto cleanup;

    failed_step = 4;
    err = dht11_data_read(&dht11->hum_int, sensor_pin);
    if (err != ESP_OK) goto cleanup;
    failed_step = 5;
    err = dht11_data_read(&dht11->hum_dec, sensor_pin);
    if (err != ESP_OK) goto cleanup;
    failed_step = 6;
    err = dht11_data_read(&dht11->temp_int, sensor_pin);
    if (err != ESP_OK) goto cleanup;
    failed_step = 7;
    err = dht11_data_read(&dht11->temp_dec, sensor_pin);
    if (err != ESP_OK) goto cleanup;
    failed_step = 8;
    err = dht11_data_read(&dht11->parity_bit, sensor_pin);
    if (err != ESP_OK) goto cleanup;
cleanup:
    portEXIT_CRITICAL(&my_spinlock);

   /* for (int i = 0; i < dht11_bit_count; i++) {
        printf("%lu ", (unsigned long)dht11_bit_high_us[i]);
    }
    printf("\n");*/

    if (err != ESP_OK) {
        ESP_LOGE("DHT11", "Failed to read data from DHT11 sensor on GPIO %d (step: %d)", sensor_pin, failed_step);
        return err;
    }

    /*ESP_LOGI("DHT11", "Successfully read data from DHT11 sensor on GPIO %d: Humidity: %X %X, Temperature: %X %X, Parity: %X",
             sensor_pin, dht11->hum_int, dht11->hum_dec, dht11->temp_int, dht11->temp_dec, dht11->parity_bit);
    */
    if (dht11->parity_bit != ((dht11->hum_int + dht11->hum_dec + dht11->temp_int + dht11->temp_dec) & 0xFF))
    {
        return ESP_ERR_INVALID_CRC;
    }
    
    return ESP_OK;
}