#include <stdio.h>
#include "esp_log.h"
#include "fingerprint.h"

void handle_fingerprint_event(fingerprint_event_t event);

void app_main(void)
{
    register_fingerprint_event_handler(handle_fingerprint_event);
    esp_err_t err = fingerprint_init();
    if (err != ESP_OK) {
        printf("Fingerprint initialization failed\n");
        return;
    }

    err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        printf("Failed to send GetImage command\n");
        return;
    }
    
    err = fingerprint_send_command(&PS_GenChar1, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        printf("Failed to send generate a character file in Buffer 1.\n");
        return;
    }
    
    err = fingerprint_send_command(&PS_RegModel, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        printf("Failed to send combines feature templates stored in Buffer 1 and Buffer 2 into a single fingerprint model.\n");
        return;
    }
    trigger_fingerprint_event(EVENT_FINGER_DETECTED);

    FingerprintPacket* response = fingerprint_read_response();
    fingerprint_status_t status = fingerprint_get_status(response);
    printf("Fingerprint status: %d\n", status); // Should print 0 (FINGERPRINT_OK)
    free(response);
}

// Sample code for event handler
void handle_fingerprint_event(fingerprint_event_t event) {
    switch (event) {
        case EVENT_FINGER_DETECTED:
            ESP_LOGI("Fingerprint", "Finger detected!");
            break;
        case EVENT_IMAGE_CAPTURED:
            ESP_LOGI("Fingerprint", "Fingerprint image captured successfully!");
            break;
        case EVENT_FEATURE_EXTRACTED:
            ESP_LOGI("Fingerprint", "Fingerprint features extracted successfully!");
            break;
        case EVENT_MATCH_SUCCESS:
            ESP_LOGI("Fingerprint", "Fingerprint match successful!");
            break;
        case EVENT_MATCH_FAIL:
            ESP_LOGI("Fingerprint", "Fingerprint mismatch.");
            break;
        case EVENT_ERROR:
            ESP_LOGI("Fingerprint", "An error occurred during fingerprint processing.");
            break;
        default:
            ESP_LOGI("Fingerprint", "Unknown event triggered.");
            break;
    }
}