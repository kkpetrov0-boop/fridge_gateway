#include <stdio.h>
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "dht11.h"
#include "esp_log.h"
#include "sd_card_spi.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "string.h"

#define DHT11_SENSOR_PIN GPIO_NUM_4

#define TELEM_NUM UART_NUM_1
#define TELEM_TX_PIN GPIO_NUM_17
#define TELEM_RX_PIN GPIO_NUM_16
#define TELEM_BAUD_RATE 115200

static const int TELEM_BUFFER_SIZE = 256;

void DHT11_task(void* arg);
void SD_task(void* arg);

TaskHandle_t dht11_task_handle;
TaskHandle_t sd_task_handle;
QueueHandle_t sd_log_queue;

void app_main(void)
{
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << DHT11_SENSOR_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    const uart_config_t uart_config = {
        .baud_rate = TELEM_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(TELEM_NUM, TELEM_BUFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(TELEM_NUM, &uart_config);
    uart_set_pin(TELEM_NUM, TELEM_TX_PIN, TELEM_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);


    ESP_ERROR_CHECK(SD_log_init(NULL));

    sd_log_queue = xQueueCreate(5, sizeof(DHT11));
    xTaskCreate(DHT11_task, "DHT11_Task", 2048, NULL, 10, &dht11_task_handle);
    xTaskCreate(SD_task, "SD_task", 4096, NULL, 5, &sd_task_handle);
}

void DHT11_task(void* arg)
{
    DHT11 dht11_data;
    vTaskDelay(pdMS_TO_TICKS(5000)); // Delay for 5 seconds before the first reading
    uint8_t count = 0;
    while (1)
    {
        esp_err_t err = dht11_read(&dht11_data, DHT11_SENSOR_PIN);
        if (err == ESP_OK)
        {
            count = 0;
            printf("Humidity: %d.%d%%, Temperature: %d.%d°C\n", dht11_data.hum_int, dht11_data.hum_dec, dht11_data.temp_int, dht11_data.temp_dec);
            // Call the SD card logging function here
            xQueueSend(sd_log_queue, &dht11_data, portMAX_DELAY);
        }
        else
        {
            count++;
            ESP_LOGE("DHT11", "Failed to read from DHT11 sensor, error: %s", esp_err_to_name(err));
            if (count >= 5) {
                vTaskDelete(NULL); // Delete the task after 5 consecutive failures
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // Delay for 2 seconds before the next reading
    }
}

//Получаем в очередь данные с сенсора и записываем их на SD карту
void SD_task(void* arg){
    DHT11 dht11_data;
    while (1)
    {
        if (xQueueReceive(sd_log_queue, &dht11_data, portMAX_DELAY) == pdTRUE) {
            SD_log_append(&dht11_data);
        }
    }
}

