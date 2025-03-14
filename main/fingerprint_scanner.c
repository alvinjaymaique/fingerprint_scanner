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

    fingerprint_set_command(&PS_SetChipAddr, PS_SetChipAddr.code.command, (uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF}, 4);
    fingerprint_send_command(&PS_SetChipAddr, DEFAULT_FINGERPRINT_ADDRESS);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // // Create task for sending commands
    // xTaskCreate(send_command_task, "SendCommandTask", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Fingerprint scanner initialized and waiting for a finger to be detected.");

    uint16_t location = 1;  // Storage location for fingerprint template
    // esp_err_t out = delete_fingerprint(location);
    // if (out == ESP_OK) {
    //     ESP_LOGI(TAG, "Fingerprint deleted successfully!");
    // } else {
    //     ESP_LOGE(TAG, "Failed to delete fingerprint!");
    // }
    
    err = enroll_fingerprint(location);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Fingerprint Enrolled!");
        // Add your access control logic here
    } else {
        ESP_LOGE(TAG, "Fingeprint not enrolled!");
        // Add your failure handling here
    }

    // esp_err_t out = delete_fingerprint(location);
    // if (out == ESP_OK) {
    //     ESP_LOGI(TAG, "Fingerprint deleted successfully!");
    // } else {
    //     ESP_LOGE(TAG, "Failed to delete fingerprint!");
    // }

    // err = clear_database();
    // if (err == ESP_OK) {
    //     ESP_LOGI(TAG, "Fingerprint database cleared successfully!");
    // } else {
    //     ESP_LOGE(TAG, "Failed to clear fingerprint database!");
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

    // Get number of enrolled fingerprints
    uint16_t enrolled_count;
    err = get_enrolled_count();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Count of enrolled fingerprints sent successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to send command to the enrolled fingerprints.");
    }

    // err = read_system_parameters();
    // if (err == ESP_OK) {
    //     ESP_LOGI(TAG, "System parameters read successfully.");
    // } else {
    //     ESP_LOGE(TAG, "Failed to read system parameters.");
    // }

    // Backup Template
    uint16_t template_id = 0;
    ESP_LOGI(TAG, "Backing up template id 0x%04X", location);
    err = backup_template(template_id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Template backed up successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to backup template.");
    }

    // // Example usage in app_main
    // err = read_info_page();
    // if (err == ESP_OK) {
    //     ESP_LOGI(TAG, "Information page read successfully");
    // } else {
    //     ESP_LOGE(TAG, "Failed to read information page");
    // }
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
            // check_duplicate_fingerprint();
            // ESP_LOGI(TAG, "Event address: 0x%08lX", (unsigned long)event.packet.address);
            // ESP_LOG_BUFFER_HEX("Event packet address: ", &event.packet, sizeof(FingerprintPacket));
            // ESP_LOG_BUFFER_HEX("Finger Detected Parameters ", event.packet.parameters, sizeof(event.packet.parameters));
            break;
        case EVENT_IMAGE_CAPTURED:
            ESP_LOGI(TAG, "Fingerprint image captured successfully! Status: 0x%02X", event.status);
            break;
        case EVENT_FEATURE_EXTRACTED:
            ESP_LOGI(TAG, "Fingerprint features extracted successfully! Status: 0x%02X", event.status);
            // ESP_LOG_BUFFER_HEX("Feautre Extracted Parameters ", event.packet.parameters, sizeof(event.packet.parameters));
            break;
        case EVENT_MATCH_SUCCESS:
            ESP_LOGI(TAG, "Fingerprint match successful! Status: 0x%02X", event.status);
            ESP_LOGI(TAG, "Match found at Enrollee ID: %d", event.data.match_info.template_id);
            ESP_LOGI(TAG, "Match score: %d", event.data.match_info.match_score);
            break;
        case EVENT_MATCH_FAIL:
            ESP_LOGI(TAG, "Fingerprint mismatch. Status: 0x%02X", event.status);
            break;
        case EVENT_ERROR:
            ESP_LOGE(TAG, "An error occurred during fingerprint processing. Status: 0x%02X", event.status);
            ESP_LOGE(TAG, "Command: 0x%02X", event.command);
            break;
        case EVENT_NO_FINGER_DETECTED:
            // ESP_LOGI(TAG, "No finger detected. Status: 0x%02X", event.status);
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
            ESP_LOGI(TAG, "Match found at Enrollee ID: %d", event.data.match_info.template_id);
            ESP_LOGI(TAG, "Match score: %d", event.data.match_info.match_score);
            break;
        case EVENT_INDEX_TABLE_READ:
            ESP_LOGI(TAG, "Index table read successful. Status: 0x%02X", event.status);
            ESP_LOG_BUFFER_HEX("Index Table Parameters", event.packet.parameters, sizeof(event.packet.parameters));
            break;
        case EVENT_TEMPLATE_COUNT:
            ESP_LOGI("EVENT TEMPLATE COUNT", "Number of valid templates: %d", event.data.template_count.count);
            break;
        case EVENT_SYS_PARAMS_READ:
            ESP_LOGI(TAG, "System parameters read successfully. Status: 0x%02X", event.status);
            ESP_LOGI(TAG, "Status Register: 0x%04X", event.data.sys_params.status_register);
            ESP_LOGI(TAG, "System ID: 0x%04X", event.data.sys_params.system_id);
            ESP_LOGI(TAG, "Fingerprint Database Size: 0x%04X", event.data.sys_params.finger_library);
            ESP_LOGI(TAG, "Security Level: 0x%04X", event.data.sys_params.security_level);
            ESP_LOGI(TAG, "Device Address: 0x%08" PRIX32, event.data.sys_params.device_address);  // Fix for uint32_t
            ESP_LOGI(TAG, "Data Packet Size: %u bytes", event.data.sys_params.data_packet_size); // No need for hex
            ESP_LOGI(TAG, "Baud Rate: %u bps", event.data.sys_params.baud_rate); // Convert baud multiplier to actual baud rate
            break;
        case EVENT_TEMPLATE_UPLOADED:
            // const char* newTag = "EVENT_TEMPLATE_UPLOADED";
            // ESP_LOGI(newTag, "Template command: 0x%02X", event.command);
            // ESP_LOGI(newTag, "Template package ID: 0x%02X", event.packet.packet_id);
            // ESP_LOGI(newTag, "Template confirmation code: 0x%02X", event.packet.code.confirmation);
            // ESP_LOGI(newTag, "Template address: 0x%08" PRIX32, event.packet.address);
            // ESP_LOGI(newTag, "Template uploaded successfully. Status: 0x%02X", event.status);
            // ESP_LOG_BUFFER_HEX(newTag, event.packet.parameters, sizeof(event.packet.parameters));
            // Add packet ID check  
            // ESP_LOG_BUFFER_HEXDUMP("Template uploaded successfully", event.packet.parameters, event.packet.length, ESP_LOG_INFO);
            // ESP_LOG_BUFFER_HEXDUMP("Template uploaded successfully", 
            //     (uint8_t*)&event.packet, 
            //     sizeof(FingerprintPacket), 
            //     ESP_LOG_INFO);

            if (event.packet.packet_id == 0x08) {
                // This is the final packet - print the complete template
                if (event.data.template_data.data && event.data.template_data.size > 0) {
                    ESP_LOGI(TAG, "Complete template data (%d bytes):", event.data.template_data.size);
                    
                    // Print data in manageable chunks (64 bytes at a time)
                    for (size_t offset = 0; offset < event.data.template_data.size; offset += 64) {
                        // Use inline expression instead of min() function
                        size_t chunk_size = (event.data.template_data.size - offset < 64) ? 
                                             event.data.template_data.size - offset : 64;
                        
                        ESP_LOG_BUFFER_HEX_LEVEL(TAG, 
                                           event.data.template_data.data + offset, 
                                           chunk_size, ESP_LOG_INFO);
                    }
                    
                    // Now free the data without referencing g_template_buffer
                    heap_caps_free(event.data.template_data.data);
                }
            } else {
                // For individual packets, just print basic info
                ESP_LOGI(TAG, "Template packet: ID=0x%02X, Length=%d", 
                         event.packet.packet_id, event.packet.length);
            }
            ESP_LOGI(TAG, "Fingerprint template uploaded successfully. Status: 0x%02X", event.status);
            break;
        case EVENT_TEMPLATE_EXISTS:
            ESP_LOGI(TAG, "Fingerprint template successfully loaded into buffer. Status: 0x%02X", event.status);
            break;
        case EVENT_TEMPLATE_UPLOAD_FAIL:
            ESP_LOGE(TAG, "Fingerprint template upload failed. Status: 0x%02X", event.status);
            break;
        // Add to handle_fingerprint_event function
        case EVENT_INFO_PAGE_READ:
            ESP_LOGI(TAG, "Information page read successfully. Status: 0x%02X", event.status);
            ESP_LOGI(TAG, "Packet ID read successfully. Status: 0x%02X", event.packet.packet_id);
            ESP_LOGI(TAG, "Packet length read successfully. Status: 0x%02X", event.packet.length);
            ESP_LOG_BUFFER_HEX("Info Page Data", event.packet.parameters, event.packet.length);
            break;
        case EVENT_TEMPLATE_LOADED:
            ESP_LOGI(TAG, "Template loaded successfully. Status: 0x%02X", event.status);
            break;
        default:
            ESP_LOGI(TAG, "Unknown event triggered. Status: 0x%02X", event.status);
            break;
    }
}
