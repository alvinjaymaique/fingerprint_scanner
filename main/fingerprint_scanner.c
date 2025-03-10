#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "fingerprint.h"

#define TAG "FINGERPRINT"
#define TEST_PIN (GPIO_NUM_46)
#define SCANNER_GPIO GPIO_NUM_46  // Change this if needed (should support 40mA)
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

void configure_scanner_gpio() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SCANNER_GPIO), 
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   // No internal pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // No internal pull-down
        .intr_type = GPIO_INTR_DISABLE       // No interrupt needed
    };
    gpio_config(&io_conf);
    // Set drive strength to maximum (40mA)
    gpio_set_drive_capability(SCANNER_GPIO, GPIO_DRIVE_CAP_MAX);
    // Set initial state (HIGH or LOW as required)
    gpio_set_level(SCANNER_GPIO, 1);  // Set HIGH if needed
}


void app_main(void)
{
    // set_all_pins_high();
    register_fingerprint_event_handler(handle_fingerprint_event);
    // configure_scanner_gpio();
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
    fingerprint_send_command(&PS_SetChipAddr, DEFAULT_FINGERPRINT_ADDRESS);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // // Create task for sending commands
    // xTaskCreate(send_command_task, "SendCommandTask", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Fingerprint scanner initialized and waiting for a finger to be detected.");

    // ESP_LOGI(TAG, "Enrolling fingerprint...");
    // // Start the enrollment process
    // auto_enroll_fingerprint(1, 3);
    // manual_enroll_fingerprint_task();

    // uint16_t location = 0x0003;  // Storage location for fingerprint template
    // enroll_fingerprint(location);

    // esp_err_t out = delete_fingerprint(location);
    // if (out == ESP_OK) {
    //     ESP_LOGI(TAG, "Fingerprint deleted successfully!");
    // } else {
    //     ESP_LOGE(TAG, "Failed to delete fingerprint!");
    // }
    
    ESP_LOGI(TAG, "Starting fingerprint verification...");
    vTaskDelay(pdMS_TO_TICKS(2000));  // Delay before sending the next command
    esp_err_t result = verify_fingerprint();

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Access granted - fingerprint verified!");
        // Add your access control logic here
    } else {
        ESP_LOGE(TAG, "Access denied - fingerprint not recognized");
        // Add your failure handling here
    }
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
            // ESP_LOGI(TAG, "Fingerprint scanner is ready for operation. Status: 0x%02X", event.status);
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
            ESP_LOGI(TAG, "Match found at Page ID: %d", 
                (event.packet.parameters[1] << 8) | event.packet.parameters[0]);
            ESP_LOGI(TAG, "Match score: %d", 
                (event.packet.parameters[3] << 8) | event.packet.parameters[2]);
            break;
        case EVENT_MATCH_FAIL:
            ESP_LOGI(TAG, "Fingerprint mismatch. Status: 0x%02X", event.status);
            break;
        case EVENT_ERROR:
            ESP_LOGE(TAG, "An error occurred during fingerprint processing. Status: 0x%02X", event.status);
            ESP_LOGE(TAG, "Command: 0x%02X", event.packet.command);
            break;
        case EVENT_NO_FINGER_DETECTED:
            ESP_LOGI(TAG, "No finger detected. Status: 0x%02X", event.status);
            break;
        case EVENT_ENROLL_SUCCESS:
            ESP_LOGI(TAG, "Fingerprint enrollment successful! Status: 0x%02X", event.status);
            ESP_LOGI("Event Type", "Event: 0x%02X", event.type);
            break;
        case EVENT_ENROLL_FAIL:
            ESP_LOGI(TAG, "Fingerprint enrollment failed. Status: 0x%02X", event.status);
            break;
        case EVENT_TEMPLATE_MERGED:
            ESP_LOGI(TAG, "Fingerprint templates merged successfully. Status: 0x%02X", event.status);
            break;
        case EVENT_TEMPLATE_STORE_SUCCESS:
            ESP_LOGI(TAG, "Fingerprint template stored successfully. Status: 0x%02X", event.status);
            break;
        case EVENT_SEARCH_SUCCESS:
            ESP_LOGI(TAG, "Fingerprint search successful. Status: 0x%02X", event.status);
            ESP_LOGI(TAG, "Match found at Page ID: %d", 
                (event.packet.parameters[1] << 8) | event.packet.parameters[0]);
            ESP_LOGI(TAG, "Match score: %d", 
                (event.packet.parameters[3] << 8) | event.packet.parameters[2]);
            break;
        default:
            ESP_LOGI(TAG, "Unknown event triggered. Status: 0x%02X", event.status);
            break;
    }
}
