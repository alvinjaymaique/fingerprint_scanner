#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "fingerprint.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"


#define TAG "FINGERPRINT"

#define EVENT_BIT_RESPONSE_READY (1 << 0)  // Define an event bit

static EventGroupHandle_t fingerprint_event_group;  // Global event group


void handle_fingerprint_event(fingerprint_event_t event);
void send_command_task(void *pvParameter);
void read_response_task(void *pvParameter);

void app_main(void)
{
    // // Set the GPIO pin HIGH (turn it on)
    // gpio_set_level(15, 1);
    // gpio_set_level(48, 1);
    fingerprint_event_group = xEventGroupCreate();  // Initialize event group

    register_fingerprint_event_handler(handle_fingerprint_event);
    esp_err_t err = fingerprint_init();
    if (err != ESP_OK) {
        printf("Fingerprint initialization failed\n");
        return;
    }

    // Create tasks for sending commands and reading responses
    xTaskCreate(send_command_task, "SendCommandTask", 4096, NULL, 5, NULL);
    xTaskCreate(read_response_task, "ReadResponseTask", 4096, NULL, 5, NULL);
}


void send_command_task(void *pvParameter)
{
    while (1)  // Keep running indefinitely
    {
        ESP_LOGI(TAG, "Attempting to send GetImage command...");

        esp_err_t err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send GetImage command! Error: %d", err);
        } else {
            ESP_LOGI(TAG, "GetImage command sent successfully.");
            
            // Signal that a response is expected
            xEventGroupSetBits(fingerprint_event_group, EVENT_BIT_RESPONSE_READY);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));  // Delay before sending the next command
    }
}




void read_response_task(void *pvParameter)
{
    while (1)
    {
        // Wait indefinitely for the event bit to be set
        xEventGroupWaitBits(fingerprint_event_group, EVENT_BIT_RESPONSE_READY,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        FingerprintPacket* response = fingerprint_read_response();

        if (response != NULL)
        {
            fingerprint_status_t status = fingerprint_get_status(response);
            ESP_LOGI("Fingerprint", "Fingerprint status: %d", status);
            free(response);
        }
        else
        {
            ESP_LOGE("Fingerprint", "Failed to read response from fingerprint module");
        }
    }
}


// Event handler function
void handle_fingerprint_event(fingerprint_event_t event) {
    switch (event.type) {
        case EVENT_FINGER_DETECTED:
            ESP_LOGI("Fingerprint", "Finger detected! Status: 0x%02X", event.status);
            break;
        case EVENT_IMAGE_CAPTURED:
            ESP_LOGI("Fingerprint", "Fingerprint image captured successfully! Status: 0x%02X", event.status);
            break;
        case EVENT_FEATURE_EXTRACTED:
            ESP_LOGI("Fingerprint", "Fingerprint features extracted successfully! Status: 0x%02X", event.status);
            break;
        case EVENT_MATCH_SUCCESS:
            ESP_LOGI("Fingerprint", "Fingerprint match successful! Status: 0x%02X", event.status);
            break;
        case EVENT_MATCH_FAIL:
            ESP_LOGI("Fingerprint", "Fingerprint mismatch. Status: 0x%02X", event.status);
            break;
        case EVENT_ERROR:
            ESP_LOGI("Fingerprint", "An error occurred during fingerprint processing. Status: 0x%02X", event.status);
            break;
        default:
            ESP_LOGI("Fingerprint", "Unknown event triggered. Status: 0x%02X", event.status);
            break;
    }
}
