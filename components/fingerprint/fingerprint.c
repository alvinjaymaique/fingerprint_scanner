#include "fingerprint.h"  // Direct include instead of "include/fingerprint.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"     // Required for QueueHandle_t
#include "freertos/timers.h"    // Required for TickType_t
#include "driver/gpio.h"
#include <string.h>
#include <stdbool.h>   // Fixes unknown type name 'bool'
#include <inttypes.h>

#define TAG "FINGERPRINT"
#define UART_NUM UART_NUM_2  // Change based on your wiring
#define RX_BUF_SIZE 256  // Adjust based on fingerprint module response

static int tx_pin = DEFAULT_TX_PIN; // Default TX pin
static int rx_pin = DEFAULT_RX_PIN; // Default RX pin
static int baud_rate = DEFAULT_BAUD_RATE; // Default baud rate

static uint16_t global_location = 0; // Global variable to store location
static bool is_fingerprint_validating = false; // Flag to check if fingerprint is validating

static MultiPacketResponse *g_template_accumulator = NULL; // Global template accumulator

/**
 * @brief Stores the last fingerprint command sent to the module.
 * 
 * This variable helps in identifying the type of response received
 * from the fingerprint module, as response packets do not include
 * an explicit command field.
 */
static uint8_t last_sent_command = 0x00;

// Define the global event handler function pointer
fingerprint_event_handler_t g_fingerprint_event_handler = NULL;

static TaskHandle_t fingerprint_task_handle = NULL;
static TaskHandle_t finger_detection_task_handle = NULL;
static QueueHandle_t fingerprint_response_queue = NULL;
static QueueHandle_t fingerprint_command_queue = NULL;
static QueueHandle_t finger_detected_queue = NULL;

// Add these variables at the top of the file with other static variables
static finger_operation_mode_t current_operation = FINGER_OP_NONE;
static SemaphoreHandle_t finger_op_mutex = NULL;

// Add these variables at the top of the file with other static variables
static SemaphoreHandle_t finger_detect_mutex = NULL;
static bool finger_detected_flag = false;
static uint32_t last_interrupt_time = 0;
static const uint32_t DEBOUNCE_TIME_MS = 300; // Debounce time in milliseconds

// bool template_available = false;
// uint8_t saved_template_size = 0;
static uint32_t last_match_time = 0;
static bool match_cooldown_active = false;

bool enrollment_in_progress = false;

// Replace the existing finger_detected_isr function with this improved version
void IRAM_ATTR finger_detected_isr(void* arg) {
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Simple debounce - ignore interrupts that come too quickly after the previous one
    if (current_time - last_interrupt_time < DEBOUNCE_TIME_MS) {
        return; // Ignore this interrupt (debounce)
    }
    
    last_interrupt_time = current_time;
    
    // Only send to queue if we're not already processing a fingerprint
    if (!is_fingerprint_validating) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        uint8_t finger_detected = 1;
        xQueueSendFromISR(finger_detected_queue, &finger_detected, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}


void finger_detection_task(void *pvParameter) {
    uint8_t finger_detected;
    esp_err_t err;
    EventBits_t bits;
    uint32_t process_start_time = 0;
    const uint32_t PROCESS_TIMEOUT_MS = 5000; // 5 second timeout
    
    // Create mutex for finger detection
    finger_detect_mutex = xSemaphoreCreateMutex();
    if (finger_detect_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create finger detection mutex");
        return;
    }
    
    ESP_LOGI(TAG, "Finger detection task started");
    
    while (1) {
        // Wait for finger detection signal from ISR
        if (xQueueReceive(finger_detected_queue, &finger_detected, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Finger detected via interrupt!");
            
            // Take mutex to prevent concurrent processing
            if (xSemaphoreTake(finger_detect_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Could not take finger detection mutex, skipping");
                continue;
            }
            
            // Only proceed if we're not already processing a fingerprint
            if (!is_fingerprint_validating) {
                is_fingerprint_validating = true;
                process_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                
                // Get current operation mode with mutex protection
                finger_operation_mode_t current_op = fingerprint_get_operation_mode();
                ESP_LOGI(TAG, "Processing fingerprint in mode: %d", current_op);
                
                // Clear states - but be careful not to flush important data
                xQueueReset(fingerprint_command_queue);
                
                if (enroll_event_group != NULL) {
                    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
                }
                
                // Confirm finger presence with multiple checks
                bool finger_confirmed = false;
                for (int attempt = 0; attempt < 3; attempt++) {
                    // Delay to ensure finger is stable
                    vTaskDelay(pdMS_TO_TICKS(50));
                    
                    // Step 1: Try to capture fingerprint image
                    err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to send GetImage command: %s", esp_err_to_name(err));
                        continue;
                    }
                    
                    // Wait for image capture response with short timeout
                    if (enroll_event_group != NULL) {
                        bits = xEventGroupWaitBits(enroll_event_group,
                                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(800));
                        
                        if (bits & ENROLL_BIT_SUCCESS) {
                            // Image captured successfully, finger is confirmed
                            finger_confirmed = true;
                            ESP_LOGI(TAG, "Finger presence confirmed on attempt %d", attempt + 1);
                            break;
                        } else {
                            ESP_LOGW(TAG, "Finger presence check failed on attempt %d", attempt + 1);
                        }
                    }
                    
                    // Short delay before next attempt
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                if (!finger_confirmed) {
                    ESP_LOGW(TAG, "Could not confirm finger presence after multiple attempts");
                    is_fingerprint_validating = false;
                    xSemaphoreGive(finger_detect_mutex);
                    continue;
                }
                
                // Now that we've confirmed a finger is present, proceed with feature extraction
                ESP_LOGI(TAG, "Image capture successful, processing features");
                
                // Determine which buffer to use based on operation
                uint8_t buffer_id = (current_op == FINGER_OP_ENROLL_SECOND) ? 2 : 1;
                
                // Step 2: Extract features into the appropriate buffer
                FingerprintPacket *gen_char_cmd = (buffer_id == 1) ? &PS_GenChar1 : &PS_GenChar2;
                err = fingerprint_send_command(gen_char_cmd, DEFAULT_FINGERPRINT_ADDRESS);
                
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send GenChar%d command: %s", buffer_id, esp_err_to_name(err));
                    is_fingerprint_validating = false;
                    xSemaphoreGive(finger_detect_mutex);
                    continue;
                }
                
                // Add a small delay to ensure command is processed
                vTaskDelay(pdMS_TO_TICKS(100));
                
                // Wait for feature extraction response with longer timeout
                bits = xEventGroupWaitBits(enroll_event_group,
                                         ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                         pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                
                if (bits & ENROLL_BIT_SUCCESS) {
                    // Feature extraction successful
                    ESP_LOGI(TAG, "Fingerprint features extracted successfully! Status: 0x00");
                    
                    switch (current_op) {
                        case FINGER_OP_VERIFY:
                            ESP_LOGI(TAG, "Verification mode: searching database");
                            uint8_t search_params[] = {0x01, 0x00, 0x00, 0x00, 0x64};
                            fingerprint_set_command(&PS_Search, FINGERPRINT_CMD_SEARCH, search_params, sizeof(search_params));
                            fingerprint_send_command(&PS_Search, DEFAULT_FINGERPRINT_ADDRESS);
                            
                            // Wait for search response with timeout
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            break;
                            
                        case FINGER_OP_ENROLL_FIRST:
                            ESP_LOGI(TAG, "First enrollment image captured successfully");
                            if (enroll_event_group != NULL) {
                                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                            }
                            break;
                            
                        case FINGER_OP_ENROLL_SECOND:
                            ESP_LOGI(TAG, "Second enrollment image captured, creating model");
                            fingerprint_send_command(&PS_RegModel, DEFAULT_FINGERPRINT_ADDRESS);
                            // Wait for model creation response
                            vTaskDelay(pdMS_TO_TICKS(500));
                            break;
                            
                        case FINGER_OP_CUSTOM:
                            ESP_LOGI(TAG, "Custom operation: feature extraction complete");
                            if (enroll_event_group != NULL) {
                                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                            }
                            break;
                            
                        case FINGER_OP_NONE:
                        default:
                            ESP_LOGI(TAG, "Default mode: searching database");
                            uint8_t default_params[] = {0x01, 0x00, 0x00, 0x00, 0x64};
                            fingerprint_set_command(&PS_Search, FINGERPRINT_CMD_SEARCH, default_params, sizeof(default_params));
                            fingerprint_send_command(&PS_Search, DEFAULT_FINGERPRINT_ADDRESS);
                            
                            // Wait for search response with timeout
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            break;
                    }
                } else {
                    ESP_LOGW(TAG, "Feature extraction failed or timed out");
                    // Signal failure
                    if (enroll_event_group != NULL) {
                        xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                    }
                }
                
                // Always reset the validation flag when processing is complete
                is_fingerprint_validating = false;
                ESP_LOGI(TAG, "Fingerprint processing completed");
            } else {
                // Check if processing has been going on too long (stuck case)
                uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (current_time - process_start_time > PROCESS_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "Fingerprint processing timed out - forcing reset");
                    is_fingerprint_validating = false;
                } else {
                    ESP_LOGW(TAG, "Ignoring finger detection - already processing");
                }
            }
            
            // Release mutex
            xSemaphoreGive(finger_detect_mutex);
            
            // Wait a bit before accepting new interrupts to prevent rapid triggering
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void fingerprint_set_pins(int tx, int rx) {
    tx_pin = tx;
    rx_pin = rx;
}
void fingerprint_set_baudrate(int baud) {
    baud_rate = baud;
}

FingerprintPacket PS_HandShake = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_HANDSHAKE, // Handshake
    .parameters = {0}, // No parameters
    .checksum = 0x0039 // Needs to be recalculated
};

FingerprintPacket PS_GetImage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_GET_IMAGE, // Get Image
    .parameters = {0}, // No parameters
    .checksum = 0x0005 // Hardcoded checksum
};

FingerprintPacket PS_GenChar1 = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .code.command = FINGERPRINT_CMD_GEN_CHAR, // Generate Character
    .parameters = {0}, // Buffer ID 1
    .checksum = 0x0008 // Hardcoded checksum
};

FingerprintPacket PS_GenChar2 = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .code.command = FINGERPRINT_CMD_GEN_CHAR, // Generate Character
    .parameters = {0}, // Buffer ID 2
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintPacket PS_RegModel = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_REG_MODEL, // Register Model
    .parameters = {0}, // No parameters
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintPacket PS_Search = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0008,
    .code.command = FINGERPRINT_CMD_SEARCH, // Search
    .parameters = {0}, // Buffer ID, Start Page, Number of Pages
    .checksum = 0x00 // Hardcoded checksum
};

FingerprintPacket PS_Match = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_MATCH, // Match
    .parameters = {0}, // No parameters
    .checksum = 0x0007 // Hardcoded checksum
};

FingerprintPacket PS_StoreChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0006,
    .code.command = FINGERPRINT_CMD_STORE_CHAR, // Store Character
    .parameters = {0}, // Buffer ID, Page ID
    .checksum = 0x000F // Hardcoded checksum
};

FingerprintPacket PS_DeleteChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .code.command = FINGERPRINT_CMD_DELETE_CHAR, // Delete Fingerprint
    .parameters = {0}, // Page ID, Number of Entries
    .checksum = 0x0015 // Hardcoded checksum
};

FingerprintPacket PS_Empty = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_EMPTY_DATABASE, // Clear Database
    .parameters = {0}, // No parameters
    .checksum = 0x0011 // Hardcoded checksum
};

FingerprintPacket PS_ReadSysPara = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_READ_SYS_PARA, // Read System Parameters
    .parameters = {0}, // No parameters
    .checksum = 0x0013 // Hardcoded checksum
};

FingerprintPacket PS_SetChipAddr = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .code.command = FINGERPRINT_CMD_SET_CHIP_ADDR, // Set Address
    .parameters = {0}, // New Address (modifiable)
    .checksum = 0x0020 // Hardcoded checksum
};

FingerprintPacket PS_Cancel = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_CANCEL, // Cancel command
    .parameters = {0}, // No parameters
    .checksum = 0x0033 // Needs to be recalculated
};

FingerprintPacket PS_AutoEnroll = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0008,
    .code.command = FINGERPRINT_CMD_AUTO_ENROLL, // AutoEnroll command
    .parameters = {0}, // ID number, number of entries, parameter
    .checksum = 0x003A // Needs to be recalculated
};

FingerprintPacket PS_Autoldentify = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0006,
    .code.command = FINGERPRINT_CMD_AUTO_IDENTIFY, // AutoIdentify command
    .parameters = {0}, // Score level, ID number
    .checksum = 0x003F // Needs to be recalculated
};

FingerprintPacket PS_GetKeyt = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_GETKEYT, // Get key pair
    .parameters = {0}, // No parameters
    .checksum = 0x00E3 // Needs to be recalculated
};

FingerprintPacket PS_SecurityStoreChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0006,
    .code.command = 0xF2, // Secure Store Template
    .parameters = {0}, // Buffer ID, Page ID
    .checksum = 0x00FB // Needs to be recalculated
};

FingerprintPacket PS_SecuritySearch = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0008,
    .code.command = FINGERPRINT_CMD_SECURITY_SEARCH, // Secure Search
    .parameters = {0}, // Buffer ID, Start Page, Number of Pages
    .checksum = 0x00FD // Needs to be recalculated
};

FingerprintPacket PS_Uplmage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_UPLOAD_IMAGE, // Upload Image
    .parameters = {0}, // No parameters
    .checksum = 0x000D // Needs to be recalculated
};

FingerprintPacket PS_Downlmage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_DOWNLOAD_IMAGE, // Download Image
    .parameters = {0}, // No parameters
    .checksum = 0x000E // Needs to be recalculated
};

FingerprintPacket PS_CheckSensor = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_CHECK_SENSOR, // Check Sensor
    .parameters = {0}, // No parameters
    .checksum = 0x0039 // Needs to be recalculated
};

FingerprintPacket PS_RestSetting = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_FACTORY_RESET, // Restore Factory Settings
    .parameters = {0}, // No parameters
    .checksum = 0x003E // Needs to be recalculated
};

FingerprintPacket PS_ReadINFpage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_READ_INF_PAGE, // Read Flash Information Page
    .parameters = {0}, // No parameters
    .checksum = 0x0019 // Needs to be recalculated
};

FingerprintPacket PS_BurnCode = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .code.command = FINGERPRINT_CMD_BURN_CODE, // Erase Code
    .parameters = {0}, // Default upgrade mode
    .checksum = 0x001F // Needs to be recalculated
};

FingerprintPacket PS_SetPwd = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .code.command = FINGERPRINT_CMD_SET_PASSWORD, // Set Password
    .parameters = {0}, // Password (modifiable)
    .checksum = 0x0019 // Needs to be recalculated
};

FingerprintPacket PS_VfyPwd = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .code.command = FINGERPRINT_CMD_VERIFY_PASSWORD, // Verify Password
    .parameters = {0}, // Password
    .checksum = 0x001A // Needs to be recalculated
};

FingerprintPacket PS_GetRandomCode = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_GET_RANDOM_CODE, // Get Random Number
    .parameters = {0}, // No parameters
    .checksum = 0x0017 // Needs to be recalculated
};

FingerprintPacket PS_WriteNotepad = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0023, // 32 bytes data
    .code.command = FINGERPRINT_CMD_WRITE_NOTEPAD, // Write Notepad
    .parameters = {0}, // Data to write (to be filled)
    .checksum = 0x003B // Needs to be recalculated
};

FingerprintPacket PS_ReadNotepad = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .code.command = FINGERPRINT_CMD_READ_NOTEPAD, // Read Notepad
    .parameters = {0}, // Page number
    .checksum = 0x001E // Needs to be recalculated
};

FingerprintPacket PS_ControlBLN = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .code.command = FINGERPRINT_CMD_CONTROL_LED, // Control LED
    .parameters = {0}, // Example parameters: function, start color, end color, cycles
    .checksum = 0x0046 // Needs to be recalculated
};

FingerprintPacket PS_GetImageInfo = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_GET_IMAGE_INFO, // Get Image Information
    .parameters = {0}, // No parameters
    .checksum = 0x0041 // Needs to be recalculated
};

FingerprintPacket PS_SearchNow = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .code.command = FINGERPRINT_CMD_SEARCH_NOW, // Search Now
    .parameters = {0}, // Start Page, Number of Pages
    .checksum = 0x0046 // Needs to be recalculated
};

FingerprintPacket PS_ValidTempleteNum = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_VALID_TEMPLATE_NUM, // Get number of valid templates
    .parameters = {0}, // No parameters
    .checksum = 0x0021 // Needs to be recalculated
};

FingerprintPacket PS_Sleep = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_SLEEP, // Enter sleep mode
    .parameters = {0}, // No parameters
    .checksum = 0x0037 // Needs to be recalculated
};

FingerprintPacket PS_LockKeyt = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_LOCKEYT, // Lock Key Pair
    .parameters = {0}, // No parameters
    .checksum = 0x00E4 // Needs to be recalculated
};

FingerprintPacket PS_GetCiphertext = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_GET_CIPHER_TEXT, // Get Ciphertext Random Number
    .parameters = {0}, // No parameters
    .checksum = 0x00E5 // Needs to be recalculated
};

FingerprintPacket PS_GetChipSN = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_GETCHIP_SN, // Get Chip Serial Number
    .parameters = {0}, // No parameters
    .checksum = 0x0016 // Needs to be recalculated
};

FingerprintPacket PS_GetEnrollImage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_GET_ENROLL_IMAGE, // Register Get Image
    .parameters = {0}, // No parameters
    .checksum = 0x002D // Needs to be recalculated
};

FingerprintPacket PS_WriteReg = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0005,
    .code.command = FINGERPRINT_CMD_WRITE_REG, // Write System Register
    .parameters = {0}, // Register Number, Value (modifiable)
    .checksum = 0x0013 // Needs to be recalculated
};

FingerprintPacket PS_ReadIndexTable = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .code.command = FINGERPRINT_CMD_READ_INDEX_TABLE, // Read Index Table
    .parameters = {0}, // Page Number
    .checksum = 0x0023 // Needs to be recalculated
};

FingerprintPacket PS_UpChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .code.command = FINGERPRINT_CMD_UP_CHAR, // Upload template from buffer
    .parameters = {0}, // BufferID
    .checksum = 0x000D // Needs to be recalculated
};

FingerprintPacket PS_DownChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .code.command = FINGERPRINT_CMD_DOWN_CHAR, // Download template to buffer
    .parameters = {0}, // BufferID
    .checksum = 0x000E // Needs to be recalculated
};

FingerprintPacket PS_LoadChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0006,
    .code.command = FINGERPRINT_CMD_LOAD_CHAR,
    .parameters = {0},  // Buffer ID, PageID
    .checksum = 0x000C
};

FingerprintPacket PS_ReadINFPage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .code.command = FINGERPRINT_CMD_READ_INF_PAGE,  // Read Information Page Command
    .parameters = {0},      // No additional parameters required
    .checksum = 0x001A      // Placeholder checksum (should be computed dynamically)
};


esp_err_t check_duplicate_fingerprint(void) {
    esp_err_t err;
    // EventBits_t bits;

    uint8_t search_params[] = {
        0x01,         // BufferID = 1
        0x00, 0x00,   // Start page = 0
        0x00, 0x64    // Number of pages = 100
    };

    fingerprint_set_command(&PS_Search, FINGERPRINT_CMD_SEARCH, search_params, sizeof(search_params));
    err = fingerprint_send_command(&PS_Search, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t validate_template_location(uint16_t location) {
    esp_err_t err;
    // EventBits_t bits;

    uint8_t index_params[] = {(uint8_t)(location >> 8)};
    fingerprint_set_command(&PS_ReadIndexTable, FINGERPRINT_CMD_READ_INDEX_TABLE, 
                          index_params, sizeof(index_params));
    
    err = fingerprint_send_command(&PS_ReadIndexTable, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read index table");
        return ESP_FAIL;
    }

    return ESP_OK;
}

 /**
 * @brief Returns the smaller of two unsigned integers.
 *
 * @param a First number
 * @param b Second number
 * @return The minimum of the two numbers
 */
static inline uint16_t min(uint16_t a, uint16_t b) {
    return (a < b) ? a : b;
}

/**
 * @brief Returns the larger of two unsigned integers.
 *
 * @param a First number
 * @param b Second number
 * @return The maximum of the two numbers
 */
static inline uint16_t max(uint16_t a, uint16_t b) {
    return (a > b) ? a : b;
}

esp_err_t fingerprint_init(void) {
    ESP_LOGI(TAG, "Initializing fingerprint scanner...");
    esp_err_t err;

    // // FIRST STEP: Power up the fingerprint module
    // err = fingerprint_power_control(true);
    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to power up fingerprint module");
    //     return err;
    // }
    
    // 1. INITIALIZE FLAGS FIRST - CRITICAL
    is_fingerprint_validating = false;
    last_interrupt_time = 0;
    
    // 2. UART CONFIGURATION FIRST
    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    
    // First configure UART
    err = uart_param_config(UART_NUM, &uart_config);
    if (err != ESP_OK) return err;
    
    // Set pins
    err = uart_set_pin(UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    
    // Install driver
    err = uart_driver_install(UART_NUM, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    
    // 3. CREATE QUEUES BEFORE STARTING ANY TASKS
    fingerprint_command_queue = xQueueCreate(QUEUE_SIZE, sizeof(fingerprint_command_info_t));
    fingerprint_response_queue = xQueueCreate(QUEUE_SIZE, sizeof(fingerprint_response_t));
    finger_detected_queue = xQueueCreate(10, sizeof(uint8_t));
    
    if (!fingerprint_command_queue || !fingerprint_response_queue || !finger_detected_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        return ESP_FAIL;
    }
    
// // 4. HANDSHAKE FIRST - BEFORE ANY INTERRUPTS OR TASKS
// uint8_t handshake;
// int length;
// bool handshake_received = false;
// const int max_retries = 10;  // Set a maximum retry count
// int retry = 0;
// int power_cycles = 0;
// const int max_power_cycles = 3;  // Maximum number of full power cycles to try

// // First, do a complete power cycle OUTSIDE the retry loop
// ESP_LOGI(TAG, "Initial power cycle of fingerprint module...");



// err = fingerprint_power_control(true);   // ON - 800ms delay built in
// if (err != ESP_OK) {
//     ESP_LOGE(TAG, "Failed to power up fingerprint module");
//     return err;
// }

// // Power off then on to ensure clean start
// err = fingerprint_power_control(false);  // OFF - 500ms delay built in
// if (err != ESP_OK) {
//     ESP_LOGE(TAG, "Failed to power down fingerprint module");
//     return err;
// }

// // Extra stabilization time after power-on
// vTaskDelay(pdMS_TO_TICKS(500));

// // Flush UART buffer to clear any garbage data
// uart_flush(UART_NUM);
// vTaskDelay(pdMS_TO_TICKS(300));

// ESP_LOGI(TAG, "Waiting for fingerprint module handshake...");

// // Attempt to receive handshake with multiple retries
// while (retry < max_retries && !handshake_received) {
//     // Try to read handshake byte
//     length = uart_read_bytes(UART_NUM, &handshake, 1, pdMS_TO_TICKS(300));
    
//     if (length > 0) {
//         ESP_LOGI(TAG, "Received byte: 0x%02X", handshake);
        
//         if (handshake == 0x55) {
//             handshake_received = true;
//             ESP_LOGI(TAG, "Fingerprint module handshake received: 0x%02X", handshake);
//             break;
//         }
//     } else {
//         ESP_LOGW(TAG, "No handshake received, retry %d/%d...", retry + 1, max_retries);
//         retry++;
        
//         // Power cycle again only after several failed attempts
//         if (retry % 5 == 0 && power_cycles < max_power_cycles) {
//             power_cycles++;
//             ESP_LOGI(TAG, "Performing power cycle %d/%d...", power_cycles, max_power_cycles);
            
//             // Power cycle the module
//             err = fingerprint_power_control(false);
//             if (err != ESP_OK) {
//                 ESP_LOGE(TAG, "Failed to power down fingerprint module");
//                 return err;
//             }
            
//             // Longer off period to ensure complete discharge
//             vTaskDelay(pdMS_TO_TICKS(800));
            
//             err = fingerprint_power_control(true);
//             if (err != ESP_OK) {
//                 ESP_LOGE(TAG, "Failed to power up fingerprint module");
//                 return err;
//             }
            
//             // Extra delay for stabilization after power cycle
//             vTaskDelay(pdMS_TO_TICKS(500));
            
//             // Flush UART buffer again after power cycle
//             uart_flush(UART_NUM);
//         }
        
//         // Short delay between retry attempts
//         vTaskDelay(pdMS_TO_TICKS(200));
//     }
// }

// if (!handshake_received) {
//     ESP_LOGE(TAG, "Failed to receive handshake after %d attempts and %d power cycles", 
//              retry, power_cycles);
//     return ESP_ERR_TIMEOUT;
// }
    
    // 5. CREATE RESPONSE TASKS ONLY AFTER HANDSHAKE
    if (xTaskCreate(read_response_task, "FingerprintReadResponse", 8192, NULL, 
                   configMAX_PRIORITIES - 2, &fingerprint_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create read response task");
        return ESP_FAIL;
    }
    
    if (xTaskCreate(process_response_task, "FingerprintProcessResponse", 4096, NULL, 
                   configMAX_PRIORITIES - 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create process response task");
        return ESP_FAIL;
    }
    
    // 6. INITIALIZE COMMANDS
    uint8_t buffer_id1 = 0x01;
    uint8_t buffer_id2 = 0x02;
    fingerprint_set_command(&PS_GenChar1, FINGERPRINT_CMD_GEN_CHAR, &buffer_id1, 1);
    fingerprint_set_command(&PS_GenChar2, FINGERPRINT_CMD_GEN_CHAR, &buffer_id2, 1);
    
    // 7. ONLY NOW SETUP GPIO AND INTERRUPTS
    esp_err_t gpio_ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (gpio_ret != ESP_OK && gpio_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(gpio_ret));
        return gpio_ret;
    }
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FINGERPRINT_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    
    err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;
    
    err = gpio_isr_handler_add(FINGERPRINT_GPIO_PIN, finger_detected_isr, NULL);
    if (err != ESP_OK) return err;
    
    // 8. FINALLY CREATE FINGER DETECTION TASK
    if (xTaskCreate(finger_detection_task, "FingerDetectionTask", 4096, NULL, 
                  configMAX_PRIORITIES - 1, &finger_detection_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create finger detection task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Fingerprint scanner initialized successfully with interrupt-based detection.");
    return ESP_OK;
}



esp_err_t fingerprint_set_command(FingerprintPacket *cmd, uint8_t command, uint8_t *params, uint8_t param_length) {
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;  // Null pointer error
    }

    if (param_length > MAX_PARAMETERS) {  // Allow up to MAX_PARAMETERS (32 bytes) for commands like PS_WriteNotepad
        return ESP_ERR_INVALID_SIZE;
    }

    cmd->header = FINGERPRINT_HEADER;
    cmd->address = DEFAULT_FINGERPRINT_ADDRESS;
    cmd->packet_id = FINGERPRINT_PACKET_ID_CMD;  // Command packet
    cmd->length = 1 + param_length + 2;  // Corrected length calculation
    cmd->code.command = command;

    // Clear parameters and copy only valid ones
    memset(cmd->parameters, 0, sizeof(cmd->parameters));
    if (params != NULL && param_length > 0) {
        memcpy(cmd->parameters, params, param_length);
    }

    // Compute checksum
    cmd->checksum = fingerprint_calculate_checksum(cmd);

    return ESP_OK;
}

uint16_t fingerprint_calculate_checksum(FingerprintPacket *cmd) {
    uint16_t sum = 0;
    // // Include address bytes in checksum calculation
    // sum += (cmd->address >> 24) & 0xFF;
    // sum += (cmd->address >> 16) & 0xFF;
    // sum += (cmd->address >> 8) & 0xFF;
    // sum += cmd->address & 0xFF;

    sum += cmd->packet_id;
    sum += (cmd->length >> 8) & 0xFF; // High byte of length
    sum += cmd->length & 0xFF;        // Low byte of length
    sum += cmd->code.command;
    // Dynamically sum all parameters based on packet length
    for (int i = 0; i < cmd->length - 3; i++) {  // Exclude command + checksum
        if(cmd->parameters[i] != 0){
            sum += cmd->parameters[i];
        } // Exclude zero bytes
    }

    return sum;
}


esp_err_t fingerprint_send_command(FingerprintPacket *cmd, uint32_t address) {
    // // Add this logging at the beginning
    // ESP_LOGI(TAG, "Sending command 0x%02X with ID=0x%02X, Len=%d", 
    //     cmd->code.command, cmd->packet_id, cmd->length);

    // uint16_t p_size = cmd->length + 9;  // Header + Addr + Packet ID + Length + Command + Checksum
    // ESP_LOGI(TAG, "Full Command Packet Size: %d bytes", p_size);
    // ESP_LOG_BUFFER_HEX(TAG, (uint8_t*)cmd, p_size);

   
//    // Log full packet details for debugging
//    if (cmd->code.command == FINGERPRINT_CMD_DOWN_CHAR) {
//        ESP_LOGI(TAG, "DOWNCHAR COMMAND DETAILS: ID=0x%02X, Addr=0x%08"PRIX32", Len=%d", 
//         cmd->packet_id, cmd->address, cmd->length);
//        ESP_LOG_BUFFER_HEX(TAG, cmd, cmd->length + 8); // Log entire packet
//    }
//    ESP_LOG_BUFFER_HEX("Command Packets", cmd, cmd->length + 8); // Log entire packet
    if (!cmd) return ESP_ERR_INVALID_ARG;  
    last_sent_command = cmd->code.command;  // Store last sent command
    cmd->address = address;
    cmd->checksum = fingerprint_calculate_checksum(cmd);

    // Store command info **before** sending
    fingerprint_command_info_t cmd_info = {
        .command = cmd->code.command,
        .timestamp = xTaskGetTickCount()
    };

    // **Ensure queue is not full before sending**
    if (xQueueSend(fingerprint_command_queue, &cmd_info, pdMS_TO_TICKS(100)) == pdPASS) {
        // ESP_LOGI(TAG, "Stored command 0x%02X in queue successfully.", cmd->code.command);
    } else {
        ESP_LOGE(TAG, "Command queue full, dropping command 0x%02X", cmd->code.command);
        return ESP_FAIL;
    }
    

    // **Construct packet**
    size_t packet_size = cmd->length + 9;
    uint8_t *buffer = malloc(packet_size);
    if (!buffer) return ESP_ERR_NO_MEM;

    buffer[0] = (cmd->header >> 8) & 0xFF;
    buffer[1] = cmd->header & 0xFF;
    buffer[2] = (cmd->address >> 24) & 0xFF;
    buffer[3] = (cmd->address >> 16) & 0xFF;
    buffer[4] = (cmd->address >> 8) & 0xFF;
    buffer[5] = cmd->address & 0xFF;
    buffer[6] = cmd->packet_id;
    buffer[7] = (cmd->length >> 8) & 0xFF;
    buffer[8] = cmd->length & 0xFF;
    buffer[9] = cmd->code.command;
    memcpy(&buffer[10], cmd->parameters, cmd->length - 3);
    buffer[packet_size - 2] = (cmd->checksum >> 8) & 0xFF;
    buffer[packet_size - 1] = cmd->checksum & 0xFF;

    // **Flush UART to prevent old data interference**
    uart_flush(UART_NUM);

    // **Send packet**
    int bytes_written = uart_write_bytes(UART_NUM, (const char *)buffer, packet_size);


    if (bytes_written != packet_size) {
        ESP_LOGE(TAG, "Failed to send the complete fingerprint command.");
        return ESP_FAIL;
    }

    // ESP_LOGI(TAG, "Sent fingerprint command: 0x%02X to address 0x%08X", cmd->code.command, (unsigned int)address);
    // ESP_LOGI(TAG, "Sent fingerprint command: 0x%02X to address 0x%08X", buffer[9], (unsigned int)address);
    // ESP_LOG_BUFFER_HEX("Fingerprint sent command: ", buffer, packet_size);
    free(buffer);

    return ESP_OK;
}


// FingerprintPacket* fingerprint_read_response(void) {
//     uint8_t buffer[512];  // Increased buffer size for template data packets
//     int length = uart_read_bytes(UART_NUM, buffer, sizeof(buffer), 200 / portTICK_PERIOD_MS);

//     if (length <= 0) {
//         return NULL;
//     }

//     // Find start of packet
//     int offset = 0;
//     while (offset < length - 1) {
//         if (buffer[offset] == 0xEF && buffer[offset + 1] == 0x01) {
//             break;
//         }
//         offset++;
//     }

//     if (offset >= length - 9) {  // Not enough bytes for header
//         return NULL;
//     }

//     // Allocate memory for the packet
//     FingerprintPacket *packet = (FingerprintPacket*)heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
//     if (!packet) {
//         ESP_LOGE(TAG, "Memory allocation failed!");
//         return NULL;
//     }
//     memset(packet, 0, sizeof(FingerprintPacket));

//     // Extract basic header information
//     packet->header = (buffer[offset] << 8) | buffer[offset + 1];
//     packet->address = (buffer[offset + 2] << 24) | (buffer[offset + 3] << 16) |
//                      (buffer[offset + 4] << 8) | buffer[offset + 5];
//     packet->packet_id = buffer[offset + 6];
//     packet->length = (buffer[offset + 7] << 8) | buffer[offset + 8];

//     // Calculate total expected packet length
//     uint16_t expected_length = packet->length + 9;  // header(2) + addr(4) + pid(1) + len(2) + data + chk(2)
    
//     if (offset + expected_length > length) {
//         // For data packets, allow partial reads
//         if (packet->packet_id == 0x02 || packet->packet_id == 0x08) {
//             expected_length = length - offset;
//             packet->length = expected_length - 9;
//         } else {
//             ESP_LOGE(TAG, "Incomplete packet! Expected: %d, Available: %d",
//                      expected_length, length - offset);
//             heap_caps_free(packet);
//             return NULL;
//         }
//     }

//     // Handle different packet types
//     if (packet->packet_id == 0x02 || packet->packet_id == 0x08) {
//         // Data packet (template data)
//         if (last_sent_command == FINGERPRINT_CMD_UP_CHAR && 
//             packet->length <= sizeof(packet->parameters)) {
//             memcpy(packet->parameters, &buffer[offset + 9], packet->length);
//             packet->code.confirmation = 0x00;  // Data packets don't use command byte
//         }
//     } else {
//         // Confirmation response packet
//         packet->code.confirmation = buffer[offset + 9];
//         if (packet->length > 3 && (packet->length - 3) <= sizeof(packet->parameters)) {
//             memcpy(packet->parameters, &buffer[offset + 10], packet->length - 3);
//         }
//     }

//     // Calculate checksum from available data
//     packet->checksum = (buffer[offset + expected_length - 2] << 8) | 
//                        buffer[offset + expected_length - 1];

//     return packet;
// }

// FingerprintPacket* fingerprint_read_response(void) {
//     uint8_t buffer[512];  // Buffer for template data
//     int length = uart_read_bytes(UART_NUM, buffer, sizeof(buffer), 200 / portTICK_PERIOD_MS);
//     // ESP_LOG_BUFFER_HEXDUMP("Fingerprint Response", buffer, length, ESP_LOG_INFO);
//     if (length <= 0) return NULL;

//     // Find packet header
//     int offset = 0;
//     while (offset < length - 1) {
//         if (buffer[offset] == 0xEF && buffer[offset + 1] == 0x01) break;
//         offset++;
//     }

//     if (offset >= length - 9) return NULL;

//     FingerprintPacket *packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
//     if (!packet) {
//         ESP_LOGE(TAG, "Memory allocation failed!");
//         return NULL;
//     }
//     memset(packet, 0, sizeof(FingerprintPacket));

//     // Parse header info
//     packet->header = (buffer[offset] << 8) | buffer[offset + 1];
//     packet->address = (buffer[offset + 2] << 24) | (buffer[offset + 3] << 16) |
//                      (buffer[offset + 4] << 8) | buffer[offset + 5];
//     packet->packet_id = buffer[offset + 6];
//     packet->length = (buffer[offset + 7] << 8) | buffer[offset + 8];

//     // Key fix: Handle data packets correctly
//     if (packet->packet_id == 0x02 || packet->packet_id == 0x08) {  // Data packet
//         // For data packets, payload starts at offset+9
//         size_t data_length = length - (offset + 9) - 2;  // -2 for checksum
//         if (data_length > 0 && data_length <= sizeof(packet->parameters)) {
//             memcpy(packet->parameters, &buffer[offset + 9], data_length);
//             ESP_LOGI(TAG, "Copying %d bytes of template data", data_length);
//             ESP_LOG_BUFFER_HEX("Template Data", packet->parameters, data_length);
//         }
//     } else {  // Command response packet
//         packet->code.confirmation = buffer[offset + 9];
//         if (packet->length > 3) {
//             size_t param_length = packet->length - 3;
//             if (param_length <= sizeof(packet->parameters)) {
//                 memcpy(packet->parameters, &buffer[offset + 10], param_length);
//             }
//         }
//     }

//     // Get checksum from end of packet
//     packet->checksum = (buffer[offset + length - 2] << 8) | buffer[offset + length - 1];

//     return packet;
// }

// Add these function implementations right before the fingerprint_init function

/**
 * @brief Register a callback function to handle fingerprint events.
 *
 * This function sets the global event handler function pointer to the provided
 * callback function, allowing it to be called when fingerprint events occur.
 *
 * @param handler Function pointer to the event handler callback
 */
void register_fingerprint_event_handler(fingerprint_event_handler_t handler) {
    g_fingerprint_event_handler = handler;
    ESP_LOGI(TAG, "Fingerprint event handler registered");
}

/**
 * @brief Trigger a fingerprint event and call the registered event handler.
 *
 * This function is called internally when a fingerprint event occurs.
 * It checks if an event handler is registered and calls it with the event data.
 *
 * @param event The fingerprint event to trigger
 */
void trigger_fingerprint_event(fingerprint_event_t event) {
    if (g_fingerprint_event_handler != NULL) {
        // Create a copy of the event to avoid any data races
        fingerprint_event_t event_copy = event;
        
        // Handle deep copy of template data if needed
        if (event.type == EVENT_TEMPLATE_UPLOADED && 
            event.data.template_data.data != NULL &&
            event.data.template_data.size > 0) {
            
            // Make a deep copy of template data
            uint8_t* template_copy = heap_caps_malloc(event.data.template_data.size, MALLOC_CAP_8BIT);
            if (template_copy) {
                memcpy(template_copy, event.data.template_data.data, event.data.template_data.size);
                event_copy.data.template_data.data = template_copy;
                // Other fields are already copied
            } else {
                // If allocation fails, set data to NULL to prevent accessing invalid memory
                event_copy.data.template_data.data = NULL;
                event_copy.data.template_data.size = 0;
                event_copy.data.template_data.is_complete = false;
                ESP_LOGE(TAG, "Failed to allocate memory for template data copy");
            }
        }
        
        // Call the handler with the event copy
        g_fingerprint_event_handler(event_copy);
        
        // Free any allocated memory
        if (event.type == EVENT_TEMPLATE_UPLOADED && 
            event_copy.data.template_data.data != NULL &&
            event_copy.data.template_data.data != event.data.template_data.data) {
            heap_caps_free(event_copy.data.template_data.data);
        }
    } else {
        ESP_LOGW(TAG, "No fingerprint event handler registered");
    }
}


MultiPacketResponse* fingerprint_read_response(void) {
    static ParserState state = WAIT_HEADER;
    static size_t content_length = 0;
    static size_t bytes_needed = 0;
    static FingerprintPacket current_packet = {0};
    static uint8_t buffer[256] = {0};  // Buffer size of 256 works well (prevents multiple FOOF detections)
    static size_t buffer_pos = 0;
    
    // Track template processing state globally within the function
    static bool template_processed = false;
    static bool final_packet_sent = false;
    static uint32_t last_template_time = 0;
    
    // Track buffer change detection to avoid stuck buffers
    static uint32_t last_buffer_change_time = 0;
    static size_t last_buffer_size = 0;
    
    // Special handling for template upload
    bool is_template_upload = (last_sent_command == FINGERPRINT_CMD_UP_CHAR);
    int timeout = is_template_upload ? 1500 : 200;
    
    // Reset template flags after timeout
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Detect and handle stuck buffer condition
    if (buffer_pos > 0 && buffer_pos <= 2 && buffer_pos == last_buffer_size && 
        (current_time - last_buffer_change_time > 2000)) {
        ESP_LOGW(TAG, "Clearing stuck buffer with %d bytes", buffer_pos);
        buffer_pos = 0;
        state = WAIT_HEADER;
    }
    
    // Update buffer tracking
    if (buffer_pos != last_buffer_size) {
        last_buffer_size = buffer_pos;
        last_buffer_change_time = current_time;
    }
    
    if (template_processed && (current_time - last_template_time > 5000)) {
        template_processed = false;
        final_packet_sent = false;
        ESP_LOGD(TAG, "Template tracking reset after timeout");
    }
    
    int bytes_read = uart_read_bytes(UART_NUM, 
                                   buffer + buffer_pos, 
                                   sizeof(buffer) - buffer_pos, 
                                   timeout / portTICK_PERIOD_MS);

    // ESP_LOG_BUFFER_HEXDUMP("Fingerprint Response", buffer, buffer_pos + bytes_read, ESP_LOG_INFO);
    // Only log the raw buffer once before processing
    // if (bytes_read > 0) {
    //     ESP_LOGI(TAG, "Read %d bytes", bytes_read);
    //     // Use EARLY_LEVEL to minimize impact
    //     ESP_LOG_BUFFER_HEX_LEVEL("Raw data", buffer, buffer_pos, ESP_LOG_INFO);
    // }
    
    if (bytes_read <= 0 && buffer_pos == 0) return NULL;
    buffer_pos += (bytes_read > 0) ? bytes_read : 0;
    
    // Add a small consistent delay instead of conditional logging
    // This ensures timing is consistent whether debug is enabled or not
    if (bytes_read > 0) {
        // Small fixed delay to maintain consistent timing
        vTaskDelay(pdMS_TO_TICKS(1));
        
        // Only log at debug level to avoid affecting timing in production
        ESP_LOGD(TAG, "Read %d bytes, buffer now contains %d bytes", bytes_read, buffer_pos);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, min(buffer_pos, 256), ESP_LOG_DEBUG);
    }
    
    // Special bulk processing for template upload data
    if (is_template_upload && buffer_pos > 9) {
        // Only process the template once
        if (!template_processed && buffer_pos > 100) {  // Ensure we have substantial data
            bool found_end_marker = false;
            bool found_natural_final = false;  // Track if we found a natural 0x08 packet
            
            // First check if there's already a natural final packet (0x08) in the data
            for (size_t i = 0; i < buffer_pos - 9; i++) {
                if (buffer[i] == 0xEF && buffer[i+1] == 0x01 && 
                    buffer[i+6] == 0x08) {  // Packet ID 0x08
                    found_natural_final = true;
                    ESP_LOGI(TAG, "Found natural final packet (0x08) at position %d", i);
                    break;
                }
            }
            
            // Check for "FOOF" sequence near the end of the data
            size_t search_start = buffer_pos > 20 ? buffer_pos - 20 : 0;
            for (size_t i = search_start; i < buffer_pos - 4; i++) {
                if (buffer[i] == 'F' && buffer[i+1] == 'O' && buffer[i+2] == 'O' && buffer[i+3] == 'F') {
                    found_end_marker = true;
                    ESP_LOGI(TAG, "Found FOOF end marker at position %d", i);
                    break;
                }
            }
            
            // Create a bulk response if we have sufficient data
            if (found_end_marker || buffer_pos > 400) {
                template_processed = true;
                last_template_time = current_time;
                
                // Create response with the data
                MultiPacketResponse *response = heap_caps_malloc(sizeof(MultiPacketResponse), MALLOC_CAP_8BIT);
                if (!response) return NULL;
                
                // Initialize the enhanced structure fields properly
                response->packets = heap_caps_malloc(sizeof(FingerprintPacket*) * 2, MALLOC_CAP_8BIT);
                if (!response->packets) {
                    heap_caps_free(response);
                    return NULL;
                }
                response->count = 0;
                response->collecting_template = true;
                response->template_complete = found_end_marker || found_natural_final;
                response->start_time = current_time;
                
                // Allocate and store template data
                response->template_data = heap_caps_malloc(buffer_pos, MALLOC_CAP_8BIT);
                if (response->template_data) {
                    memcpy(response->template_data, buffer, buffer_pos);
                    response->template_size = buffer_pos;
                    response->template_capacity = buffer_pos;
                    ESP_LOGI(TAG, "Copied %d bytes to template buffer", buffer_pos);
                } else {
                    response->template_data = NULL;
                    response->template_size = 0;
                    response->template_capacity = 0;
                }
                
                // Create data packet with the template data
                FingerprintPacket *data_packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                if (data_packet) {
                    memset(data_packet, 0, sizeof(FingerprintPacket));
                    data_packet->header = 0xEF01;
                    data_packet->address = 0xFFFFFFFF;
                    data_packet->packet_id = 0x02;  // Template data packet
                    
                    // Copy template data to parameters (limit to reasonable size)
                    size_t data_size = min(buffer_pos, sizeof(data_packet->parameters));
                    memcpy(data_packet->parameters, buffer, data_size);
                    data_packet->length = data_size;
                    
                    response->packets[response->count++] = data_packet;
                    ESP_LOGD(TAG, "Created bulk template data packet with %d bytes", data_size);
                    
                    // Add the final 0x08 packet only if needed
                    if (!final_packet_sent && !found_natural_final) {
                        FingerprintPacket *final_packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                        if (final_packet) {
                            memset(final_packet, 0, sizeof(FingerprintPacket));
                            final_packet->header = 0xEF01;
                            final_packet->address = 0xFFFFFFFF;
                            final_packet->packet_id = 0x08;  // Final template packet
                            final_packet->length = 8;  // Standard length for final packet
                            
                            response->packets[response->count++] = final_packet;
                            ESP_LOGD(TAG, "Added final packet marker (0x08) to response");
                            final_packet_sent = true;
                        }
                    } else {
                        ESP_LOGD(TAG, "Skipped adding final packet - already sent or found in data");
                    }
                    
                    // Reset buffer and static flag for next template
                    vTaskDelay(pdMS_TO_TICKS(10));  // Short delay
                    buffer_pos = 0;
                    
                    return response;
                }
            }
        }
    }
    
    // Normal packet processing for standard packets
    MultiPacketResponse *response = heap_caps_malloc(sizeof(MultiPacketResponse), MALLOC_CAP_8BIT);
    if (!response) return NULL;
    
    response->packets = heap_caps_malloc(sizeof(FingerprintPacket*) * 4, MALLOC_CAP_8BIT);
    if (!response->packets) {
        heap_caps_free(response);
        return NULL;
    }
    response->count = 0;
    
    // Initialize the enhanced structure fields for non-template packets
    response->collecting_template = false;
    response->template_complete = false;
    response->template_data = NULL;
    response->template_size = 0;
    response->template_capacity = 0;
    response->start_time = current_time;
    
    // Process buffer
    size_t i = 0;
    while (i < buffer_pos) {
        switch (state) {
            case WAIT_HEADER:
                // Look for EF 01 header
                if (buffer_pos - i >= 2) {
                    if (buffer[i] == 0xEF && buffer[i+1] == 0x01) {
                        ESP_LOGD(TAG, "Found header (0xEF01) at position %d", i);
                        memset(&current_packet, 0, sizeof(FingerprintPacket));
                        current_packet.header = 0xEF01;
                        state = READ_ADDRESS;
                        i += 2;
                        bytes_needed = 4; // Need 4 address bytes
                    } else {
                        i++;
                    }
                } else {
                    // Not enough bytes to check for header
                    goto buffer_shift;
                }
                break;
                
            case READ_ADDRESS:
                if (buffer_pos - i >= bytes_needed) {
                    current_packet.address = (buffer[i] << 24) | (buffer[i+1] << 16) |
                                           (buffer[i+2] << 8) | buffer[i+3];
                    ESP_LOGD(TAG, "Read address: 0x%08" PRIX32, current_packet.address);
                    i += 4;
                    state = READ_PACKET_ID;
                    bytes_needed = 1; // Need 1 packet ID byte
                } else {
                    // Not enough data yet
                    goto buffer_shift;
                }
                break;
                
            case READ_PACKET_ID:
                if (buffer_pos - i >= bytes_needed) {
                    current_packet.packet_id = buffer[i];
                    ESP_LOGD(TAG, "Read packet_id: 0x%02X", current_packet.packet_id);
                    i++;
                    state = READ_LENGTH;
                    bytes_needed = 2; // Need 2 length bytes
                } else {
                    goto buffer_shift;
                }
                break;
                
            case READ_LENGTH:
                if (buffer_pos - i >= bytes_needed) {
                    current_packet.length = (buffer[i] << 8) | buffer[i+1];
                    content_length = current_packet.length;
                    ESP_LOGD(TAG, "Read length: %d", content_length);
                    i += 2;
                    state = READ_CONTENT;
                    bytes_needed = content_length; // Need content_length bytes
                } else {
                    goto buffer_shift;
                }
                break;
                
            case READ_CONTENT:
                if (buffer_pos - i >= bytes_needed) {
                    if (current_packet.packet_id == 0x02 || current_packet.packet_id == 0x08) {
                        // Template data packet
                        size_t data_length = content_length - 2; // -2 for checksum
                        if (data_length <= sizeof(current_packet.parameters)) {
                            memcpy(current_packet.parameters, &buffer[i], data_length);
                            
                            // Special log for final packet (0x08)
                            if (current_packet.packet_id == 0x08) {
                                ESP_LOGI(TAG, "Received FINAL template packet (ID=0x08), Length=%d", data_length);
                            } else if (is_template_upload) {
                                // For template data, only log occasionally to reduce clutter
                                static int packet_count = 0;
                                if (packet_count++ % 10 == 0) {
                                    ESP_LOGI(TAG, "Received template data packet %d", packet_count);
                                }
                            }
                        }
                    } else {
                        // Command response packet
                        current_packet.code.confirmation = buffer[i];
                        if (content_length > 3) {
                            size_t param_length = content_length - 3;
                            if (param_length <= sizeof(current_packet.parameters)) {
                                memcpy(current_packet.parameters, &buffer[i+1], param_length);
                            }
                        }
                    }
                    i += content_length - 2; // Move past content but not checksum
                    state = READ_CHECKSUM;
                    bytes_needed = 2;
                } else {
                    // Not enough bytes for complete packet
                    // Special case handling for template data that may not fit the expected length
                    if (current_packet.packet_id == 0x02 && is_template_upload) {
                        // For template data, use what we have
                        size_t available_data = buffer_pos - i;
                        if (available_data > 0) {
                            ESP_LOGD(TAG, "Processing partial template data (%d bytes available)", available_data);
                            
                            // Copy the available data
                            if (available_data <= sizeof(current_packet.parameters)) {
                                memcpy(current_packet.parameters, &buffer[i], available_data);
                                current_packet.length = available_data + 2; // +2 for checksum
                                
                                // Add this packet to the response
                                FingerprintPacket *new_packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                                if (new_packet) {
                                    memcpy(new_packet, &current_packet, sizeof(FingerprintPacket));
                                    response->packets[response->count++] = new_packet;
                                    ESP_LOGD(TAG, "Added partial template data packet to response");
                                }
                                
                                // Reset buffer and state
                                buffer_pos = 0;
                                state = WAIT_HEADER;
                                return response;
                            }
                        }
                    } else {
                        // Check if we've waited too long without progress
                        if ((current_time - last_buffer_change_time) > 1000) {
                            ESP_LOGW(TAG, "Timeout waiting for more data, resetting state");
                            state = WAIT_HEADER;
                            i = buffer_pos; // Force buffer reset
                        }
                    }
                    
                    // Standard non-template case: wait for more data
                    goto buffer_shift;
                }
                break;
                
            case READ_CHECKSUM:
                if (buffer_pos - i >= bytes_needed) {
                    current_packet.checksum = (buffer[i] << 8) | buffer[i+1];
                    ESP_LOGD(TAG, "Read checksum: 0x%04X", current_packet.checksum);
                    i += 2;
                    
                    // Add packet to response
                    FingerprintPacket *new_packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                    if (new_packet) {
                        memcpy(new_packet, &current_packet, sizeof(FingerprintPacket));
                        
                        // Check if we need to resize the packets array
                        if (response->count >= 4) {
                            size_t new_size = response->count * 2;
                            FingerprintPacket **new_packets = heap_caps_realloc(
                                response->packets,
                                sizeof(FingerprintPacket*) * new_size,
                                MALLOC_CAP_8BIT);
                                
                            if (new_packets) {
                                response->packets = new_packets;
                                ESP_LOGD(TAG, "Expanded packet array to %d slots", new_size);
                            } else {
                                ESP_LOGW(TAG, "Failed to resize packet array, discarding packet");
                                heap_caps_free(new_packet);
                                goto buffer_shift;
                            }
                        }
                        
                        response->packets[response->count++] = new_packet;
                        ESP_LOGD(TAG, "Added packet to response, count now: %d", response->count);
                    }
                    
                    state = WAIT_HEADER;  // Look for next packet header
                } else {
                    goto buffer_shift;
                }
                break;
        }
    }
    
    // Emergency timeout - if we've been processing too long without progress
    if ((current_time - last_buffer_change_time) > 10000) {  // Increased from 5000 to 10000
        // Only reset if we're still within the same state for too long
        if (state != WAIT_HEADER) {
            ESP_LOGW(TAG, "Emergency timeout - packet processing incomplete after 10s");
            // Don't fully reset the buffer, try to find next valid header
            size_t next_header = i;
            for (; next_header < buffer_pos - 1; next_header++) {
                if (buffer[next_header] == 0xEF && buffer[next_header+1] == 0x01) {
                    i = next_header;  // Jump to next header
                    state = WAIT_HEADER;
                    ESP_LOGD(TAG, "Found next valid header at position %d, continuing", next_header);
                    break;
                }
            }
            
            // Only if no valid header found, clear the buffer
            if (state != WAIT_HEADER) {
                i = buffer_pos;  // Clear buffer
                state = WAIT_HEADER;
                ESP_LOGW(TAG, "No valid headers found, clearing buffer");
            }
        }
        // Reset the timer to prevent repeated timeouts
        last_buffer_change_time = current_time;
    }
    
buffer_shift:
    // Move any remaining bytes to the start of buffer
    if (i < buffer_pos) {
        memmove(buffer, buffer + i, buffer_pos - i);
        buffer_pos = buffer_pos - i;
        ESP_LOGD(TAG, "Shifted buffer, %d bytes remaining", buffer_pos);
    } else {
        buffer_pos = 0;
        ESP_LOGD(TAG, "Buffer fully processed, reset position");
    }
    
    if (response->count == 0) {
        ESP_LOGW(TAG, "No complete packets found, returning NULL");
        if (response->template_data) {
            heap_caps_free(response->template_data);
        }
        heap_caps_free(response->packets);
        heap_caps_free(response);
        return NULL;
    }
    
    // For non-template responses, provide a summary instead of packet details
    if (response->count > 0) {
        ESP_LOGI(TAG, "Returning response with %d packets", response->count);
        
        // Log the first packet ID and type at INFO level
        if (response->packets[0]->packet_id == 0x07) {
            ESP_LOGI(TAG, "Command response packet: ID=0x%02X, Status=0x%02X", 
                    response->packets[0]->packet_id, response->packets[0]->code.confirmation);
        }
    }
    
    return response;
}


// void read_response_task(void *pvParameter) {
//     while (1) {
//         FingerprintPacket *response = fingerprint_read_response();
//         if (response) {
//             fingerprint_response_t event = {0};
//             memcpy(&event.packet, response, sizeof(FingerprintPacket));
//             heap_caps_free(response);

//             // Try to send to queue with increased timeout
//             if (xQueueSend(fingerprint_response_queue, &event, pdMS_TO_TICKS(500)) != pdPASS) {
//                 ESP_LOGW(TAG, "Response queue full, clearing old messages");
//                 xQueueReset(fingerprint_response_queue);  // Clear stale messages
//                 // Try sending again
//                 if (xQueueSend(fingerprint_response_queue, &event, pdMS_TO_TICKS(100)) != pdPASS) {
//                     ESP_LOGE(TAG, "Still unable to send to queue after reset");
//                 }
//             }
//         }
//         vTaskDelay(pdMS_TO_TICKS(10));
//     }
// }

// Insert this function right before the read_response_task function
FingerprintPacket* extract_packet_from_raw_data(uint8_t* data, size_t data_len, uint8_t target_packet_id) {
    for (size_t i = 0; i < data_len - 9; i++) {
        // Find header (0xEF01) followed by the correct packet ID
        if (data[i] == 0xEF && data[i+1] == 0x01 && i+6 < data_len && data[i+6] == target_packet_id) {
            // Allocate memory for the packet
            FingerprintPacket* packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
            if (!packet) return NULL;
            
            memset(packet, 0, sizeof(FingerprintPacket));
            packet->header = 0xEF01;
            packet->address = (data[i+2] << 24) | (data[i+3] << 16) | (data[i+4] << 8) | data[i+5];
            packet->packet_id = target_packet_id;
            
            // Get length
            if (i+8 < data_len) {
                packet->length = (data[i+7] << 8) | data[i+8];
            }
            
            // Parameter data starts at i+9 
            size_t param_start = i + 9;
            
            // Calculate actual parameter length (excluding checksum)
            size_t param_len = 0;
            if (packet->length > 2) {
                param_len = packet->length - 2; // Subtract 2 bytes for checksum
                
                // Check for FOOF marker in parameter data
                for (size_t j = 0; j < param_len && param_start + j < data_len - 3; j++) {
                    if (data[param_start + j] == 'F' && 
                        data[param_start + j+1] == 'O' && 
                        data[param_start + j+2] == 'O' && 
                        data[param_start + j+3] == 'F') {
                        
                        // Found FOOF marker - adjust packet length to include up to FOOF
                        param_len = j + 4; // Include the FOOF marker
                        packet->length = param_len + 2; // +2 for checksum
                        ESP_LOGI(TAG, "FOOF marker found in embedded packet at position %d", j);
                        break;
                    }
                }
                
                // Limit to available data and parameter buffer size
                param_len = min(param_len, sizeof(packet->parameters));
                param_len = min(param_len, data_len - param_start);
                
                // Copy parameter data
                if (param_len > 0) {
                    memcpy(packet->parameters, &data[param_start], param_len);
                }
            }
            
            // Checksum is the last 2 bytes of the packet
            size_t checksum_pos = param_start + param_len;
            if (checksum_pos + 1 < data_len) {
                packet->checksum = (data[checksum_pos] << 8) | data[checksum_pos + 1];
                
                // Log the extracted checksum
                ESP_LOGI(TAG, "Extracted packet ID=0x%02X with checksum 0x%04X at position %d", 
                         packet->packet_id, packet->checksum, i);
                
                // Verify checksum
                uint16_t calc_checksum = target_packet_id;
                calc_checksum += (packet->length >> 8) & 0xFF;
                calc_checksum += packet->length & 0xFF;
                
                for (size_t j = 0; j < param_len; j++) {
                    calc_checksum += packet->parameters[j];
                }
                
                if (calc_checksum != packet->checksum) {
                    // Just warn, don't stop processing
                    ESP_LOGW(TAG, "Checksum mismatch for packet 0x%02X: extracted=0x%04X, calculated=0x%04X", 
                             target_packet_id, packet->checksum, calc_checksum);
                    // Update with calculated checksum
                    packet->checksum = calc_checksum;
                }
            }
            
            return packet;
        }
    }
    
    return NULL;
}

static uint8_t saved_template[1024]; // Buffer for template data
bool template_available = false;
uint8_t saved_template_size = 0;

// Replace the existing read_response_task function with this improved version
void read_response_task(void *pvParameter) {
    // Static timer for template timeouts
    static uint32_t template_start_time = 0;
    
    while (1) {
        MultiPacketResponse *response = fingerprint_read_response();
        
        if (response) {
            // Special handling for template upload (UpChar command)
            if (last_sent_command == FINGERPRINT_CMD_UP_CHAR) {
                // Initialize template accumulator if needed
                if (g_template_accumulator == NULL) {
                    g_template_accumulator = heap_caps_malloc(sizeof(MultiPacketResponse), MALLOC_CAP_8BIT);
                    if (g_template_accumulator) {
                        g_template_accumulator->packets = NULL;
                        g_template_accumulator->count = 0;
                        g_template_accumulator->collecting_template = true;
                        g_template_accumulator->template_data = NULL;
                        g_template_accumulator->template_size = 0;
                        g_template_accumulator->template_capacity = 0;
                        template_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        ESP_LOGI(TAG, "Template accumulator initialized");
                    }
                }
                
                // Process if we have a valid accumulator
                if (g_template_accumulator) {
                    bool found_final_packet = false;
                    bool found_raw_final_packet = false;
                    bool found_foof_marker = false;
                    size_t foof_position = 0;
                    
                    // Check if we have a final packet (0x08) already
                    for (size_t i = 0; i < g_template_accumulator->count; i++) {
                        if (g_template_accumulator->packets[i]->packet_id == 0x08) {
                            found_final_packet = true;
                            ESP_LOGI(TAG, "Already have final packet (ID=0x08) at position %d", i);
                            break;
                        }
                    }
                    
                    // Add packets to accumulator
                    if (response->count > 0) {
                        // Resize packet array
                        size_t new_count = g_template_accumulator->count + response->count;
                        FingerprintPacket** new_packets = heap_caps_realloc(
                            g_template_accumulator->packets,
                            sizeof(FingerprintPacket*) * new_count,
                            MALLOC_CAP_8BIT
                        );
                        
                        if (new_packets) {
                            g_template_accumulator->packets = new_packets;
                            
                            // Deep copy each packet with proper checksum calculation
                            for (size_t i = 0; i < response->count; i++) {
                                FingerprintPacket* src_packet = response->packets[i];
                                
                                // Create a new packet with extracted data and calculated checksum
                                FingerprintPacket* new_packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                                if (new_packet) {
                                    // Copy basic packet info
                                    memcpy(new_packet, src_packet, sizeof(FingerprintPacket));
                                    
                                    // Calculate proper checksum instead of using the copied one
                                    uint16_t calc_checksum = src_packet->packet_id;
                                    calc_checksum += (src_packet->length >> 8) & 0xFF;
                                    calc_checksum += src_packet->length & 0xFF;
                                    
                                    // Add all parameter bytes to checksum
                                    size_t param_len = src_packet->length > 2 ? src_packet->length - 2 : 0;
                                    for (size_t j = 0; j < param_len; j++) {
                                        calc_checksum += src_packet->parameters[j];
                                    }
                                    
                                    // Use the calculated checksum
                                    new_packet->checksum = calc_checksum;
                                    
                                    // Check for FOOF marker in this packet's data
                                    if (src_packet->packet_id == 0x02) {
                                        for (size_t j = 0; j < param_len - 3; j++) {
                                            if (src_packet->parameters[j] == 'F' && 
                                                src_packet->parameters[j+1] == 'O' &&
                                                src_packet->parameters[j+2] == 'O' &&
                                                src_packet->parameters[j+3] == 'F') {
                                                found_foof_marker = true;
                                                foof_position = j;
                                                ESP_LOGI(TAG, "Found FOOF marker in packet at position %d", j);
                                                
                                                // Truncate this packet at the FOOF marker
                                                size_t new_data_length = j + 4; // Include the FOOF marker
                                                size_t new_packet_length = new_data_length + 2; // Add checksum bytes
                                                
                                                // Update packet length
                                                new_packet->length = new_packet_length;
                                                
                                                // Recalculate checksum for truncated packet
                                                calc_checksum = new_packet->packet_id;
                                                calc_checksum += (new_packet_length >> 8) & 0xFF;
                                                calc_checksum += new_packet_length & 0xFF;
                                                
                                                for (size_t k = 0; k < new_data_length; k++) {
                                                    calc_checksum += new_packet->parameters[k];
                                                }
                                                
                                                new_packet->checksum = calc_checksum;
                                                ESP_LOGI(TAG, "Truncated packet at FOOF marker, new length: %d", new_data_length);
                                                
                                                // CRITICAL FIX: Set the TEMPLATE_UPLOAD_COMPLETE_BIT immediately upon finding FOOF
                                                if (enroll_event_group != NULL) {
                                                    xEventGroupSetBits(enroll_event_group, TEMPLATE_UPLOAD_COMPLETE_BIT);
                                                    ESP_LOGI(TAG, "Template upload completion signaled (FOOF marker found)");
                                                }
                                                
                                                break;
                                            }
                                        }
                                    }
                                    
                                    g_template_accumulator->packets[g_template_accumulator->count++] = new_packet;
                                    
                                    ESP_LOGI(TAG, "Added packet ID=0x%02X with calculated checksum 0x%04X", 
                                            new_packet->packet_id, new_packet->checksum);
                                            
                                    // Check if this is a final packet (0x08)
                                    if (new_packet->packet_id == 0x08) {
                                        found_final_packet = true;
                                        ESP_LOGI(TAG, "Found final packet (ID=0x08) at position %d", g_template_accumulator->count-1);
                                    }
                                }
                            }
                        }
                    }
                    
                    // Accumulate raw template data - no filtering
                    if (response->template_data && response->template_size > 0) {
                        if (g_template_accumulator->template_data == NULL) {
                            // First data chunk
                            g_template_accumulator->template_data = heap_caps_malloc(
                                response->template_size, 
                                MALLOC_CAP_8BIT
                            );
                            if (g_template_accumulator->template_data) {
                                memcpy(g_template_accumulator->template_data, 
                                       response->template_data, 
                                       response->template_size);
                                g_template_accumulator->template_size = response->template_size;
                                g_template_accumulator->template_capacity = response->template_size;
                                ESP_LOGI(TAG, "Added %d bytes to template accumulator", 
                                         response->template_size);
                            }
                        } else {
                            // Append to existing data
                            size_t new_size = g_template_accumulator->template_size + response->template_size;
                            if (new_size > g_template_accumulator->template_capacity) {
                                // Expand buffer
                                uint8_t* new_data = heap_caps_realloc(
                                    g_template_accumulator->template_data,
                                    new_size,
                                    MALLOC_CAP_8BIT
                                );
                                if (new_data) {
                                    g_template_accumulator->template_data = new_data;
                                    g_template_accumulator->template_capacity = new_size;
                                }
                            }
                            
                            // Append data if we have room
                            if (new_size <= g_template_accumulator->template_capacity) {
                                memcpy(g_template_accumulator->template_data + g_template_accumulator->template_size,
                                       response->template_data,
                                       response->template_size);
                                g_template_accumulator->template_size = new_size;
                                ESP_LOGI(TAG, "Added %d bytes to template accumulator (total: %d bytes)",
                                         response->template_size, g_template_accumulator->template_size);
                            }
                        }
                    }
                    
                    // Check for FOOF marker in the raw data if we haven't found one already
                    if (!found_foof_marker && g_template_accumulator->template_data && g_template_accumulator->template_size > 4) {
                        for (size_t i = 0; i < g_template_accumulator->template_size - 4; i++) {
                            if (g_template_accumulator->template_data[i] == 'F' && 
                                g_template_accumulator->template_data[i+1] == 'O' &&
                                g_template_accumulator->template_data[i+2] == 'O' && 
                                g_template_accumulator->template_data[i+3] == 'F') {
                                found_foof_marker = true;
                                foof_position = i;
                                ESP_LOGI(TAG, "Found FOOF marker in raw data at position %d", i);
                                
                                // Truncate the raw template data to include the FOOF marker
                                g_template_accumulator->template_size = i + 4;
                                ESP_LOGI(TAG, "Truncated raw template data to %d bytes (including FOOF marker)", 
                                         g_template_accumulator->template_size);
                                
                                // CRITICAL FIX: Set the template completion bit immediately
                                if (enroll_event_group != NULL) {
                                    xEventGroupSetBits(enroll_event_group, TEMPLATE_UPLOAD_COMPLETE_BIT);
                                    ESP_LOGI(TAG, "Template upload completion signaled (FOOF marker in raw data)");
                                }
                                
                                break;
                            }
                        }
                    }
                    
                    // Check for completion based on explicit final packet detection or timing
                    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    bool template_complete = found_final_packet || found_raw_final_packet || found_foof_marker;
                    
                    // Fallback to time-based detection only if we haven't found an explicit marker
                    if (!template_complete) {
                        // If we have a substantial amount of data AND a certain time has passed
                        if (g_template_accumulator->template_size > 500 && 
                            (current_time - template_start_time > 1500)) {
                            template_complete = true;
                            ESP_LOGI(TAG, "Template appears complete based on size and timing");
                        }
                        
                        // Force completion after a timeout
                        if (current_time - template_start_time > 3000) {
                            template_complete = true;
                            ESP_LOGI(TAG, "Template collection timed out, treating as complete");
                        }
                    }
                    
                    if (template_complete) {
                        // If we found a FOOF marker but don't have a final packet, create one
                       
                        
                        // Create event with accumulated data
                        fingerprint_event_t event = {
                            .type = EVENT_TEMPLATE_UPLOADED,
                            .status = FINGERPRINT_OK,
                            .command = FINGERPRINT_CMD_UP_CHAR
                        };
                        
                        // Use the last packet for event details
                        if (g_template_accumulator->count > 0) {
                            event.packet = *(g_template_accumulator->packets[g_template_accumulator->count-1]);
                        }
                        
                        // Add the multi-packet response to the event
                        MultiPacketResponse* event_multi_packet = heap_caps_malloc(sizeof(MultiPacketResponse), MALLOC_CAP_8BIT);
                        if (event_multi_packet) {
                            // Copy basic details
                            event_multi_packet->count = g_template_accumulator->count;
                            event_multi_packet->collecting_template = g_template_accumulator->collecting_template;
                            event_multi_packet->template_complete = true;  // Mark as complete
                            event_multi_packet->start_time = g_template_accumulator->start_time;
                            event_multi_packet->template_size = g_template_accumulator->template_size;
                            event_multi_packet->template_capacity = g_template_accumulator->template_capacity;
                            
                            // Copy packets
                            event_multi_packet->packets = heap_caps_malloc(sizeof(FingerprintPacket*) * g_template_accumulator->count, MALLOC_CAP_8BIT);
                            if (event_multi_packet->packets) {
                                for (size_t i = 0; i < g_template_accumulator->count; i++) {
                                    if (g_template_accumulator->packets[i] != NULL) {
                                        event_multi_packet->packets[i] = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                                        if (event_multi_packet->packets[i]) {
                                            memcpy(event_multi_packet->packets[i], g_template_accumulator->packets[i], sizeof(FingerprintPacket));
                                        }
                                    } else {
                                        event_multi_packet->packets[i] = NULL;
                                    }
                                }
                            } else {
                                event_multi_packet->packets = NULL;
                            }
                            
                            // Copy template data if available
                            if (g_template_accumulator->template_data && g_template_accumulator->template_size > 0) {
                                event_multi_packet->template_data = heap_caps_malloc(g_template_accumulator->template_size, MALLOC_CAP_8BIT);
                                if (event_multi_packet->template_data) {
                                    memcpy(event_multi_packet->template_data, 
                                          g_template_accumulator->template_data,
                                          g_template_accumulator->template_size);
                                } else {
                                    event_multi_packet->template_data = NULL;
                                }
                            } else {
                                event_multi_packet->template_data = NULL;
                            }
                            
                            // Assign to event
                            event.multi_packet = event_multi_packet;
                        } else {
                            event.multi_packet = NULL;
                            ESP_LOGE(TAG, "Failed to allocate memory for multi-packet response");
                        }
                        
                        // Add debug output before triggering the event
                        ESP_LOGI(TAG, "Triggering EVENT_TEMPLATE_UPLOADED with %d packets", 
                                 event.multi_packet ? event.multi_packet->count : 0);
                        
                        // Set global variables to indicate template is available
                        template_available = true;
                        saved_template_size = g_template_accumulator->template_size;
                        
                        // Signal completion
                        if (enroll_event_group != NULL) {
                            xEventGroupSetBits(enroll_event_group, TEMPLATE_UPLOAD_COMPLETE_BIT);
                            ESP_LOGI(TAG, "Template upload complete");
                        }
                        
                        // Trigger the event
                        trigger_fingerprint_event(event);
                        
                        // Clean up accumulator after triggering the event
                        for (size_t i = 0; i < g_template_accumulator->count; i++) {
                            if (g_template_accumulator->packets[i] != NULL) {
                                heap_caps_free(g_template_accumulator->packets[i]);
                                g_template_accumulator->packets[i] = NULL;
                            }
                        }
                        heap_caps_free(g_template_accumulator->packets);
                        if (g_template_accumulator->template_data) {
                            heap_caps_free(g_template_accumulator->template_data);
                        }
                        heap_caps_free(g_template_accumulator);
                        g_template_accumulator = NULL;
                    }
                }
                
                // Clean up current response
                for (size_t i = 0; i < response->count; i++) {
                    heap_caps_free(response->packets[i]);
                }
                heap_caps_free(response->packets);
                if (response->template_data) {
                    heap_caps_free(response->template_data);
                }
                heap_caps_free(response);
            } else {
                // Process non-template responses normally
                for (size_t i = 0; i < response->count; i++) {
                    if (response->packets[i]->packet_id == 0x07) {
                        fingerprint_status_event_handler(
                            (fingerprint_status_t)response->packets[i]->code.confirmation, 
                            response->packets[i]);
                    }
                }
                
                // Clean up response
                for (size_t i = 0; i < response->count; i++) {
                    heap_caps_free(response->packets[i]);
                }
                heap_caps_free(response->packets);
                if (response->template_data) {
                    heap_caps_free(response->template_data);
                }
                heap_caps_free(response);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// In process_response_task
// In process_response_task function

static fingerprint_template_buffer_t g_template_buffer = {
    .data = NULL,
    .size = 0,
    .is_complete = false
};

void process_response_task(void *pvParameter) {
    fingerprint_command_info_t last_cmd = {0};
    fingerprint_response_t response;
    bool waiting_for_template = false;
    
    // Add template processing cooldown tracking
    static bool template_completed_recently = false;
    static uint32_t template_completion_time = 0;
    
    // Increase buffer size to 4096 bytes (4KB) - sufficient for all templates
    static uint8_t template_buffer[4096] = {0};
    static size_t template_size = 0;
    static uint32_t template_start_time = 0;
    static bool template_data_complete = false;

    while (1) {
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Skip processing during cooldown period after template upload completes
        if (template_completed_recently) {
            if (current_time - template_completion_time > 2000) {
                template_completed_recently = false;
                ESP_LOGD(TAG, "Process task exiting cooldown after template event");
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }
        
        if (xQueueReceive(fingerprint_response_queue, &response, portMAX_DELAY) == pdTRUE) {
            BaseType_t cmd_received = xQueueReceive(fingerprint_command_queue, 
                                                  &last_cmd, 
                                                  pdMS_TO_TICKS(100));

            // Always set success bit for any UpChar response packet
            if (cmd_received && last_cmd.command == FINGERPRINT_CMD_UP_CHAR) {
                if (enroll_event_group != NULL) {
                    ESP_LOGD(TAG, "Setting success bit for UpChar command acknowledgment");
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                }
            }

            // Check if this is the start of template upload
            if (cmd_received && last_cmd.command == FINGERPRINT_CMD_UP_CHAR && 
                response.packet.packet_id != 0x02 && response.packet.packet_id != 0x08) {
                waiting_for_template = true;
                
                // Explicitly initialize ALL variables to known good values
                template_size = 0;
                template_start_time = current_time;
                template_data_complete = false;
                template_available = false;  // Reset global flag
                saved_template_size = 0;     // Reset global size
                
                // Use explicit safe buffer clearing
                memset(template_buffer, 0, sizeof(template_buffer));
                ESP_LOGD(TAG, "Starting template upload, buffer cleared (size: %d bytes)", sizeof(template_buffer));
            }

            // Process based on packet type
            if (response.packet.packet_id == 0x02) {
                // Regular template data packet
                if (waiting_for_template) {
                    // Get actual data length with proper calculation and bounds checking
                    size_t data_length = 0;
                    
                    // Check if packet contains actual data with strict validation
                    if (response.packet.length > 2 && response.packet.length <= sizeof(response.packet.parameters) + 2) {
                        data_length = response.packet.length - 2; // Subtract checksum bytes
                    } else {
                        ESP_LOGW(TAG, "Invalid packet length: %d", response.packet.length);
                        data_length = 0;
                    }
                    
                    // Safety check - don't exceed parameters buffer
                    size_t max_data_length = sizeof(response.packet.parameters);
                    if (data_length > max_data_length) {
                        ESP_LOGW(TAG, "Limiting packet data from %d to %d bytes", data_length, max_data_length);
                        data_length = max_data_length;
                    }
                    
                    // Look for FOOF marker in the data which indicates end of template
                    for (size_t i = 0; i < data_length - 3; i++) {
                        if (response.packet.parameters[i] == 'F' && 
                            response.packet.parameters[i+1] == 'O' &&
                            response.packet.parameters[i+2] == 'O' &&
                            response.packet.parameters[i+3] == 'F') {
                            ESP_LOGI(TAG, "FOOF marker found in template data at position %d", i);
                            template_data_complete = true;
                            // IMPORTANT: Trim template data at FOOF marker (i+4 includes FOOF)
                            template_size = template_size + i;
                            break;
                        }
                    }
                    
                    // Strict buffer overflow prevention
                    if (template_size + data_length <= sizeof(template_buffer)) {
                        if (data_length > 0) {
                            memcpy(template_buffer + template_size, 
                                   response.packet.parameters, 
                                   data_length);
                            template_size += data_length;
                            
                            // Only log significant changes to reduce clutter
                            if (data_length >= 100) {
                                ESP_LOGI(TAG, "Added %d bytes to template buffer (total: %d bytes)", 
                                         data_length, template_size);
                            } else {
                                ESP_LOGD(TAG, "Added %d bytes to template buffer (total: %d bytes)", 
                                         data_length, template_size);
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "Buffer would overflow! Current: %d, Adding: %d, Max: %d", 
                                template_size, data_length, sizeof(template_buffer));
                    }
                    
                    // Create standard event for ongoing template data
                    fingerprint_event_t template_event = {
                        .type = EVENT_TEMPLATE_UPLOADED,
                        .status = FINGERPRINT_OK,
                        .command = last_cmd.command,
                        .packet = response.packet,
                        .data = { .template_data = {NULL, 0, false} }  // Initialize to safe values
                    };
                    
                    // Include template data if complete
                    if (template_data_complete) {
                        // Sanity check template size
                        if (template_size > 0 && template_size <= 10000) {
                            uint8_t* template_copy = heap_caps_malloc(template_size, MALLOC_CAP_8BIT);
                            if (template_copy) {
                                memcpy(template_copy, template_buffer, template_size);
                                template_event.data.template_data.data = template_copy;
                                template_event.data.template_data.size = template_size;
                                template_event.data.template_data.is_complete = true;
                                ESP_LOGI(TAG, "Including complete template data (%d bytes) in event", template_size);
                                
                                // Set global variables to indicate template is available
                                template_available = true;
                                saved_template_size = template_size;
                                
                                // Signal completion - IMMEDIATELY after finding FOOF
                                if (enroll_event_group != NULL) {
                                    xEventGroupSetBits(enroll_event_group, TEMPLATE_UPLOAD_COMPLETE_BIT);
                                    ESP_LOGI(TAG, "Template upload complete (FOOF marker)");
                                    waiting_for_template = false;  // Stop waiting for more data
                                    
                                    // Set template completion cooldown period
                                    template_completed_recently = true;
                                    template_completion_time = current_time;
                                }
                            } else {
                                ESP_LOGE(TAG, "Failed to allocate memory for template copy");
                            }
                        } else {
                            ESP_LOGW(TAG, "Template size invalid: %d bytes", template_size);
                            template_data_complete = false;
                        }
                    }
                    
                    trigger_fingerprint_event(template_event);
                }
            } else if (response.packet.packet_id == 0x08) {
                // Final template packet - should only reach here if no FOOF marker was found
                if (waiting_for_template) {
                    ESP_LOGI(TAG, "Final template packet detected (total size: %d bytes)", template_size);
                    
                    // Clean up trailing zeros from template
                    size_t actual_size = template_size;
                    while (actual_size > 0 && template_buffer[actual_size-1] == 0) {
                        actual_size--;
                    }
                    if (actual_size < template_size) {
                        ESP_LOGD(TAG, "Trimmed %d trailing zeros from template", template_size - actual_size);
                        template_size = actual_size;
                    }
                    
                    // Create event with complete template
                    fingerprint_event_t template_event = {
                        .type = EVENT_TEMPLATE_UPLOADED,
                        .status = FINGERPRINT_OK,
                        .command = last_cmd.command,
                        .packet = response.packet,
                        .data = { .template_data = {NULL, 0, false} }  // Initialize to safe values
                    };
                    
                    // Copy final template data to event only if not already done with FOOF marker
                    if (!template_data_complete && template_size > 0) {
                        // Sanity check template size
                        if (template_size <= 10000) {
                            uint8_t* template_copy = heap_caps_malloc(template_size, MALLOC_CAP_8BIT);
                            if (template_copy) {
                                memcpy(template_copy, template_buffer, template_size);
                                template_event.data.template_data.data = template_copy;
                                template_event.data.template_data.size = template_size;
                                template_event.data.template_data.is_complete = true;
                                ESP_LOGI(TAG, "Including complete template data (%d bytes) in final event", template_size);
                                
                                // Set global variables to indicate template is available
                                template_available = true;
                                saved_template_size = template_size;
                            } else {
                                ESP_LOGE(TAG, "Failed to allocate memory for template copy");
                            }
                        } else {
                            ESP_LOGW(TAG, "Template size too large: %d bytes", template_size);
                        }
                    }
                    
                    // Signal completion - MOST IMPORTANT PART
                    if (enroll_event_group != NULL) {
                        xEventGroupSetBits(enroll_event_group, TEMPLATE_UPLOAD_COMPLETE_BIT);
                        ESP_LOGI(TAG, "Template upload complete (final packet)");
                    }
                    
                    // Trigger the event
                    trigger_fingerprint_event(template_event);
                    
                    // Reset for next time
                    waiting_for_template = false;
                    template_size = 0;
                    template_data_complete = false;
                    
                    // Set template completion cooldown period
                    template_completed_recently = true;
                    template_completion_time = current_time;
                }
            } else {
                // Regular command response packet
                fingerprint_status_event_handler(response.packet.code.confirmation, &response.packet);
            }
            
            // Check for template timeout (prevent stuck state)
            // ONLY reset if we're still waiting (don't interfere with completed templates)
            if (waiting_for_template && (current_time - template_start_time > 5000)) {
                ESP_LOGW(TAG, "Template upload timed out after 5 seconds");
                waiting_for_template = false;
                template_size = 0;
                template_data_complete = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// void process_response_task(void *pvParameter) {
//     fingerprint_command_info_t last_cmd;
//     fingerprint_response_t response;
//     bool waiting_for_template = false;
//     uint8_t last_buffer_id = 0;

//     while (1) {
//         if (xQueueReceive(fingerprint_response_queue, &response, portMAX_DELAY) == pdTRUE) {
//             // ESP_LOGI(TAG, "Received response: 0x%02X", response.packet.code.command);
//             // Handle template data packets differently
//             if (response.packet.packet_id == 0x02 || response.packet.packet_id == 0x08) {
//                 // This is a template data packet
//                 if (waiting_for_template) {
//                     // Process template data
//                     fingerprint_status_event_handler(response.packet.code.confirmation, &response.packet);
                    
//                     if (response.packet.packet_id == 0x08) {
//                         // Last packet received, reset state
//                         waiting_for_template = false;
//                     }
//                 }
//                 continue;
//             }

//             // Regular command-response handling
//             if (xQueueReceive(fingerprint_command_queue, &last_cmd, pdMS_TO_TICKS(3000)) == pdTRUE) {
//                 uint8_t received_confirmation = response.packet.code.command;
                
//                 // Check if this is the start of template upload
//                 if (last_cmd.command == FINGERPRINT_CMD_UP_CHAR && received_confirmation == FINGERPRINT_OK) {
//                     waiting_for_template = true;
//                     last_buffer_id = response.packet.parameters[0];
//                 }
                
//                 fingerprint_status_event_handler((fingerprint_status_t)received_confirmation, &response.packet);
//             } else {
//                 // Only log warning for non-template packets
//                 if (response.packet.packet_id != 0x02 && response.packet.packet_id != 0x08) {
//                     ESP_LOGW(TAG, "No corresponding command found for response!");
//                 }
//             }
//         }
//         vTaskDelay(pdMS_TO_TICKS(10));
//     }
// }

// void process_response_task(void *pvParameter) {
//     fingerprint_command_info_t last_cmd = {0};
//     fingerprint_response_t response;
//     bool waiting_for_template = false;

//     while (1) {
//         // Wait for response with timeout
//         if (xQueueReceive(fingerprint_response_queue, &response, portMAX_DELAY) == pdTRUE) {
//             // Always try to get corresponding command first
//             BaseType_t cmd_received = xQueueReceive(fingerprint_command_queue, 
//                                                   &last_cmd, 
//                                                   pdMS_TO_TICKS(100));
//             // ESP_LOG_BUFFER_HEX_LEVEL(TAG, &response.packet, sizeof(FingerprintPacket), ESP_LOG_INFO);
//             // ESP_LOG_BUFFER_HEXDUMP(TAG, &response.packet, sizeof(FingerprintPacket), ESP_LOG_INFO);

//             // Process based on packet type
//             if (response.packet.packet_id == 0x02 || response.packet.packet_id == 0x08) {
//                 // Template data packet
//                 if (waiting_for_template) {
//                     fingerprint_status_event_handler(response.packet.code.confirmation, 
//                                                   &response.packet);
                    
//                     if (response.packet.packet_id == 0x08) {
//                         waiting_for_template = false;
//                     }
//                 }
//             } else {
//                 // Command response packet
//                 if (cmd_received == pdTRUE) {
//                     // Check for template upload start
//                     if (last_cmd.command == FINGERPRINT_CMD_UP_CHAR && 
//                         response.packet.code.command == FINGERPRINT_OK) {
//                         waiting_for_template = true;
//                     }
                    
//                     fingerprint_status_event_handler((fingerprint_status_t)response.packet.code.command, 
//                                                   &response.packet);
//                 }
//             }
//         }
//         vTaskDelay(pdMS_TO_TICKS(10));
//     }
// }


fingerprint_status_t fingerprint_get_status(FingerprintPacket *packet) {
    if (!packet) {
        return FINGERPRINT_ILLEGAL_DATA; // Return a default error if the packet is NULL
    }

    return (fingerprint_status_t)packet->code.confirmation; // The command field stores the status code
}

// Event status handler function
void fingerprint_status_event_handler(fingerprint_status_t status, FingerprintPacket *packet) {
    fingerprint_event_type_t event_type = EVENT_NONE;
    fingerprint_event_t event;
    event.status = status;
    event.command = last_sent_command;
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI("Status Event Handler", "Status: %d", status);
    if (packet != NULL) {
        event.packet = *packet;  // Store full response packet
    } else {
        memset(&event.packet, 0, sizeof(FingerprintPacket));  // Clear packet structure to avoid garbage values
    }

    switch (status) {
        case FINGERPRINT_OK:
        // Signal success for GetImage command
        if (last_sent_command == FINGERPRINT_CMD_GET_IMAGE && enroll_event_group != NULL) {
            xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
            ESP_LOGI(TAG, "Image capture successful, signaling event group");
        }
        // Signal success for GenChar command
        else if ((last_sent_command == FINGERPRINT_CMD_GEN_CHAR) && enroll_event_group != NULL) {
            xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
            ESP_LOGI(TAG, "Feature extraction successful, signaling event group");
        }

        // In the fingerprint_status_event_handler function, modify the FINGERPRINT_CMD_SEARCH handling:
        if (last_sent_command == FINGERPRINT_CMD_SEARCH) {
            // Check if we're in cooldown period to prevent duplicate matches
            if (match_cooldown_active && (current_time - last_match_time < 1000)) {
                ESP_LOGD(TAG, "Ignoring duplicate match response (cooldown active)");
                return; // Skip processing this response
            }
            
            // Parse match information first
            event.data.match_info.page_id = (packet->parameters[1] << 8) | packet->parameters[0];
            event.data.match_info.template_id = convert_page_id_to_index(event.data.match_info.page_id);
            event.data.match_info.match_score = (packet->parameters[3] << 8) | packet->parameters[2];
            
            // Only consider it a match if score is greater than 0
            if (event.data.match_info.match_score > 0) {
                // A duplicate was found, but the error handling isn't stopping the enrollment
                event_type = EVENT_SEARCH_SUCCESS;
                // This still sets the SUCCESS bit instead of FAIL
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                }
                ESP_LOGI(TAG, "Real match found with score %d", event.data.match_info.match_score);
                
                // Set cooldown to prevent duplicate processing
                last_match_time = current_time;
                match_cooldown_active = true;
            } else {
                // Score of 0 means no real match was found (common with empty database)
                if (enrollment_in_progress) {
                    // During enrollment, no match is expected and not an error
                    event_type = EVENT_NONE;  // Don't trigger error event
                    ESP_LOGI(TAG, "Search returned zero score, not a duplicate");
                } else {
                    // During verification, for zero scores, don't generate an error event immediately
                    // Just log it and set the FAIL bit, but don't generate an event
                    event_type = EVENT_NONE;  // DON'T trigger error event for zero scores
                    ESP_LOGI(TAG, "Search returned success but match score is 0, waiting for more responses");
                }
                
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                }
            }
        } else if (last_sent_command == FINGERPRINT_CMD_GET_IMAGE && enroll_event_group!=NULL) {
            event_type = EVENT_FINGER_DETECTED;
            xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
        } else if (last_sent_command == FINGERPRINT_CMD_GET_IMAGE) {
            // event_type = EVENT_IMAGE_VALID;
        } else if (last_sent_command == FINGERPRINT_CMD_VALID_TEMPLATE_NUM) {
            event_type = EVENT_TEMPLATE_COUNT;
            event.data.template_count.count = (packet->parameters[0] << 8) | packet->parameters[1];
        } else if (last_sent_command == FINGERPRINT_CMD_READ_INDEX_TABLE) {
            event_type = EVENT_INDEX_TABLE_READ;
            
            // Check if we're validating a location during enrollment
            if (packet != NULL && enroll_event_group && 
                xEventGroupGetBits(enroll_event_group) & CHECKING_LOCATION_BIT) {
                
                uint8_t template_exists = 0;
                uint8_t position = global_location & 0xFF;
                uint8_t byte_offset = position / 8;    // Which byte contains our bit
                uint8_t bit_position = position % 8;   // Which bit in that byte
                
                ESP_LOGI(TAG, "Checking template at position %d (byte %d, bit %d)", 
                         position, byte_offset, bit_position);
                
                // Check if the bit is set in the index table
                if (byte_offset < 32) { // We have 32 bytes of index data
                    if (packet->parameters[byte_offset] & (1 << bit_position)) {
                        template_exists = 1;
                        ESP_LOGW(TAG, "Template exists at position %d", position);
                    } else {
                        ESP_LOGI(TAG, "Position %d is free", position);
                    }
                }
                
                // Set appropriate event bit based on whether template exists
                if (template_exists) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                } else {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                }
            } else {
                // For normal index table reads, just signal success
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                }
            }
        } else if (last_sent_command == FINGERPRINT_CMD_GEN_CHAR) {
            event_type = EVENT_FEATURE_EXTRACTED;
        } else if (last_sent_command == FINGERPRINT_CMD_REG_MODEL) {
            event_type = EVENT_MODEL_CREATED;
        } else if (last_sent_command == FINGERPRINT_CMD_STORE_CHAR) {
            event_type = EVENT_TEMPLATE_STORED;
        } else if (last_sent_command == FINGERPRINT_CMD_READ_SYS_PARA) {
            event_type = EVENT_SYS_PARAMS_READ;
            event.data.sys_params.status_register = (packet->parameters[0] << 8) | packet->parameters[1];
            event.data.sys_params.system_id = (packet->parameters[2] << 8) | packet->parameters[3];
            event.data.sys_params.finger_library = (packet->parameters[4] << 8) | packet->parameters[5];
            event.data.sys_params.security_level = (packet->parameters[6] << 8) | packet->parameters[7];
            event.data.sys_params.device_address = (packet->parameters[8] << 24) | 
                                                 (packet->parameters[9] << 16) |
                                                 (packet->parameters[10] << 8) | 
                                                 packet->parameters[11];
            event.data.sys_params.data_packet_size = (1 << 5) << ((packet->parameters[12] << 8) | packet->parameters[13]); // 32 << N
            event.data.sys_params.baud_rate = ((packet->parameters[14] << 8) | packet->parameters[15]) * 9600;
        } else if (last_sent_command == FINGERPRINT_CMD_LOAD_CHAR) {
            event_type = EVENT_TEMPLATE_LOADED;
        } if (last_sent_command == FINGERPRINT_CMD_UP_CHAR) {
            event_type = EVENT_TEMPLATE_UPLOADED;
            if (packet->packet_id == 0x02) {  // Data packet
                ESP_LOGI(TAG, "Received data packet");
            } else if (packet->packet_id == 0x07) {  // Initial acknowledgment
                ESP_LOGI(TAG, "Upload starting");
            } else if (packet->packet_id == 0x08) {  // Final packet
                ESP_LOGI(TAG, "Upload complete");
            }
            // Set success bit for all valid packets
            if (enroll_event_group) {
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
            }
        } else if (last_sent_command == FINGERPRINT_CMD_READ_INF_PAGE) {
            event_type = EVENT_INFO_PAGE_READ;
            
            // For data packets (0x02) and end packet (0x08)
            if (packet->packet_id == 0x02 || packet->packet_id == 0x08) {
                ESP_LOGI(TAG, "Received info page packet: ID=0x%02X, Length=%d", 
                         packet->packet_id, packet->length);
                ESP_LOG_BUFFER_HEX(TAG, packet->parameters, packet->length);
                
                // Signal success for each received packet
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                }
            }
            // For confirmation packet
            else if (packet->packet_id == 0x07) {
                if (packet->code.confirmation == FINGERPRINT_OK) {
                    ESP_LOGI(TAG, "Information page read command accepted");
                    if (enroll_event_group) {
                        xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                    }
                } else {
                    ESP_LOGE(TAG, "Information page read command failed: 0x%02X", 
                             packet->code.confirmation);
                    if (enroll_event_group) {
                        xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                    }
                }
            }
        } 
        
        if (enroll_event_group) {
            if (last_sent_command != FINGERPRINT_CMD_SEARCH) {
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
            }
        }
        break;

        case FINGERPRINT_NO_FINGER:
            event_type = EVENT_NO_FINGER_DETECTED;
            if (enroll_event_group) {
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
            }
            break;
        
        case FINGERPRINT_IMAGE_FAIL:
        case FINGERPRINT_TOO_DRY:
        case FINGERPRINT_TOO_WET:
        case FINGERPRINT_TOO_CHAOTIC:
        case FINGERPRINT_UPLOAD_IMAGE_FAIL:
        case FINGERPRINT_IMAGE_AREA_SMALL:
        case FINGERPRINT_IMAGE_NOT_AVAILABLE:
            ESP_LOGE(TAG, "Image acquisition failed (0x%02X)", status);
            event_type = EVENT_IMAGE_FAIL;
            if (enroll_event_group) {
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
            }
            break;

        case FINGERPRINT_TOO_FEW_POINTS:
            ESP_LOGE(TAG, "Feature extraction failed (0x%02X)", status);
            event_type = EVENT_FEATURE_EXTRACT_FAIL;
            if (enroll_event_group) {
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
            }
            break;

        case FINGERPRINT_MISMATCH:
        case FINGERPRINT_NOT_FOUND:
        if (last_sent_command == FINGERPRINT_CMD_SEARCH) {
            // Determine if we're in enrollment or verification mode
            if (enrollment_in_progress) {
                // For enrollment, no match is GOOD (not a duplicate)
                event_type = EVENT_NONE;  // <-- Don't generate any error event
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                }
                ESP_LOGI(TAG, "No duplicate found during enrollment check - good to proceed");
            } else {
                // For verification, no match is a FAIL
                event_type = EVENT_MATCH_FAIL;
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                }
                ESP_LOGI(TAG, "No match found during verification");
            }
        }
        break;
        case FINGERPRINT_DB_FULL:
            event_type = EVENT_DB_FULL;
            break;

        case FINGERPRINT_TIMEOUT:
            event_type = EVENT_ERROR;
            break;

        case FINGERPRINT_PACKET_ERROR:
            if(last_sent_command == FINGERPRINT_CMD_DOWN_CHAR){
                ESP_LOGI(TAG, "DownChar packet error (status: 0x%02X)", status);
                event_type = EVENT_TEMPLATE_STORE_PACKET_ERROR;
                
                // CRITICAL FIX: Set the success bit to unblock the download process
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                    ESP_LOGI(TAG, "Forced success bit for DownChar despite packet error");
                }
            } else {
                event_type = EVENT_ERROR;
            }
            break;
            
        case FINGERPRINT_DATA_PACKET_ERROR:
            if(last_sent_command == FINGERPRINT_CMD_DOWN_CHAR){
                ESP_LOGI(TAG, "DownChar data packet error (status: 0x%02X)", status);
                event_type = EVENT_PACKET_RECEPTION_FAIL;
                
                // CRITICAL FIX: Set the success bit to unblock the download process
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                    ESP_LOGI(TAG, "Forced success bit for DownChar despite data error");
                }
            } else {
                event_type = EVENT_ERROR;
            }
            break;
        case FINGERPRINT_FUNCTION_ENCRYPTION_MISMATCH:
            // Add special error case for DownChar
            if (last_sent_command == FINGERPRINT_CMD_DOWN_CHAR) {
                ESP_LOGI(TAG, "DownChar command error response (status: 0x%02X)", status);
                // Log the exact response for debugging
                ESP_LOG_BUFFER_HEX("DownChar error response", packet->parameters, packet->length - 3);
                
                // IMPORTANT: Set the success bit anyway to unblock the download process
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
                    ESP_LOGI(TAG, "Forced success bit for DownChar despite error");
                }
            }
            
            // Rest of your error handling...
            break;
        case FINGERPRINT_FLASH_RW_ERROR:
        case FINGERPRINT_PORT_OP_FAIL:
        case FINGERPRINT_DB_CLEAR_FAIL:
        case FINGERPRINT_DB_RANGE_ERROR:
        case FINGERPRINT_READ_TEMPLATE_ERROR:
        case FINGERPRINT_UPLOAD_FEATURE_FAIL:
            ESP_LOGE(TAG, "Template upload failed with status 0x%02X - template may not exist", status);
            event_type = EVENT_TEMPLATE_UPLOAD_FAIL;
            
            // Make sure to set the event bits
            if (enroll_event_group) {
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
            }
            break;
        case FINGERPRINT_DELETE_TEMPLATE_FAIL:
        case FINGERPRINT_DB_EMPTY:
        case FINGERPRINT_ENTRY_COUNT_ERROR:
        case FINGERPRINT_ALREADY_EXISTS:
            ESP_LOGI(TAG, "Template exists at specified location");
            if(last_sent_command == FINGERPRINT_CMD_STORE_CHAR){
                event_type = EVENT_TEMPLATE_EXISTS;
            } else {
                event_type = EVENT_MATCH_FAIL;
            }
            if (enroll_event_group) {
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
            }
            break;
        case FINGERPRINT_MODULE_INFO_NOT_EMPTY:
        case FINGERPRINT_MODULE_INFO_EMPTY:
        case FINGERPRINT_OTP_FAIL:
        case FINGERPRINT_KEY_GEN_FAIL:
        case FINGERPRINT_KEY_NOT_EXIST:
        case FINGERPRINT_SECURITY_ALGO_FAIL:
        case FINGERPRINT_ENCRYPTION_MISMATCH:
        case FINGERPRINT_KEY_LOCKED:
            event_type = EVENT_ERROR;
            break;

        case FINGERPRINT_SENSOR_OP_FAIL:
            event_type = EVENT_SENSOR_ERROR;
            break;

        default:
            ESP_LOGW(TAG, "Unhandled status code: 0x%02X", status);
            if (status == 0x02) { // Scanner ready status
                event_type = EVENT_SCANNER_READY;
            } else {
                event_type = EVENT_ERROR;
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                }
            }
            break;
    }
    if(event_type != EVENT_NONE && g_fingerprint_event_handler){
        event.type = event_type;
        trigger_fingerprint_event(event);
    }

    // Log event group bits for debugging
    if (enroll_event_group) {
        EventBits_t bits = xEventGroupGetBits(enroll_event_group);
        ESP_LOGD(TAG, "Current event bits: 0x%02X", (unsigned int)bits);
    }
}

esp_err_t enroll_fingerprint(uint16_t location) {
    global_location = location; // Store location in global variable
    uint8_t attempts = 0;
    esp_err_t err;
    EventBits_t bits;
    bool finger_removed = false;

    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Clear any stale bits before starting
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL | CHECKING_LOCATION_BIT);
    
    // Set checking location flag
    xEventGroupSetBits(enroll_event_group, CHECKING_LOCATION_BIT);
    
    // First, validate if the location is available
    uint8_t page = location >> 8;  // Get the page number
    uint8_t position = location & 0xFF;  // Get position within page
    
    ESP_LOGI(TAG, "Checking if location %d is available (page %d, position %d)", location, page, position);
    
    // Clear any stale data in UART and queues
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    
    uint8_t index_params[] = {page};  // Use the calculated page number
    fingerprint_set_command(&PS_ReadIndexTable, FINGERPRINT_CMD_READ_INDEX_TABLE, 
                          index_params, sizeof(index_params));
    
    err = fingerprint_send_command(&PS_ReadIndexTable, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read index table");
        xEventGroupClearBits(enroll_event_group, CHECKING_LOCATION_BIT);
        goto cleanup;
    }

    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

    // Clear checking location flag
    xEventGroupClearBits(enroll_event_group, CHECKING_LOCATION_BIT);

    // Check if the location is occupied
    if (bits & ENROLL_BIT_FAIL) {
        ESP_LOGE(TAG, "Location %d is already occupied", location);
        goto cleanup;
    } else if (!(bits & ENROLL_BIT_SUCCESS)) {
        ESP_LOGE(TAG, "Failed to check if location is available (timeout)");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Location %d is available", location);

    // Set enrollment in progress flag to true
    enrollment_in_progress = true;

    while (attempts < 3) {
        // Set operation mode for first enrollment image
        fingerprint_set_operation_mode(FINGER_OP_ENROLL_FIRST);
        
        ESP_LOGI(TAG, "Waiting for a finger to be placed (via interrupt)...");
        
        // Clear states
        uart_flush(UART_NUM);
        xQueueReset(fingerprint_command_queue);
        xQueueReset(fingerprint_response_queue);
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);

        // Wait for finger detection and processing via interrupt
        err = fingerprint_wait_for_finger(30000); // 30 second timeout
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Timeout or error waiting for first finger placement");
            attempts++;
            continue;
        }
        
        ESP_LOGI(TAG, "First fingerprint image captured successfully!");

        ESP_LOGI(TAG, "Remove finger and place it again...");
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Wait for finger removal with improved reliability
        finger_removed = false;
        uint8_t no_finger_count = 0;
        uint32_t removal_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        while (!finger_removed) { 
            // Check for timeout on finger removal
            uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (current_time - removal_start_time > 10000) {  // 10 second timeout
                ESP_LOGW(TAG, "Timeout waiting for finger removal");
                break;
            }
            
            xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
            err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
            if (err != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            
            bits = xEventGroupWaitBits(enroll_event_group,
                                    ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(800));
            
            // Check specifically for ENROLL_BIT_FAIL which is set when NO_FINGER is detected
            if (bits & ENROLL_BIT_FAIL) {  
                no_finger_count++;
                if (no_finger_count >= 2) {  // Require multiple confirmations
                    finger_removed = true;
                    ESP_LOGI(TAG, "Finger removal confirmed");
                }
            } else {
                no_finger_count = 0;
                ESP_LOGI(TAG, "Please remove your finger...");
            }
            
            vTaskDelay(pdMS_TO_TICKS(300));  // Delay between checks
        }

        if (!finger_removed) {
            ESP_LOGW(TAG, "Finger not removed within timeout period");
            attempts++;
            continue;
        }

        // Set operation mode for second enrollment image
        fingerprint_set_operation_mode(FINGER_OP_ENROLL_SECOND);
        
        // Wait for second finger placement via interrupt
        ESP_LOGI(TAG, "Please place the same finger again (via interrupt)...");
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
        
        // Wait for finger detection and processing via interrupt
        err = fingerprint_wait_for_finger(30000); // 30 second timeout
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Timeout or error waiting for second finger placement");
            attempts++;
            continue;
        }
        
        ESP_LOGI(TAG, "Second fingerprint image captured successfully!");
        
        // Create model from the two fingerprint images
        err = fingerprint_send_command(&PS_RegModel, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send RegModel command");
            attempts++;
            continue;
        }
        
        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                                 
        if (!(bits & ENROLL_BIT_SUCCESS)) {
            ESP_LOGE(TAG, "Failed to create fingerprint model");
            attempts++;
            continue;
        }
        
        ESP_LOGI(TAG, "Fingerprint model created successfully!");

        // Check for duplicate fingerprint
        uint8_t search_params[] = {0x01,    // BufferID = 1
            0x00, 0x00, // Start page = 0
            0x00, 0x64}; // Number of pages = 100

        fingerprint_set_command(&PS_Search, FINGERPRINT_CMD_SEARCH, search_params, sizeof(search_params));
        err = fingerprint_send_command(&PS_Search, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            attempts++;
            continue;
        }

        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

        // Don't rely just on event bits - check the actual match score
        bool duplicate_found = false;
        fingerprint_response_t response;

        // Check if we received a valid response with match data
        if (xQueueReceive(fingerprint_response_queue, &response, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Extract match score
            uint16_t match_score = (response.packet.parameters[3] << 8) | response.packet.parameters[2];
            
            // Only consider it a duplicate if the match score is greater than 0
            if (match_score > 0) {
                uint16_t page_id = (response.packet.parameters[1] << 8) | response.packet.parameters[0];
                ESP_LOGE(TAG, "Fingerprint already exists in database! (ID: %d, Score: %d)",
                        convert_page_id_to_index(page_id), match_score);
                duplicate_found = true;
            } else {
                ESP_LOGI(TAG, "Search returned zero score, not a duplicate");
            }
        } else if (bits & ENROLL_BIT_SUCCESS) {
            // No response data but success bit set - be cautious and check
            ESP_LOGW(TAG, "Search returned success but no response data available");
            // If in doubt, continue with enrollment
        }

        if (duplicate_found) {
            attempts++;
            continue;
        }

        ESP_LOGI(TAG, "No duplicate found, continuing enrollment...");

        // Store template at specified location
        uint8_t store_params[] = {1, (uint8_t)(location >> 8), (uint8_t)(location & 0xFF)};
        fingerprint_set_command(&PS_StoreChar, FINGERPRINT_CMD_STORE_CHAR, store_params, 3);
        err = fingerprint_send_command(&PS_StoreChar, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            attempts++;
            continue;
        }

        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                                 
        if (bits & ENROLL_BIT_SUCCESS) {
            ESP_LOGI(TAG, "Fingerprint enrolled successfully!");
            // Reset operation mode
            fingerprint_set_operation_mode(FINGER_OP_NONE);
            enrollment_in_progress = false;
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
            return ESP_OK;
        }
        
        attempts++;
    }

    ESP_LOGE(TAG, "Enrollment failed after %d attempts", attempts);
    
cleanup:
    // Reset operation mode
    fingerprint_set_operation_mode(FINGER_OP_NONE);
    enrollment_in_progress = false;
    if (enroll_event_group) {
        vEventGroupDelete(enroll_event_group);
        enroll_event_group = NULL;
    }
    return ESP_FAIL;
}

esp_err_t verify_fingerprint(void) {
    esp_err_t err;
    uint8_t attempts = 0;
    const uint8_t max_attempts = 3;

    // Reset match cooldown at the start of verification
    match_cooldown_active = false;

    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    while (attempts < max_attempts) {
        // Clear queues and event bits
        uart_flush(UART_NUM);
        xQueueReset(fingerprint_command_queue);
        xQueueReset(fingerprint_response_queue);
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
        
        // Wait for finger detection
        ESP_LOGI(TAG, "Please place your finger on the sensor...");
        err = fingerprint_wait_for_finger(30000);
        if (err != ESP_OK) {
            attempts++;
            continue;
        }
        
        // Run search
        uint8_t search_params[] = {0x01, 0x00, 0x00, 0x00, 0x64};
        fingerprint_set_command(&PS_Search, FINGERPRINT_CMD_SEARCH, search_params, sizeof(search_params));
        err = fingerprint_send_command(&PS_Search, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            attempts++;
            continue;
        }
        
        // Wait for the event handler to process results
        // Use a longer timeout to allow all responses to be processed
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Wait for event bits to be set
        // ENROLL_BIT_SUCCESS will be set when a real match with score > 0 is found
        EventBits_t bits = xEventGroupWaitBits(enroll_event_group,
                                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                             pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
        
        if (bits & ENROLL_BIT_SUCCESS) {
            // The event handler set the success bit for a real match
            ESP_LOGI(TAG, "Fingerprint verification successful!");
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
            // Reset cooldown flag after successful verification
            match_cooldown_active = false;
            return ESP_OK;
        }
        
        ESP_LOGW(TAG, "No match found, attempt %d of %d", attempts + 1, max_attempts);
        attempts++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "Verification failed after %d attempts", attempts);
    vEventGroupDelete(enroll_event_group);
    enroll_event_group = NULL;
    // Reset cooldown flag after failed verification
    match_cooldown_active = false;
    return ESP_FAIL;
}

esp_err_t delete_fingerprint(uint16_t location) {
    esp_err_t err;
    EventBits_t bits;

    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Clear states
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);

    // Set up delete parameters (Page ID and number of templates to delete)
    uint8_t delete_params[] = {
        (uint8_t)(location >> 8),    // Page ID high byte
        (uint8_t)(location & 0xFF),  // Page ID low byte
        0x00, 0x01                   // Delete 1 template
    };
    
    fingerprint_set_command(&PS_DeleteChar, FINGERPRINT_CMD_DELETE_CHAR, delete_params, sizeof(delete_params));
    err = fingerprint_send_command(&PS_DeleteChar, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    
    if (!(bits & ENROLL_BIT_SUCCESS)) {
        ESP_LOGE(TAG, "Failed to delete fingerprint at location %d", location);
        err = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Successfully deleted fingerprint at location %d", location);
    err = ESP_OK;

cleanup:
    vEventGroupDelete(enroll_event_group);
    enroll_event_group = NULL;
    return err;
}

esp_err_t clear_database(void) {
    esp_err_t err;
    EventBits_t bits;

    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Clearing fingerprint database...");

    // Clear states
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);

    // Send empty database command
    err = fingerprint_send_command(&PS_Empty, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(5000)); // Longer timeout for database clear
    
    if (!(bits & ENROLL_BIT_SUCCESS)) {
        ESP_LOGE(TAG, "Failed to clear fingerprint database");
        err = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Successfully cleared fingerprint database");
    err = ESP_OK;

cleanup:
    vEventGroupDelete(enroll_event_group);
    enroll_event_group = NULL;
    return err;
}

esp_err_t get_enrolled_count(void) {
    esp_err_t err;
    EventBits_t bits;
    fingerprint_response_t response;
    
    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Clear states
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);

    // Send command to get template count
    err = fingerprint_send_command(&PS_ValidTempleteNum, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) return err;

    // Wait for response
    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

    vEventGroupDelete(enroll_event_group);
    enroll_event_group = NULL;

    if (bits & ENROLL_BIT_SUCCESS) {
        // // Get response from queue
        // if (xQueueReceive(fingerprint_response_queue, &response, pdMS_TO_TICKS(1000)) == pdTRUE) {
        //     // *count = (response.packet.parameters[0] << 8) | 
        //     //          response.packet.parameters[1];
        //     vEventGroupDelete(enroll_event_group);
        //     enroll_event_group = NULL;
        //     return ESP_OK;
        // }
        return ESP_OK;
    }

    return ESP_FAIL;
}

uint16_t convert_page_id_to_index(uint16_t page_id) {
    return page_id / 256;
}

uint16_t convert_index_to_page_id(uint16_t index) {
    return index * 256;
}

/**
 * @brief Reads system parameters from the fingerprint module.
 */
esp_err_t read_system_parameters(void) {
    ESP_LOGI(TAG, "Reading system parameters...");
    esp_err_t err = fingerprint_send_command(&PS_ReadSysPara, DEFAULT_FINGERPRINT_ADDRESS);
    return err;
}

/// Helper function to load template from flash to buffer
esp_err_t load_template_to_buffer(uint16_t template_id, uint8_t buffer_id) {
    esp_err_t err;
    EventBits_t bits;
    // uint16_t page_id = convert_index_to_page_id(template_id);
    uint16_t page_id = template_id;
    
    // Parameters: BufferID, Page Address (2 bytes)
    uint8_t params[] = {
        buffer_id,
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xFF)
    };
    
    fingerprint_set_command(&PS_LoadChar, FINGERPRINT_CMD_LOAD_CHAR, params, sizeof(params));
    err = fingerprint_send_command(&PS_LoadChar, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "LoadChar command sent");
    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
                             
    return (bits & ENROLL_BIT_SUCCESS) ? ESP_OK : ESP_FAIL;
}

// Upload template from module buffer to host
// esp_err_t upload_template(uint8_t buffer_id, uint8_t* template_data, size_t* template_size) {
//     esp_err_t err;
//     EventBits_t bits;
//     size_t total_size = 0;
    
//     // First send upload command
//     uint8_t params[] = {buffer_id};
//     fingerprint_set_command(&PS_UpChar, FINGERPRINT_CMD_UP_CHAR, params, sizeof(params));
//     err = fingerprint_send_command(&PS_UpChar, DEFAULT_FINGERPRINT_ADDRESS);
//     if (err != ESP_OK) return err;

//     // Wait for acknowledgment and subsequent data packets
//     bits = xEventGroupWaitBits(enroll_event_group,
//                              ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
//                              pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                             
//     if (!(bits & ENROLL_BIT_SUCCESS)) {
//         ESP_LOGE(TAG, "Failed to start template upload");
//         return ESP_FAIL;
//     }

//     // The actual template data will be handled by process_response_task
//     // which will trigger appropriate events with the template data

//     return ESP_OK;
// }

esp_err_t upload_template(uint8_t buffer_id, uint8_t *template_data, size_t *template_size) {
    esp_err_t err;
    EventBits_t bits;
    
    // Validate parameters
    if (template_data == NULL || template_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Start with zero size
    *template_size = 0;
    
    // Parameters: BufferID
    uint8_t params[] = {buffer_id};
    
    ESP_LOGI(TAG, "Sending UpChar command for buffer %d", buffer_id);
    
    fingerprint_set_command(&PS_UpChar, FINGERPRINT_CMD_UP_CHAR, params, sizeof(params));
    err = fingerprint_send_command(&PS_UpChar, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) return err;
    
    ESP_LOGI(TAG, "Waiting for template data transfer...");
    
    // Wait for initial confirmation (0x00 means data packets will follow)
    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
                             
    // Check for error response (0x0D = invalid template)
    fingerprint_response_t last_response;
    if (xQueuePeek(fingerprint_response_queue, &last_response, 0) == pdTRUE) {
        if (last_response.packet.code.confirmation != 0x00) {
            ESP_LOGE(TAG, "Template upload failed with error: 0x%02X", 
                    last_response.packet.code.confirmation);
            return ESP_FAIL;
        }
    }
    
    // If we get here, processing should continue as normal for successful uploads
    
    // Wait for the template data to be fully uploaded by the module
    // (this will be processed by the UART receive handler and event system)
    bits = xEventGroupWaitBits(enroll_event_group,
                             TEMPLATE_UPLOAD_COMPLETE_BIT,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    
    ESP_LOGI(TAG, "Template upload should be complete");
    
    // Check if template was successfully received
    if (g_template_accumulator != NULL && 
        g_template_accumulator->template_data != NULL &&
        g_template_accumulator->template_size > 0) {
        
        // Copy the template data
        size_t copy_size = min(g_template_accumulator->template_size, 512);
        memcpy(template_data, g_template_accumulator->template_data, copy_size);
        *template_size = copy_size;
        
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "No template data available after upload complete");
        return ESP_ERR_NOT_FOUND;
    }
}

/**
 * @brief Checks if a template exists at the specified location
 * 
 * This function attempts to upload a template from the specified location.
 * If the template exists, the function returns ESP_OK.
 * If the template doesn't exist, the function returns ESP_FAIL.
 * 
 * @param template_id The ID of the template to check
 * @return ESP_OK if template exists, ESP_FAIL if not, or other error code
 */
// Fix the fingerprint_check_template_exists function to properly handle the event group
esp_err_t fingerprint_check_template_exists(uint16_t template_id) {
    esp_err_t err;
    bool created_event_group = false;
    
    // Create event group for this operation if needed
    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create event group for template check");
            return ESP_ERR_NO_MEM;
        }
        created_event_group = true;
    }
    
    // First try to read the index table to check if the template exists
    uint8_t page = template_id >> 8;  // Get the page number
    uint8_t position = template_id & 0xFF;  // Get position within page
    
    ESP_LOGI(TAG, "Checking if template %d exists (page %d, position %d)", template_id, page, position);
    
    // Clear any stale data
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
    
    // Read the index table for the page
    uint8_t index_params[] = {page};
    fingerprint_set_command(&PS_ReadIndexTable, FINGERPRINT_CMD_READ_INDEX_TABLE, 
                          index_params, sizeof(index_params));
    
    err = fingerprint_send_command(&PS_ReadIndexTable, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send ReadIndexTable command");
        if (created_event_group) {
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
        }
        return err;
    }
    
    // Wait for response with timeout
    EventBits_t bits = xEventGroupWaitBits(enroll_event_group,
                                         ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                         pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    
    if (bits & ENROLL_BIT_SUCCESS) {
        // Check if the bit corresponding to this template is set in the index table
        // This requires accessing the response packet directly
        fingerprint_response_t response;
        bool template_exists = false;
        
        if (xQueueReceive(fingerprint_response_queue, &response, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint8_t bit_position = position % 8;
            uint8_t byte_offset = position / 8;
            
            if (byte_offset < 32 && response.packet.parameters[byte_offset] & (1 << bit_position)) {
                template_exists = true;
                ESP_LOGI(TAG, "Template %d exists according to index table", template_id);
            } else {
                ESP_LOGI(TAG, "Template %d does not exist according to index table", template_id);
            }
        }
        
        // Don't delete the event group here if we didn't create it
        if (created_event_group) {
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
        }
        
        return template_exists ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    
    // If reading index table failed, try loading the template as a fallback
    ESP_LOGW(TAG, "Reading index table failed, trying to load template directly");
    
    // Clear any stale data again
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
    
    // Try to load the template to buffer 1
    uint8_t load_params[] = {
        1,  // Buffer ID = 1
        (uint8_t)(template_id >> 8),
        (uint8_t)(template_id & 0xFF)
    };
    
    fingerprint_set_command(&PS_LoadChar, FINGERPRINT_CMD_LOAD_CHAR, load_params, sizeof(load_params));
    err = fingerprint_send_command(&PS_LoadChar, DEFAULT_FINGERPRINT_ADDRESS);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send LoadChar command");
        if (created_event_group) {
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
        }
        return err;
    }
    
    // Wait for response with timeout
    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    
    // Clean up only if we created the event group
    if (created_event_group) {
        vEventGroupDelete(enroll_event_group);
        enroll_event_group = NULL;
    }
    
    // Interpret results
    if (bits & ENROLL_BIT_SUCCESS) {
        ESP_LOGI(TAG, "Template %d exists (load successful)", template_id);
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "Template %d does not exist (load failed)", template_id);
        return ESP_ERR_NOT_FOUND;
    }
}

// Download template from host to module buffer
esp_err_t download_template(uint8_t buffer_id, const uint8_t* template_data, size_t template_size) {
    esp_err_t err;
    EventBits_t bits;

    // 1. First send the download command
    uint8_t params[] = {buffer_id};
    fingerprint_set_command(&PS_DownChar, FINGERPRINT_CMD_DOWN_CHAR, params, sizeof(params));
    err = fingerprint_send_command(&PS_DownChar, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) return err;
    
    // 2. Wait for acknowledgment (confirmation code = 0x00)
    if (enroll_event_group != NULL) {
        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
                                 
        if (!(bits & ENROLL_BIT_SUCCESS)) {
            ESP_LOGE(TAG, "Module not ready to receive template data");
            return ESP_FAIL;
        }
    }
    
    // 3. Send data in larger chunks - match packet size with what the module expects
    const size_t CHUNK_SIZE = 128;  // Use this larger chunk size
    size_t remaining = template_size;
    size_t offset = 0;
    
    // 4. Send all but the last chunk with packet_id=0x02
    while (remaining > CHUNK_SIZE) {
        FingerprintPacket data_packet = {
            .header = FINGERPRINT_PACKET_HEADER,
            .address = DEFAULT_FINGERPRINT_ADDRESS,
            .packet_id = 0x02,  // Standard data packet (more will follow)
            .length = CHUNK_SIZE + 2,
            .code.command = 0x00
        };
        
        memcpy(data_packet.parameters, template_data + offset, CHUNK_SIZE);
        data_packet.checksum = fingerprint_calculate_checksum(&data_packet);
        
        err = fingerprint_send_command(&data_packet, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send template data chunk");
            return err;
        }
        
        // Short delay between chunks
        vTaskDelay(pdMS_TO_TICKS(10));
        
        offset += CHUNK_SIZE;
        remaining -= CHUNK_SIZE;
    }
    
    // 5. Send final chunk with packet_id=0x08
    if (remaining > 0) {
        FingerprintPacket final_packet = {
            .header = FINGERPRINT_PACKET_HEADER,
            .address = DEFAULT_FINGERPRINT_ADDRESS,
            .packet_id = 0x08,  // Final data packet
            .length = remaining + 2,
            .code.command = 0x00
        };
        
        memcpy(final_packet.parameters, template_data + offset, remaining);
        final_packet.checksum = fingerprint_calculate_checksum(&final_packet);
        
        err = fingerprint_send_command(&final_packet, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send final template data chunk");
            return err;
        }
    } else {
        // Send an empty final packet if the data aligned perfectly with chunk size
        FingerprintPacket final_packet = {
            .header = FINGERPRINT_PACKET_HEADER,
            .address = DEFAULT_FINGERPRINT_ADDRESS,
            .packet_id = 0x08,  // Final data packet
            .length = 2,
            .code.command = 0x00
        };
        
        final_packet.checksum = fingerprint_calculate_checksum(&final_packet);
        
        err = fingerprint_send_command(&final_packet, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send empty final packet");
            return err;
        }
    }
    
    // Short delay to let the module process the data
    vTaskDelay(pdMS_TO_TICKS(100));
    
    return ESP_OK;
}

// Store template from buffer to flash memory
esp_err_t store_template(uint8_t buffer_id, uint16_t template_id) {
    esp_err_t err;
    EventBits_t bits;
    uint16_t page_id = convert_index_to_page_id(template_id);
    
    // Parameters: BufferID, Page Address (2 bytes)
    uint8_t params[] = {
        buffer_id,
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xFF)
    };
    
    fingerprint_set_command(&PS_StoreChar, FINGERPRINT_CMD_STORE_CHAR, params, sizeof(params));
    err = fingerprint_send_command(&PS_StoreChar, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) return err;

    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
                             
    return (bits & ENROLL_BIT_SUCCESS) ? ESP_OK : ESP_FAIL;
}

// Example usage for backing up a template:
esp_err_t backup_template(uint16_t template_id) {
    esp_err_t err;
    uint8_t template_data[512];  // Buffer for template data
    size_t template_size = 0;
    bool template_valid = false;

    // Create event group at the start of backup process
    err = initialize_event_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize event group");
        return err;
    }
    
    ESP_LOGI(TAG, "%d. Backing up template id 0x%04X", template_id, template_id);
    
    // 1. Load template from flash to buffer 1
    err = load_template_to_buffer(template_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load template");
        cleanup_event_group();
        return err;
    }
    
    ESP_LOGI(TAG, "Loading Template Successful");
    
    // 2. Upload template from buffer to host
    ESP_LOGI(TAG, "Uploading Template...");
    err = upload_template(1, template_data, &template_size);
    
    // Check explicit upload template error codes
    if (err == ESP_OK) {
        // Check if we actually received template data
        if (template_size == 0) {
            ESP_LOGE(TAG, "No template data received despite success status");
            cleanup_event_group();
            return ESP_ERR_INVALID_STATE;
        }
        
        ESP_LOGI(TAG, "Template backup successful");
    } else {
        // Check for specific error code 0x0D which means "template not found"
        fingerprint_response_t last_response;
        if (xQueuePeek(fingerprint_response_queue, &last_response, 0) == pdTRUE) {
            if (last_response.packet.code.confirmation == 0x0D) {
                ESP_LOGE(TAG, "Location %d is empty (error 0x0D)", template_id);
                cleanup_event_group();
                return ESP_ERR_NOT_FOUND;
            }
        }
        
        ESP_LOGE(TAG, "Failed to upload template: %s", esp_err_to_name(err));
        cleanup_event_group();
        return err;
    }
    
    cleanup_event_group();
    return ESP_OK;
}

// Example usage for restoring a template:
esp_err_t restore_template(uint16_t template_id, const uint8_t* template_data, size_t template_size) {
    // Validate input data
    if (template_data == NULL || template_size == 0 || template_size > 512) {
        ESP_LOGE(TAG, "Invalid template data: %p, size: %d", template_data, template_size);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create our own event group specifically for this operation
    EventGroupHandle_t restore_event_group = xEventGroupCreate();
    if (restore_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group for restore_template");
        return ESP_ERR_NO_MEM;
    }
    
    // Set the global event group to our local one for this operation
    enroll_event_group = restore_event_group;
    
    // Check if template appears valid (contains FOOF marker)
    bool valid_template = false;
    for (size_t i = 0; i < template_size - 4; i++) {
        if (template_data[i] == 'F' && template_data[i+1] == 'O' && 
            template_data[i+2] == 'O' && template_data[i+3] == 'F') {
            valid_template = true;
            ESP_LOGI(TAG, "Found template validation marker (FOOF) at offset %d", i);
            break;
        }
    }
    
    if (!valid_template) {
        ESP_LOGW(TAG, "Template data may be invalid (no FOOF marker found)");
    }
    
    // 1. Download template to buffer 1
    esp_err_t err = download_template(1, template_data, template_size);
    if (err != ESP_OK) {
        vEventGroupDelete(restore_event_group);
        enroll_event_group = NULL;
        return err;
    }
    
    // 2. Store template from buffer to flash
    err = store_template(1, template_id);
    
    // Clean up
    vEventGroupDelete(restore_event_group);
    enroll_event_group = NULL;
    
    return err;
}
esp_err_t initialize_event_group(void) {
    // Check if event group already exists
    if (enroll_event_group != NULL) {
        // Just clear the bits and return success
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
        return ESP_OK;
    }
    
    // Create a new event group
    enroll_event_group = xEventGroupCreate();
    if (enroll_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }
    
    // Clear any existing bits to ensure a clean state
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
    ESP_LOGD(TAG, "Event group initialized successfully");
    return ESP_OK;
}
esp_err_t cleanup_event_group(void) {
    if (enroll_event_group == NULL) {
        ESP_LOGW(TAG, "Event group is already NULL.");
        return ESP_ERR_INVALID_STATE;  // Indicates it was already deleted
    }

    // Delete the event group and set to NULL
    EventGroupHandle_t temp = enroll_event_group;
    enroll_event_group = NULL;  // Set to NULL BEFORE deleting to prevent race conditions
    vEventGroupDelete(temp);
    
    ESP_LOGD(TAG, "Enrollment event group deleted successfully.");
    return ESP_OK;
}

esp_err_t read_info_page(void) {
    esp_err_t err;
    EventBits_t bits;
    bool info_complete = false;
    uint8_t packet_count = 0;
    const uint8_t MAX_PACKETS = 32;  // 512 bytes at 16 bytes per packet
    
    err = initialize_event_group();
    if (err != ESP_OK) {
        return err;
    }

    // Clear any stale data
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);

    // Send ReadINFPage command (0x16)
    fingerprint_set_command(&PS_ReadINFPage, FINGERPRINT_CMD_READ_INF_PAGE, NULL, 0);
    err = fingerprint_send_command(&PS_ReadINFPage, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send ReadINFPage command");
        cleanup_event_group();
        return err;
    }

    // Wait for initial confirmation (0x00 means data packets will follow)
    bits = xEventGroupWaitBits(enroll_event_group,
                              ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                              pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

    if (!(bits & ENROLL_BIT_SUCCESS)) {
        ESP_LOGE(TAG, "Failed to read information page");
        cleanup_event_group();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initial acknowledgment received, waiting for data packets...");

    // The actual data packets will be handled by process_response_task
    // We'll wait here until either we get all the data or timeout
    while (!info_complete && packet_count < MAX_PACKETS) {
        bits = xEventGroupWaitBits(enroll_event_group,
                                  ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                  pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        if (bits & ENROLL_BIT_SUCCESS) {
            packet_count++;
            if (packet_count == 1) {
                ESP_LOGI(TAG, "Started receiving info page data packets");
            }
        } else {
            ESP_LOGW(TAG, "Timeout waiting for data packet %d", packet_count + 1);
            if (packet_count == 0) {
                cleanup_event_group();
                return ESP_ERR_TIMEOUT;
            }
            break;
        }

        // Check if this was the final packet (0x08)
        fingerprint_response_t response;
        if (xQueuePeek(fingerprint_response_queue, &response, 0) == pdTRUE) {
            if (response.packet.packet_id == 0x08) {
                info_complete = true;
                ESP_LOGI(TAG, "Received complete information page (%d packets)", packet_count);
                break;
            }
        }
    }

    cleanup_event_group();
    return info_complete ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/**
 * @brief Restores a fingerprint template from a MultiPacketResponse structure
 *
 * This function takes a template stored in a MultiPacketResponse structure (as received during
 * template upload), and downloads it to the fingerprint module, storing it at the specified location.
 *
 * @param template_id The ID where to store the template in the fingerprint database
 * @param response Pointer to the MultiPacketResponse structure containing template data
 * @return ESP_OK on success, or appropriate error code on failure
 */
esp_err_t restore_template_from_multipacket(uint16_t template_id, MultiPacketResponse *response) {
    // Save previous event group and track our own
    EventGroupHandle_t previous_event_group = enroll_event_group;
    EventGroupHandle_t restore_event_group = NULL;
    esp_err_t err;
    bool created_event_group = false;
    
    // Validate input parameters
    if (response == NULL) {
        ESP_LOGE(TAG, "Invalid MultiPacketResponse (NULL)");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (response->template_data == NULL || response->template_size == 0) {
        ESP_LOGE(TAG, "No template data available in MultiPacketResponse");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Log template information
    ESP_LOGI(TAG, "Preparing to download template (size: %d bytes) to location %d", 
             response->template_size, template_id);
    
    // Check template for validity (FOOF marker or minimum size)
    bool valid_template = false;
    size_t marker_pos = 0;
    
    // Look for FOOF validation marker
    for (size_t i = 0; i < response->template_size - 4; i++) {
        if (response->template_data[i] == 'F' && response->template_data[i+1] == 'O' && 
            response->template_data[i+2] == 'O' && response->template_data[i+3] == 'F') {
            valid_template = true;
            marker_pos = i + 4;  // Position right after FOOF
            ESP_LOGI(TAG, "Found template validation marker at offset %d", i);
            break;
        }
    }
    
    // Create a specific event group for this operation
    restore_event_group = xEventGroupCreate();
    if (restore_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group for template restore");
        return ESP_ERR_NO_MEM;
    }
    
    created_event_group = true;
    enroll_event_group = restore_event_group;
    
    // Add a longer delay before attempting to download the template
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Reset any pending UART data and queues
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    
    // Make sure we have at least 16 bytes after the FOOF marker for proper padding
    uint8_t* download_data = response->template_data;
    size_t download_size = response->template_size;
    uint8_t* extended_data = NULL;
    
    if (valid_template) {
        size_t proper_size = marker_pos + 16;
        if (proper_size > response->template_size) {
            // Need to extend the template data for proper padding
            extended_data = heap_caps_malloc(proper_size, MALLOC_CAP_8BIT);
            if (extended_data) {
                // Copy existing data
                memcpy(extended_data, response->template_data, response->template_size);
                // Add padding zeros
                memset(extended_data + response->template_size, 0, proper_size - response->template_size);
                
                download_data = extended_data;
                download_size = proper_size;
                
                ESP_LOGI(TAG, "Extended template from %d to %d bytes for proper padding", 
                         response->template_size, proper_size);
            }
        }
    }
    
    // Download template to buffer 1
    ESP_LOGI(TAG, "Downloading template to buffer 1...");
    err = download_template(1, download_data, download_size);
    
    // Free extended data if we allocated it
    if (extended_data) {
        heap_caps_free(extended_data);
        extended_data = NULL;
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to download template to buffer: %s", esp_err_to_name(err));
        goto cleanup;
    }
    
    // Add delay between download and store operations
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Store template from buffer to flash at specified location
    ESP_LOGI(TAG, "Storing template to location %d...", template_id);
    err = store_template(1, template_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store template: %s", esp_err_to_name(err));
        goto cleanup;
    } else {
        ESP_LOGI(TAG, "Template successfully stored at location %d", template_id);
    }
    
cleanup:
    // IMPORTANT: First restore the previous event group, then delete our local one
    if (created_event_group) {
        EventGroupHandle_t temp = restore_event_group;
        enroll_event_group = previous_event_group;  // Restore previous event group
        
        if (temp != NULL) {
            vEventGroupDelete(temp);  // Delete our temporary group
        }
    }
    
    return err;
}

esp_err_t fingerprint_set_operation_mode(finger_operation_mode_t mode) {
    // Create mutex if it doesn't exist
    if (finger_op_mutex == NULL) {
        finger_op_mutex = xSemaphoreCreateMutex();
        if (finger_op_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create operation mode mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Set the operation mode with mutex protection
    if (xSemaphoreTake(finger_op_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        current_operation = mode;
        xSemaphoreGive(finger_op_mutex);
        ESP_LOGI(TAG, "Fingerprint operation mode set to %d", mode);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to take mutex for setting operation mode");
        return ESP_ERR_TIMEOUT;
    }
}

finger_operation_mode_t fingerprint_get_operation_mode(void) {
    finger_operation_mode_t mode = FINGER_OP_NONE;
    
    // Get the operation mode with mutex protection
    if (finger_op_mutex != NULL && xSemaphoreTake(finger_op_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        mode = current_operation;
        xSemaphoreGive(finger_op_mutex);
    }
    
    return mode;
}

esp_err_t fingerprint_wait_for_finger(uint32_t timeout_ms) {
    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create event group for wait_for_finger");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Clear any previous bits
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
    
    // Start time for timeout tracking
    uint32_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t current_time;
    bool success = false;
    
    // Wait for finger with active polling as a backup to the interrupt
    while (1) {
        // Check if we've exceeded timeout
        current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (timeout_ms > 0 && (current_time - start_time) > timeout_ms) {
            ESP_LOGW(TAG, "Timeout waiting for finger placement");
            return ESP_ERR_TIMEOUT;
        }
        
        // Check if event bits were set by interrupt handler
        EventBits_t bits = xEventGroupGetBits(enroll_event_group);
        if (bits & ENROLL_BIT_SUCCESS) {
            success = true;
            break;
        } else if (bits & ENROLL_BIT_FAIL) {
            ESP_LOGW(TAG, "Finger detection failed");
            return ESP_FAIL;
        }
        
        // If no interrupt has triggered, periodically check for finger directly
        // This provides a backup method if the interrupt isn't working reliably
        if ((current_time - start_time) % 1000 == 0) {  // Check every second
            // Only do this if we're not already processing
            if (!is_fingerprint_validating && xSemaphoreTake(finger_detect_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                ESP_LOGI(TAG, "Polling for finger presence...");
                
                // Try to capture image
                esp_err_t err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
                if (err == ESP_OK) {
                    // Wait for response
                    bits = xEventGroupWaitBits(enroll_event_group,
                                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                             pdTRUE, pdFALSE, pdMS_TO_TICKS(800));
                    
                    if (bits & ENROLL_BIT_SUCCESS) {
                        ESP_LOGI(TAG, "Finger detected through polling");
                        success = true;
                        xSemaphoreGive(finger_detect_mutex);
                        break;
                    }
                }
                xSemaphoreGive(finger_detect_mutex);
            }
        }
        
        // Short delay to prevent CPU hogging
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    return success ? ESP_OK : ESP_FAIL;
}

// Function to control power to fingerprint module
esp_err_t fingerprint_power_control(bool power_on) {
    // Configure GPIO for output if not already done
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FINGERPRINT_VIN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure VIN control pin: %s", esp_err_to_name(err));
        return err;
    }
    
    // Set pin level according to power_on parameter
    gpio_set_level(FINGERPRINT_VIN_PIN, power_on ? 1 : 0);
    ESP_LOGI(TAG, "Fingerprint module power %s", power_on ? "ON" : "OFF");
    
    if (power_on) {
        // If powering on, add longer delay for module to initialize
        vTaskDelay(pdMS_TO_TICKS(800));  // Increased from 500ms to 800ms
    } else {
        // CRITICAL FIX: Add delay after powering OFF to ensure proper discharge
        vTaskDelay(pdMS_TO_TICKS(500));  // Need at least 500ms for capacitors to discharge
    }
    
    return ESP_OK;
}
