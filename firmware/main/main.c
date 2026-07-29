#include <stdio.h>
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "string.h"
#include "dht11.h"

#define DHT11_SENSOR_PIN GPIO_NUM_4
TaskHandle_t dht11_task_handle;
void DHT11_task(void* arg);

#define TELEM_NUM UART_NUM_1
#define TELEM_TX_PIN GPIO_NUM_17
#define TELEM_RX_PIN GPIO_NUM_16
#define TELEM_BAUD_RATE 115200
static const int TELEM_BUFFER_SIZE = 256;
TaskHandle_t telemetry_task_handle;
void telemetry_task(void* arg);

TaskHandle_t command_task_handle;
void command_task(void* arg);

QueueHandle_t telemetry_queue;

typedef enum comp_state_machine {
    COMP_OFF = 0,
    COMP_ON = 1
} comp_state_t;

void telem_uart_init(void);
void gpio_init(void);
unsigned char calculate_xor_checksum(const char* str);

volatile float g_setpoint = 4.0; // Setpoint temperature in Celsius

void app_main(void)
{
    telem_uart_init();
    gpio_init();
    vTaskDelay(pdMS_TO_TICKS(5000)); // Delay for 5 second to allow UART initialization
    telemetry_queue = xQueueCreate(5, sizeof(DHT11));
    xTaskCreate(DHT11_task, "DHT11_task", 2048, NULL, 15, &dht11_task_handle);
    xTaskCreate(telemetry_task, "telemetry_task", 2048, NULL, 8, &telemetry_task_handle);
    xTaskCreate(command_task, "command_task", 2048, NULL, 9, &command_task_handle);
}

void telem_uart_init(void)
{
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
}

void gpio_init(void)
{
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << DHT11_SENSOR_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

void telemetry_task(void* arg)
{
    DHT11 telemetry_data;
    uint64_t seq = 0;
    
    float hysteresis = 0.5; // Hysteresis value in Celsius
    unsigned char checksum;
    comp_state_t comp_state = 0;
    while (1)
    {
        xQueueReceive(telemetry_queue, &telemetry_data, portMAX_DELAY);

        float temp = telemetry_data.temp_int + telemetry_data.temp_dec/10.0;
        switch (comp_state)
        {
        case COMP_OFF:
            if (temp > g_setpoint + hysteresis){
                comp_state = COMP_ON;
            }
            break;
        case COMP_ON:
            if (temp < g_setpoint - hysteresis){
                comp_state = COMP_OFF;
            }
            break;
        default:
            break;
        }
        uint8_t comp = (comp_state == COMP_ON) ? 1 : 0;
        char frame[256];
        snprintf(frame, sizeof(frame), "FRIDGE,temp=%.1f,setpoint=%.1f,hysteresis=%.1f,comp=%hhd,seq=%lld",
                temp, g_setpoint, hysteresis, comp, seq);

        checksum = calculate_xor_checksum(frame);
        snprintf(frame + strlen(frame), sizeof(frame) - strlen(frame), "*%02X\n", checksum);
        seq++;
        uart_write_bytes(TELEM_NUM, frame, strlen(frame));

        //vTaskDelay(pdMS_TO_TICKS(1000)); // Delay for 1 second
    }
}

void command_task(void* arg){
    uint8_t buf[128];
    int idx = 0;

    while (1){
        uint8_t ch;
        int len = uart_read_bytes(TELEM_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (len > 0){
            //ESP_LOGI("CMD", "setpoint updated to"); 
            if (ch == '\n'){
                buf[idx] = '\0';
                // ESP_LOGI("CMD", "raw buffer: [%s]", buf);
                char *cmd = strstr((char*)buf, "SET,setpoint=");
                if (cmd != NULL){
                    float new_setpoint = g_setpoint; // Initialize with current setpoint
                    if (sscanf(cmd, "SET,setpoint=%f", &new_setpoint) == 1){
                        g_setpoint = new_setpoint;
                        ESP_LOGI("CMD", "setpoint updated to %.1f", new_setpoint);
                    }
                }
                idx = 0;
            } else if (idx < sizeof(buf) - 1){
                buf[idx++] = ch;
            }
        }
    }
}

void DHT11_task(void* arg)
{
    DHT11 dht11_data;
    uint8_t count = 0;
    while (1)
    {
        esp_err_t err = dht11_read(&dht11_data, DHT11_SENSOR_PIN);
        if (err == ESP_OK)
        {
            count = 0;
            printf("Humidity: %d.%d%%, Temperature: %d.%d°C\n", dht11_data.hum_int, dht11_data.hum_dec, dht11_data.temp_int, dht11_data.temp_dec);
            // Call the SD card logging function here
            xQueueSend(telemetry_queue, &dht11_data, portMAX_DELAY);
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

unsigned char calculate_xor_checksum(const char* str){
    unsigned char checksum = 0;
    while (*str != '\0' && *str != '\n') {
        checksum ^= *str;
        str++;
    }
    return checksum;
}