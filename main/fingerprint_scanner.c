#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "fingerprint.h"

#define TAG "FINGERPRINT"
#define TEST_PIN (GPIO_NUM_19)
void handle_fingerprint_event(fingerprint_event_t event);
void send_command_task(void *pvParameter);

// // Task to set GPIO 11 HIGH after 2 seconds
// void set_gpio_high_task(void *pvParameter)
// {
//     vTaskDelay(pdMS_TO_TICKS(2000)); // Non-blocking delay of 2 seconds
//     gpio_set_level(TEST_PIN, 0);
//     ESP_LOGI(TAG, "GPIO 11 set to LOW after 5 seconds.");
//     vTaskDelay(pdMS_TO_TICKS(1000)); // Non-blocking delay of 2 seconds
//     gpio_set_level(TEST_PIN, 1);
//     ESP_LOGI(TAG, "GPIO 11 set to HIGH after 5 seconds.");

//     vTaskDelete(NULL); // Delete task after execution
// }

void app_main(void)
{
    register_fingerprint_event_handler(handle_fingerprint_event);

    // // Set GPIO 11 as output
    // gpio_set_direction(TEST_PIN, GPIO_MODE_OUTPUT);
    // // Delay 2 seconds before setting GPIO 11 HIGH
    // vTaskDelay(pdMS_TO_TICKS(2000));
    // gpio_set_level(TEST_PIN, 1);
    // ESP_LOGI(TAG, "GPIO 11 set to HIGH after 2 seconds.");

    // // Set GPIO 11 as output
    // gpio_set_direction(TEST_PIN, GPIO_MODE_OUTPUT);
    // gpio_set_level(TEST_PIN, 1); // Initially set to HIGH

    // // Create a non-blocking task to set GPIO 11 LOW after 5 seconds
    // xTaskCreate(set_gpio_high_task, "SetGPIOHighTask", 2048, NULL, 5, NULL);

    esp_err_t err = fingerprint_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fingerprint initialization failed");
        return;
    }

    // Allow module to stabilize before sending commands
    vTaskDelay(pdMS_TO_TICKS(100));

    fingerprint_set_command(&PS_SetChipAddr, PS_SetChipAddr.command, (uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF}, 4);
    // fingerprint_send_command(&PS_SetChipAddr, DEFAULT_FINGERPRINT_ADDRESS);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Create task for sending commands
    xTaskCreate(send_command_task, "SendCommandTask", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Fingerprint scanner initialized and waiting for a finger to be detected.");
}

void send_command_task(void *pvParameter)
{
    while (1)
    {
        ESP_LOGI(TAG, "Attempting to send Get Image command...");

        esp_err_t err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send Get Image command! Error: %d", err);
        } else {
            ESP_LOGI(TAG, "Get Image command sent successfully.");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));  // Delay before sending the next command
    }
}

// Event handler function
void handle_fingerprint_event(fingerprint_event_t event) {
    switch (event.type) {
        case EVENT_SCANNER_READY:
            ESP_LOGI(TAG, "Fingerprint scanner is ready for operation. Status: 0x%02X", event.status);
            break;
        case EVENT_FINGER_DETECTED:
            ESP_LOGI(TAG, "Finger detected! Status: 0x%02X", event.status);
            // ESP_LOGI(TAG, "Event address: 0x%08lX", (unsigned long)event.packet.address);
            // ESP_LOG_BUFFER_HEX("Event packet address: ", &event.packet, sizeof(FingerprintPacket));
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
        case EVENT_NO_FINGER_DETECTED:
            ESP_LOGI(TAG, "No finger detected. Status: 0x%02X", event.status);
        case EVENT_ENROLL_SUCCESS:
            ESP_LOGI(TAG, "Fingerprint enrollment successful! Status: 0x%02X", event.status);
            break;
        case EVENT_ENROLL_FAIL:
            ESP_LOGI(TAG, "Fingerprint enrollment failed. Status: 0x%02X", event.status);
            break;
        default:
            ESP_LOGI(TAG, "Unknown event triggered. Status: 0x%02X", event.status);
            break;
    }
}
