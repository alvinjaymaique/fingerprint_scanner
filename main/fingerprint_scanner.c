#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "fingerprint.h"

#define TAG "FINGERPRINT"

void handle_fingerprint_event(fingerprint_event_t event);
void send_command_task(void *pvParameter);

void app_main(void)
{
    register_fingerprint_event_handler(handle_fingerprint_event);
    
    esp_err_t err = fingerprint_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fingerprint initialization failed");
        return;
    }

    // Allow module to stabilize before sending commands
    vTaskDelay(pdMS_TO_TICKS(100));

    // Create task for sending commands
    xTaskCreate(send_command_task, "SendCommandTask", 4096, NULL, 5, NULL);
}

void send_command_task(void *pvParameter)
{
    while (1)
    {
        ESP_LOGI(TAG, "Attempting to send Handshake command...");

        esp_err_t err = fingerprint_send_command(&PS_HandShake, DEFAULT_FINGERPRINT_ADDRESS);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send Handshake command! Error: %d", err);
        } else {
            ESP_LOGI(TAG, "Handshake command sent successfully.");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));  // Delay before sending the next command
    }
}

// Event handler function
void handle_fingerprint_event(fingerprint_event_t event) {
    switch (event.type) {
        case EVENT_FINGER_DETECTED:
            ESP_LOGI(TAG, "Finger detected! Status: 0x%02X", event.status);
            break;
        case EVENT_IMAGE_CAPTURED:
            ESP_LOGI(TAG, "Fingerprint image captured successfully! Status: 0x%02X", event.status);
            break;
        case EVENT_FEATURE_EXTRACTED:
            ESP_LOGI(TAG, "Fingerprint features extracted successfully! Status: 0x%02X", event.status);
            break;
        case EVENT_MATCH_SUCCESS:
            ESP_LOGI(TAG, "Fingerprint match successful! Status: 0x%02X", event.status);
            break;
        case EVENT_MATCH_FAIL:
            ESP_LOGI(TAG, "Fingerprint mismatch. Status: 0x%02X", event.status);
            break;
        case EVENT_ERROR:
            ESP_LOGI(TAG, "An error occurred during fingerprint processing. Status: 0x%02X", event.status);
            break;
        default:
            ESP_LOGI(TAG, "Unknown event triggered. Status: 0x%02X", event.status);
            break;
    }
}
