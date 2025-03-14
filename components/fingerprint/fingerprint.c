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
#define UART_NUM UART_NUM_1  // Change based on your wiring
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
static QueueHandle_t fingerprint_response_queue = NULL;
static QueueHandle_t fingerprint_command_queue = NULL;
static QueueHandle_t finger_detected_queue = NULL;

bool enrollment_in_progress = false;

// ISR handler for fingerprint detection interrupt, sending detection signal to the queue.
void IRAM_ATTR finger_detected_isr(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint8_t finger_detected = 1; // Flag for finger detection
    xQueueSendFromISR(finger_detected_queue, &finger_detected, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken); // Yield if needed
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

    // UART configuration
    uart_config_t uart_config = {
        .baud_rate = baud_rate,  // Use provided DEFAULT_BAUD_RATE
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    esp_err_t err;

    // Set UART pins first
    err = uart_set_pin(UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
        return err;
    }

    // Now configure UART
    err = uart_param_config(UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART");
        return err;
    }

    // Install UART driver
    err = uart_driver_install(UART_NUM, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver");
        return err;
    }

    // Prevent Reinitialization
    if (fingerprint_task_handle != NULL) {
        ESP_LOGW(TAG, "Fingerprint library already initialized.");
        return ESP_OK;
    }

    // // Handshake detection (0x55)
    // uint8_t handshake;
    // int length = uart_read_bytes(UART_NUM, &handshake, 1, pdMS_TO_TICKS(500));  // Timeout 200ms

    // if (length > 0 && handshake == 0x55) {
    //     ESP_LOGI(TAG, "Fingerprint module handshake received.");
    // } else {
    //     ESP_LOGW(TAG, "No handshake received, waiting 200ms before communication.");
    //     vTaskDelay(pdMS_TO_TICKS(200));  // Delay to ensure module is ready
    // }
    // Handshake detection loop (0x55)
    uint8_t handshake;
    int length;
    while (1) {
        length = uart_read_bytes(UART_NUM, &handshake, 1, pdMS_TO_TICKS(200));  // Timeout 500ms
        if (length > 0 && handshake == 0x55) {
            ESP_LOGI(TAG, "Fingerprint module handshake received: 0x%02X", handshake);
            break;  // Exit the loop once handshake is received
        } else {
            ESP_LOGW(TAG, "No handshake received, retrying...");
            vTaskDelay(pdMS_TO_TICKS(200));  // Delay to ensure module is ready before retrying
        }
    }

    // Create Queue for Response Handling
    fingerprint_response_queue = xQueueCreate(QUEUE_SIZE, sizeof(fingerprint_response_t));
    if (fingerprint_response_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create response queue");
        return ESP_FAIL;
    }

    // Create Queue for Storing Sent Commands
    fingerprint_command_queue = xQueueCreate(QUEUE_SIZE, sizeof(fingerprint_command_info_t));
    if (fingerprint_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_FAIL;
    }


    // // Create Response Handling Task
    // if (xTaskCreate(read_response_task, "FingerprintReadResponse", 4096, NULL, 5, &fingerprint_task_handle) != pdPASS) {
    //     ESP_LOGE(TAG, "Failed to create fingerprint response task");
    //     return ESP_FAIL;
    // }

    // Create Response Handling Task
    if (xTaskCreate(read_response_task, "FingerprintReadResponse", 4096, NULL, configMAX_PRIORITIES - 2, &fingerprint_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fingerprint response task");
        return ESP_FAIL;
    }

    // Create Response Handling Task
    if (xTaskCreate(process_response_task, "FingerprintProcessResponse", 4096, NULL, configMAX_PRIORITIES - 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fingerprint response task");
        return ESP_FAIL;
    }

    // // Create Processing Task (processes responses from queue)
    // if (xTaskCreate(process_fingerprint_responses_task, "FingerprintProcessResponse", 4096, NULL, 5, NULL) != pdPASS) {
    //     ESP_LOGE(TAG, "Failed to create fingerprint processing task");
    //     return ESP_FAIL;
    // }

    // Create the queue for finger detection
    finger_detected_queue = xQueueCreate(10, sizeof(uint8_t));
    if (finger_detected_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_FAIL;
    }

    // Install GPIO ISR service
    esp_err_t gpio_ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (gpio_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(gpio_ret));
        return gpio_ret;
    }

    // Initialize GPIO for interrupt
    gpio_set_direction(FINGERPRINT_GPIO_PIN, GPIO_MODE_INPUT);
    gpio_set_intr_type(FINGERPRINT_GPIO_PIN, GPIO_INTR_POSEDGE); // Trigger on rising edge
    gpio_isr_handler_add(FINGERPRINT_GPIO_PIN, finger_detected_isr, NULL);

    // // Create tasks
    // if (xTaskCreate(finger_detected_task, "finger_detected_task", 4096, NULL, 10, NULL) != pdPASS) {
    //     ESP_LOGE(TAG, "Failed to create finger_detected_task");
    //     return ESP_FAIL;
    // }

    // if (xTaskCreate(detect_fingerprint_uart_task, "uart_read_task", 4096, NULL, 10, NULL) != pdPASS) {
    //     ESP_LOGE(TAG, "Failed to create uart_read_task");
    //     return ESP_FAIL;
    // }

    // Initialize Fingerprint Commands
    uint8_t buffer_id1 = 0x01;  // Buffer ID for CharBuffer1
    uint8_t buffer_id2 = 0x02;  // Buffer ID for CharBuffer2

    fingerprint_set_command(&PS_GenChar1, FINGERPRINT_CMD_GEN_CHAR, &buffer_id1, 1);
    fingerprint_set_command(&PS_GenChar2, FINGERPRINT_CMD_GEN_CHAR, &buffer_id2, 1);

    // enroll_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Fingerprint scanner initialized successfully.");
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

MultiPacketResponse* fingerprint_read_response(void) {
    static ParserState state = WAIT_HEADER;
    static size_t content_length = 0;
    static size_t bytes_needed = 0;
    static FingerprintPacket current_packet = {0};
    static uint8_t buffer[256] = {0};  // Buffer size of 256 works well (prevents multiple FOOF detections)
    static size_t buffer_pos = 0;
    
    // Track template processing state globally within the function
    static bool template_processed = false;  // Track if we've already processed this template
    static bool final_packet_sent = false;   // Track if we've already sent a final packet
    static uint32_t last_template_time = 0;  // Track when we last processed a template
    
    // Track buffer change detection to avoid stuck buffers
    static uint32_t last_buffer_change_time = 0;
    static size_t last_buffer_size = 0;
    
    // Special handling for template upload
    bool is_template_upload = (last_sent_command == FINGERPRINT_CMD_UP_CHAR);
    int timeout = is_template_upload ? 1500 : 200;  // Longer timeout for template data
    
    // Reset template flags after timeout
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Detect and handle stuck buffer condition
    if (buffer_pos > 0 && buffer_pos <= 2 && buffer_pos == last_buffer_size && 
        (current_time - last_buffer_change_time > 2000)) {  // 2 seconds with no change
        ESP_LOGI(TAG, "Clearing stuck buffer with %d bytes", buffer_pos);
        buffer_pos = 0;  // Reset the buffer
        state = WAIT_HEADER;  // Reset state machine to look for new header
    }
    
    // Update buffer tracking
    if (buffer_pos != last_buffer_size) {
        last_buffer_size = buffer_pos;
        last_buffer_change_time = current_time;
    }
    
    if (template_processed && (current_time - last_template_time > 5000)) {
        template_processed = false;
        final_packet_sent = false;
        ESP_LOGI(TAG, "Template tracking reset after timeout");
    }
    
    int bytes_read = uart_read_bytes(UART_NUM, 
                                   buffer + buffer_pos, 
                                   sizeof(buffer) - buffer_pos, 
                                   timeout / portTICK_PERIOD_MS);
    
    if (bytes_read <= 0 && buffer_pos == 0) return NULL;
    buffer_pos += (bytes_read > 0) ? bytes_read : 0;
    
    // Debug: Print received bytes
    if (bytes_read > 0) {
        ESP_LOGI(TAG, "Read %d bytes, buffer now contains %d bytes", bytes_read, buffer_pos);
        // ESP_LOG_BUFFER_HEX(TAG, buffer, min(buffer_pos, 256));  // Only show first 256 bytes to avoid log spam
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
            // Only check the last 20 bytes to avoid false positives earlier in the data
            size_t search_start = buffer_pos > 20 ? buffer_pos - 20 : 0;
            for (size_t i = search_start; i < buffer_pos - 4; i++) {
                if (buffer[i] == 'F' && buffer[i+1] == 'O' && buffer[i+2] == 'O' && buffer[i+3] == 'F') {
                    found_end_marker = true;
                    ESP_LOGI(TAG, "Found FOOF end marker at position %d", i);
                    break;
                }
            }
            
            // Create a bulk response if we have sufficient data (found end marker or have enough bytes)
            if (found_end_marker || buffer_pos > 400) {
                template_processed = true;  // Mark as processed
                last_template_time = current_time;  // Update timestamp
                
                // Create response with the data
                MultiPacketResponse *response = heap_caps_malloc(sizeof(MultiPacketResponse), MALLOC_CAP_8BIT);
                if (!response) return NULL;
                
                response->packets = heap_caps_malloc(sizeof(FingerprintPacket*) * 2, MALLOC_CAP_8BIT);
                if (!response->packets) {
                    heap_caps_free(response);
                    return NULL;
                }
                response->count = 0;
                
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
                    ESP_LOGI(TAG, "Created bulk template data packet with %d bytes", data_size);
                    
                    // Add the final 0x08 packet ONLY if we haven't already sent one
                    // and we didn't find a natural one in the data
                    if (!final_packet_sent && !found_natural_final) {
                        FingerprintPacket *final_packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                        if (final_packet) {
                            memset(final_packet, 0, sizeof(FingerprintPacket));
                            final_packet->header = 0xEF01;
                            final_packet->address = 0xFFFFFFFF;
                            final_packet->packet_id = 0x08;  // Final template packet
                            final_packet->length = 8;  // Standard length for final packet
                            
                            response->packets[response->count++] = final_packet;
                            ESP_LOGI(TAG, "Added final packet marker (0x08) to response");
                            final_packet_sent = true;  // Mark that we've sent a final packet
                        }
                    } else {
                        ESP_LOGI(TAG, "Skipped adding final packet - already sent or found in data");
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
    
    // Process buffer
    size_t i = 0;
    while (i < buffer_pos) {
        switch (state) {
            case WAIT_HEADER:
                // Look for EF 01 header
                if (buffer_pos - i >= 2) {
                    if (buffer[i] == 0xEF && buffer[i+1] == 0x01) {
                        ESP_LOGI(TAG, "Found header (0xEF01) at position %d", i);
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
                    ESP_LOGI(TAG, "Read address: 0x%08" PRIX32, current_packet.address);
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
                    ESP_LOGI(TAG, "Read packet_id: 0x%02X", current_packet.packet_id);
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
                    ESP_LOGI(TAG, "Read length: %d", content_length);
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
                            } else {
                                ESP_LOGI(TAG, "Received template data packet: ID=0x%02X, Length=%d", 
                                        current_packet.packet_id, data_length);
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
                            ESP_LOGI(TAG, "Processing partial template data (%d bytes available)", available_data);
                            
                            // Copy the available data
                            if (available_data <= sizeof(current_packet.parameters)) {
                                memcpy(current_packet.parameters, &buffer[i], available_data);
                                current_packet.length = available_data + 2; // +2 for checksum
                                
                                // Add this packet to the response
                                // We don't add synthetic final packet here - that's handled in the bulk processing
                                FingerprintPacket *new_packet = heap_caps_malloc(sizeof(FingerprintPacket), MALLOC_CAP_8BIT);
                                if (new_packet) {
                                    memcpy(new_packet, &current_packet, sizeof(FingerprintPacket));
                                    response->packets[response->count++] = new_packet;
                                    ESP_LOGI(TAG, "Added partial template data packet to response");
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
                    ESP_LOGI(TAG, "Read checksum: 0x%04X", current_packet.checksum);
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
                                ESP_LOGI(TAG, "Expanded packet array to %d slots", new_size);
                            } else {
                                ESP_LOGW(TAG, "Failed to resize packet array, discarding packet");
                                heap_caps_free(new_packet);
                                goto buffer_shift;
                            }
                        }
                        
                        response->packets[response->count++] = new_packet;
                        ESP_LOGI(TAG, "Added packet to response, count now: %d", response->count);
                    }
                    
                    state = WAIT_HEADER;  // Look for next packet header
                } else {
                    goto buffer_shift;
                }
                break;
        }
    }
    
    // Emergency timeout - if we've been processing too long without progress
    if ((current_time - last_buffer_change_time) > 5000) {
        ESP_LOGW(TAG, "Emergency timeout - processing took too long, clearing buffer");
        i = buffer_pos;  // This will cause the buffer to be cleared in buffer_shift
        state = WAIT_HEADER;
    }
    
buffer_shift:
    // Move any remaining bytes to the start of buffer
    if (i < buffer_pos) {
        memmove(buffer, buffer + i, buffer_pos - i);
        buffer_pos = buffer_pos - i;
        ESP_LOGI(TAG, "Shifted buffer, %d bytes remaining", buffer_pos);
    } else {
        buffer_pos = 0;
        ESP_LOGI(TAG, "Buffer fully processed, reset position");
    }
    
    if (response->count == 0) {
        ESP_LOGW(TAG, "No complete packets found, returning NULL");
        heap_caps_free(response->packets);
        heap_caps_free(response);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Returning response with %d packets", response->count);
    for (size_t i = 0; i < response->count; i++) {
        if (response->packets[i]) {
            ESP_LOGI(TAG, "Packet %d: ID=0x%02X, Length=%d", i, response->packets[i]->packet_id, response->packets[i]->length);
            ESP_LOG_BUFFER_HEX(TAG, response->packets[i], sizeof(FingerprintPacket));
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

void read_response_task(void *pvParameter) {
    while (1) {
        MultiPacketResponse *response = fingerprint_read_response();
        if (response) {
            // Process each packet
            for (size_t i = 0; i < response->count; i++) {
                fingerprint_response_t event = {0};
                memcpy(&event.packet, response->packets[i], sizeof(FingerprintPacket));
                
                // Try to send to queue with increased timeout
                if (xQueueSend(fingerprint_response_queue, &event, pdMS_TO_TICKS(500)) != pdPASS) {
                    ESP_LOGW(TAG, "Response queue full, clearing old messages");
                    xQueueReset(fingerprint_response_queue);
                    if (xQueueSend(fingerprint_response_queue, &event, pdMS_TO_TICKS(100)) != pdPASS) {
                        ESP_LOGE(TAG, "Still unable to send to queue after reset");
                    }
                }
                
                // Free the packet
                heap_caps_free(response->packets[i]);
            }

            // Free the response structure
            heap_caps_free(response->packets);
            heap_caps_free(response);
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
    
    // Increase buffer size to 4096 bytes (4KB) - sufficient for all templates
    static uint8_t template_buffer[4096] = {0};
    static size_t template_size = 0;
    static uint32_t template_start_time = 0;
    static bool template_data_complete = false;

    while (1) {
        if (xQueueReceive(fingerprint_response_queue, &response, portMAX_DELAY) == pdTRUE) {
            BaseType_t cmd_received = xQueueReceive(fingerprint_command_queue, 
                                                  &last_cmd, 
                                                  pdMS_TO_TICKS(100));

            uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

            // Always set success bit for any UpChar response packet
            if (cmd_received && last_cmd.command == FINGERPRINT_CMD_UP_CHAR) {
                if (enroll_event_group != NULL) {
                    ESP_LOGI(TAG, "Setting success bit for UpChar command acknowledgment");
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
                
                // Use explicit safe buffer clearing
                memset(template_buffer, 0, sizeof(template_buffer));
                ESP_LOGI(TAG, "Starting template upload, buffer cleared (size: %d bytes)", sizeof(template_buffer));
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
                            
                            ESP_LOGI(TAG, "Added %d bytes to template buffer (total: %d bytes)", 
                                     data_length, template_size);
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
                                
                                // Signal completion - IMMEDIATELY after finding FOOF
                                if (enroll_event_group != NULL) {
                                    xEventGroupSetBits(enroll_event_group, TEMPLATE_UPLOAD_COMPLETE_BIT);
                                    ESP_LOGI(TAG, "Template upload complete (FOOF marker), signaled event group");
                                    waiting_for_template = false;  // Stop waiting for more data
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
                        ESP_LOGI(TAG, "Trimmed %d trailing zeros from template", template_size - actual_size);
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
                        ESP_LOGI(TAG, "Template upload complete (final packet), signaled event group");
                    }
                    
                    // Trigger the event
                    trigger_fingerprint_event(template_event);
                    
                    // Reset for next time
                    waiting_for_template = false;
                    template_size = 0;
                    template_data_complete = false;
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
    // event.packet = *packet;  // Store full response packet

    if (packet != NULL) {
        event.packet = *packet;  // Store full response packet
    } else {
        memset(&event.packet, 0, sizeof(FingerprintPacket));  // Clear packet structure to avoid garbage values
    }
    // uint16_t packet_length = sizeof(packet);
    // ESP_LOGI(TAG, "Outside fingerprint status handler: 0x%02X", status);
    // ESP_LOG_BUFFER_HEX(TAG, packet, packet_length);
    switch (status) {
        case FINGERPRINT_OK:
            // ESP_LOGI(TAG, "Fingerprint operation successful: 0x%02X", status);
            if (last_sent_command == FINGERPRINT_CMD_SEARCH) {
                event_type = EVENT_SEARCH_SUCCESS;
                event.packet = *packet;
                // Parse parameters into structured format
                event.data.match_info.page_id = (packet->parameters[1] << 8) | packet->parameters[0];
                event.data.match_info.template_id = convert_page_id_to_index(event.data.match_info.page_id);
                event.data.match_info.match_score = (packet->parameters[3] << 8) | packet->parameters[2];
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);  // Changed from FAIL to SUCCESS
                }
            } else if (last_sent_command == FINGERPRINT_CMD_GET_IMAGE && enroll_event_group!=NULL) {
                event_type = EVENT_FINGER_DETECTED;
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
            } else if (last_sent_command == FINGERPRINT_CMD_GET_IMAGE) {
                // event_type = EVENT_IMAGE_VALID;
            } else if (last_sent_command == FINGERPRINT_CMD_VALID_TEMPLATE_NUM) {
                event_type = EVENT_TEMPLATE_COUNT;
                event.data.template_count.count = (packet->parameters[0] << 8) | packet->parameters[1];
                // ESP_LOGI(TAG, "Number of valid templates: %d", event.data.template_count.count);
            } else if (last_sent_command == FINGERPRINT_CMD_READ_INDEX_TABLE) {
                event_type = EVENT_INDEX_TABLE_READ;
                // if (enroll_event_group) {
                //     EventBits_t flags = xEventGroupGetBits(enroll_event_group);
                //     if (flags & CHECKING_LOCATION_BIT) {  // Check if location check is active
                //         xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                //     }
                // }

                if (packet != NULL && enroll_event_group) {
                    uint8_t template_exists = 0;
                    
                    // // Debug print the index table data
                    // ESP_LOGI(TAG, "Index Table Response received:");
                    // ESP_LOG_BUFFER_HEX(TAG, packet->parameters, packet->length); // Print all 32 bytes of index data
                    
                    // The index table response format has 32 bytes of index data
                    // Each bit represents one template location
                    uint8_t position = global_location & 0xFF;
                    uint8_t byte_offset = position / 8;    // Which byte contains our bit
                    uint8_t bit_position = position % 8;   // Which bit in that byte
                    
                    // ESP_LOGI(TAG, "Checking template at position %d (byte %d, bit %d)", 
                    //          position, byte_offset, bit_position);
                    
                    // Check if the bit is set in the index table
                    if (byte_offset < 32) { // We have 32 bytes of index data
                        if (packet->parameters[byte_offset] & (1 << bit_position)) {
                            template_exists = 1;
                            // ESP_LOGW(TAG, "Template exists at position %d", position);
                        } else {
                            // ESP_LOGI(TAG, "Position %d is free", position);
                        }
                    }
                    
                    if (template_exists) {
                        xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
                    } else {
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
                // ESP_LOGI(TAG, "Template loaded successfully");
                event_type = EVENT_TEMPLATE_LOADED;
            } if (last_sent_command == FINGERPRINT_CMD_UP_CHAR) {
                event_type = EVENT_TEMPLATE_UPLOADED;
                if (packet->packet_id == 0x02) {  // Data packet
                    ESP_LOGI(TAG, "Received data packet");
                    // Process the template data here
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
                xEventGroupSetBits(enroll_event_group, ENROLL_BIT_SUCCESS);
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
                // No match found during search (good for enrollment)
                event_type = EVENT_MATCH_FAIL;
                if (enroll_event_group) {
                    xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
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
        case FINGERPRINT_DATA_PACKET_ERROR:
        case FINGERPRINT_FLASH_RW_ERROR:
        case FINGERPRINT_PORT_OP_FAIL:
        case FINGERPRINT_DB_CLEAR_FAIL:
        case FINGERPRINT_DB_RANGE_ERROR:
        case FINGERPRINT_READ_TEMPLATE_ERROR:
        case FINGERPRINT_UPLOAD_FEATURE_FAIL:
            event_type = EVENT_ERROR;
            break;
        case FINGERPRINT_DELETE_TEMPLATE_FAIL:
        case FINGERPRINT_DB_EMPTY:
        case FINGERPRINT_ENTRY_COUNT_ERROR:
        case FINGERPRINT_ALREADY_EXISTS:
        // if (last_sent_command == FINGERPRINT_CMD_STORE_CHAR || 
        //     last_sent_command == FINGERPRINT_CMD_REG_MODEL ||
        //     (last_sent_command == FINGERPRINT_CMD_GEN_CHAR && packet->parameters[0] == 0x01) ||  // Gen Char 1
        //     (last_sent_command == FINGERPRINT_CMD_GEN_CHAR && packet->parameters[0] == 0x02))   // Gen Char 2
        //     {
        //         event_type = EVENT_ENROLL_FAIL;  // ❌ Enrollment failed
        //         if (enroll_event_group) {   
        //             xEventGroupSetBits(enroll_event_group, ENROLL_BIT_FAIL);
        //         }
        //     } else {
        //         event_type = EVENT_MATCH_FAIL;
        //     }
            ESP_LOGI(TAG, "Template exists at specified location");
            if(last_sent_command == FINGERPRINT_CMD_STORE_CHAR){
                event_type = EVENT_TEMPLATE_EXISTS;
            } else {
                event_type = EVENT_MATCH_FAIL;
            }
            // event_type = EVENT_TEMPLATE_EXISTS;
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
            // ESP_LOGE("Fingerprint", "Unknown status: 0x%02X", status);
            // event_type = EVENT_ERROR;
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
        // ESP_LOGI("Fingerprint", "Triggering event: %d for status: 0x%02X", event_type, status);
        trigger_fingerprint_event(event);
    }
    // ESP_LOGI("Fingerprint", "Triggering event: %d for status: 0x%02X", event_type, status);
    // trigger_fingerprint_event(event_type, status);

    // Log event group bits for debugging
    if (enroll_event_group) {
        EventBits_t bits = xEventGroupGetBits(enroll_event_group);
        // ESP_LOGI(TAG, "Current event bits: 0x%02X", (unsigned int)bits);
    }
}

// Function to register the event handler
void register_fingerprint_event_handler(fingerprint_event_handler_t handler) {
    g_fingerprint_event_handler = handler;
}

// Function to trigger the event (you can call this inside your fingerprint processing flow)
void trigger_fingerprint_event(fingerprint_event_t event) {
    if (g_fingerprint_event_handler != NULL) {
        // fingerprint_event_t event = {event_type, status};
        g_fingerprint_event_handler(event);  /**< Call the registered event handler. */
    } else {
        ESP_LOGE("Fingerprint", "No event handler registered.");
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

    // Set checking location flag
    xEventGroupSetBits(enroll_event_group, CHECKING_LOCATION_BIT);
    
    // First, validate if the location is available
    uint8_t page = location >> 8;  // Get the page number
    uint8_t position = location & 0xFF;  // Get position within page
    
    uint8_t index_params[] = {page};  // Use the calculated page number
    // uint16_t page_id = convert_index_to_page_id(location);  // Convert location to proper page ID
    // uint8_t index_params[] = {
    //     1,  // Buffer ID
    //     (uint8_t)(page_id >> 8),     // High byte of page ID
    //     (uint8_t)(page_id & 0xFF)    // Low byte of page ID
    // };
    fingerprint_set_command(&PS_ReadIndexTable, FINGERPRINT_CMD_READ_INDEX_TABLE, 
                          index_params, sizeof(index_params));
    
    err = fingerprint_send_command(&PS_ReadIndexTable, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read index table");
        goto cleanup;
    }

    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

    // Clear checking location flag
    xEventGroupClearBits(enroll_event_group, CHECKING_LOCATION_BIT);

    if (bits & ENROLL_BIT_FAIL) {
        ESP_LOGE(TAG, "Location %d is already occupied", location);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Location %d is available", location);

    while (attempts < 3) {
        ESP_LOGI(TAG, "Waiting for a finger to be placed...");
        
        // Clear states
        uart_flush(UART_NUM);
        xQueueReset(fingerprint_command_queue);
        xQueueReset(fingerprint_response_queue);
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);

        // Wait for finger placement
        bool finger_detected = false;
        while (!finger_detected) {
            err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
            if (err != ESP_OK) return err;

            bits = xEventGroupWaitBits(enroll_event_group,
                                     ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                     pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
            
            if (bits & ENROLL_BIT_SUCCESS) {
                finger_detected = true;
                // ESP_LOGI(TAG, "Finger detected!");
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        // Generate first template
        err = fingerprint_send_command(&PS_GenChar1, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) return err;

        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                                 
        if (!(bits & ENROLL_BIT_SUCCESS)) {
            attempts++;
            continue;
        }

        ESP_LOGI(TAG, "Remove finger and place it again...");
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Wait for finger removal
        finger_removed = false;
        uint8_t no_finger_count = 0;
        while (!finger_removed && no_finger_count < 20) { 
            xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
            err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
            if (err != ESP_OK) return err;
            
            bits = xEventGroupWaitBits(enroll_event_group,
                                    ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
            
            // Check specifically for ENROLL_BIT_FAIL which is set when NO_FINGER is detected
            if (bits & ENROLL_BIT_FAIL) {  
                no_finger_count++;
                if (no_finger_count >= 1) {
                    finger_removed = true;
                    ESP_LOGI(TAG, "Finger removal confirmed");
                }
            } else {
                no_finger_count = 0;
                ESP_LOGI(TAG, "Please remove your finger...");
            }
            // vTaskDelay(pdMS_TO_TICKS(200)); // Increased delay between checks
        }

        if (!finger_removed) {
            ESP_LOGW(TAG, "Finger not removed within timeout period");
            attempts++;
            continue;
        }

        // Wait for second finger placement
        finger_detected = false;
        while (!finger_detected) {
            xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
            err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
            if (err != ESP_OK) return err;

            bits = xEventGroupWaitBits(enroll_event_group,
                                     ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                     pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
            
            if (bits & ENROLL_BIT_SUCCESS) {
                finger_detected = true;
                ESP_LOGI(TAG, "Second finger placement detected!");
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        // Generate Char 2
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
        err = fingerprint_send_command(&PS_GenChar2, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) return err;

        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                                 
        if (!(bits & ENROLL_BIT_SUCCESS)) {
            attempts++;
            continue;
        }

        // Create template model
        err = fingerprint_send_command(&PS_RegModel, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            attempts++;
            continue;
        }

        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                                 
        if (!(bits & ENROLL_BIT_SUCCESS)) {
            attempts++;
            continue;
        }

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

        if (bits & ENROLL_BIT_SUCCESS) {  // Match found = duplicate exists
            ESP_LOGE(TAG, "Fingerprint already exists in database!");
            attempts++;
            continue;
        } else if (bits & ENROLL_BIT_FAIL) {  // No match found = good to proceed
            ESP_LOGI(TAG, "No duplicate found, continuing enrollment...");
        }

        // Store template at specified location
        uint8_t store_params[] = {1, (uint8_t)(location >> 8), (uint8_t)(location & 0xFF)};
        // uint16_t page_id = convert_index_to_page_id(location);  // Convert location to proper page ID
        // uint8_t store_params[] = {
        //     1,  // Buffer ID
        //     (uint8_t)(page_id >> 8),     // High byte of page ID
        //     (uint8_t)(page_id & 0xFF)    // Low byte of page ID
        // };
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
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
            return ESP_OK;
        }
        
        attempts++;
    }

    ESP_LOGE(TAG, "Enrollment failed after %d attempts", attempts);
    cleanup:
        if (enroll_event_group) {
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
        }
    // return err;
    return ESP_FAIL;
}

esp_err_t verify_fingerprint(void) {
    esp_err_t err;
    EventBits_t bits;
    uint8_t attempts = 0;
    const uint8_t max_attempts = 3;

    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    while (attempts < max_attempts) {
        ESP_LOGI(TAG, "Please place your finger on the sensor...");
        
        // Clear states more thoroughly
        uart_flush(UART_NUM);
        xQueueReset(fingerprint_command_queue);
        xQueueReset(fingerprint_response_queue);
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
        vTaskDelay(pdMS_TO_TICKS(100)); // Short delay after clearing

        // Wait for finger
        bool finger_detected = false;
        const int MAX_NO_FINGER_TIME = 5000; // 5 seconds timeout
        TickType_t start_time = xTaskGetTickCount();
        
        while (!finger_detected && (xTaskGetTickCount() - start_time < pdMS_TO_TICKS(MAX_NO_FINGER_TIME))) {
            err = fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
            if (err != ESP_OK) return err;

            bits = xEventGroupWaitBits(enroll_event_group,
                                     ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                     pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
            
            if (bits & ENROLL_BIT_SUCCESS) {
                finger_detected = true;
                ESP_LOGI(TAG, "Finger detected!");
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        if (!finger_detected) {
            ESP_LOGW(TAG, "No finger detected within 5 seconds");
            attempts++;
            continue;  // Try next attempt
        }

        // Generate character file
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
        err = fingerprint_send_command(&PS_GenChar1, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) {
            attempts++;
            continue;
        }

        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        
        if (!(bits & ENROLL_BIT_SUCCESS)) {
            attempts++;
            continue;
        }

        // Search for match with modified parameters
        uint8_t search_params[] = {0x01,    // BufferID = 1
                                 0x00, 0x00, // Start page = 0
                                 0x00, 0x64}; // Number of pages = 100 (increased range)
        
        xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
        fingerprint_set_command(&PS_Search, FINGERPRINT_CMD_SEARCH, search_params, sizeof(search_params));
        err = fingerprint_send_command(&PS_Search, DEFAULT_FINGERPRINT_ADDRESS);
        
        if (err != ESP_OK) {
            attempts++;
            continue;
        }

        ESP_LOGI(TAG, "Searching for fingerprint match...");
        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
        
        if (bits & ENROLL_BIT_SUCCESS) {
            // ESP_LOGI(TAG, "Fingerprint match found!");
            vEventGroupDelete(enroll_event_group);
            enroll_event_group = NULL;
            return ESP_OK;
        }
        
        ESP_LOGW(TAG, "No match found, attempt %d of %d", attempts + 1, max_attempts);
        attempts++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "Verification failed after %d attempts", attempts);
    vEventGroupDelete(enroll_event_group);
    enroll_event_group = NULL;
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

// Helper function to load template from flash to buffer
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

esp_err_t upload_template(uint8_t buffer_id, uint8_t* template_data, size_t* template_size) {
    esp_err_t err;
    
    // Skip event group mechanism for template upload - it's unreliable
    
    // 1. Clear UART buffer and response queues
    uart_flush(UART_NUM);
    xQueueReset(fingerprint_command_queue);
    xQueueReset(fingerprint_response_queue);
    
    // 2. Send upload command with longer timeout
    ESP_LOGI(TAG, "Sending UpChar command for buffer %d", buffer_id);
    uint8_t params[] = {buffer_id};
    fingerprint_set_command(&PS_UpChar, FINGERPRINT_CMD_UP_CHAR, params, sizeof(params));
    err = fingerprint_send_command(&PS_UpChar, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send UpChar command");
        return err;
    }
    
    // 3. Simply wait a fixed time for template to transfer rather than using events
    ESP_LOGI(TAG, "Waiting for template data transfer...");
    vTaskDelay(pdMS_TO_TICKS(4000));  // 4 seconds should be plenty for template transfer
    
    // 4. Template should now be processed by fingerprint_read_response
    ESP_LOGI(TAG, "Template upload should be complete");
    return ESP_OK;
}

// Download template from host to module buffer
esp_err_t download_template(uint8_t buffer_id, const uint8_t* template_data, size_t template_size) {
    esp_err_t err;
    EventBits_t bits;
    
    // 1. Send initial download command
    uint8_t params[] = {buffer_id};
    fingerprint_set_command(&PS_DownChar, FINGERPRINT_CMD_DOWN_CHAR, params, sizeof(params));
    err = fingerprint_send_command(&PS_DownChar, DEFAULT_FINGERPRINT_ADDRESS);
    if (err != ESP_OK) return err;

    // 2. Wait for confirmation to start sending data (0x00 means ready to receive)
    bits = xEventGroupWaitBits(enroll_event_group,
                             ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                             pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
                             
    if (!(bits & ENROLL_BIT_SUCCESS)) {
        ESP_LOGE(TAG, "Module not ready to receive template data");
        return ESP_FAIL;
    }

    // 3. Send template data packets
    const size_t MAX_PACKET_SIZE = 128;  // Typical packet size for template data
    size_t remaining = template_size;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk_size = (remaining > MAX_PACKET_SIZE) ? MAX_PACKET_SIZE : remaining;
        bool is_last_packet = (remaining <= MAX_PACKET_SIZE);

        // Create data packet structure
        FingerprintPacket data_packet = {
            .header = FINGERPRINT_PACKET_HEADER,
            .address = DEFAULT_FINGERPRINT_ADDRESS,
            .packet_id = is_last_packet ? 0x08 : 0x02,  // 0x08 for last packet, 0x02 for others
            .length = chunk_size + 2,  // Add 2 for checksum
            .code.command = 0x00  // Data packets don't use command byte
        };

        // Copy chunk of template data
        memcpy(data_packet.parameters, template_data + offset, chunk_size);
        data_packet.checksum = fingerprint_calculate_checksum(&data_packet);

        // Send packet
        err = fingerprint_send_command(&data_packet, DEFAULT_FINGERPRINT_ADDRESS);
        if (err != ESP_OK) return err;

        // Wait for acknowledgment after each packet
        bits = xEventGroupWaitBits(enroll_event_group,
                                 ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL,
                                 pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        
        if (!(bits & ENROLL_BIT_SUCCESS)) {
            ESP_LOGE(TAG, "Failed to send template data packet");
            return ESP_FAIL;
        }

        offset += chunk_size;
        remaining -= chunk_size;
    }

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
    uint8_t template_data[512];  // Adjust size as needed
    size_t template_size = 0;

    // Create event group at the start of backup process
    err = initialize_event_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize event group");
        return err;
    }
    
    ESP_LOGI(TAG, "Loading Template...");
    // Convert template_id to page_id for LoadChar command
    // uint16_t page_id = convert_index_to_page_id(template_id);
    uint16_t page_id = template_id;
    
    // 1. Load template from flash to buffer 1 with correct page_id
    err = load_template_to_buffer(template_id, 1);
    if (err != ESP_OK) {
        cleanup_event_group();
        return err;
    }
    
    ESP_LOGI(TAG, "Loading Template Successful");
    // 2. Upload template from buffer to host
    ESP_LOGI(TAG, "Uploading Template...");
    err = upload_template(1, template_data, &template_size);
    
    cleanup_event_group();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Template backup successful");
    return ESP_OK;
}

// Example usage for restoring a template:
esp_err_t restore_template(uint16_t template_id, const uint8_t* template_data, size_t template_size) {
    // 1. Download template to buffer 1
    esp_err_t err = download_template(1, template_data, template_size);
    if (err != ESP_OK) return err;
    
    // 2. Store template from buffer to flash
    err = store_template(1, template_id);
    if (err != ESP_OK) return err;
    
    return ESP_OK;
}

esp_err_t initialize_event_group(void) {
    if (enroll_event_group == NULL) {
        enroll_event_group = xEventGroupCreate();
        if (enroll_event_group == NULL) {
            // ESP_LOGE(TAG, "Failed to create event group");
            return ESP_ERR_NO_MEM;
        }
    }

    // Clear any existing bits to ensure a clean state
    xEventGroupClearBits(enroll_event_group, ENROLL_BIT_SUCCESS | ENROLL_BIT_FAIL);
    return ESP_OK;
}

esp_err_t cleanup_event_group(void) {
    if (enroll_event_group == NULL) {
        // ESP_LOGW(TAG, "Event group is already NULL.");
        return ESP_ERR_INVALID_STATE;  // Indicates it was already deleted
    }

    vEventGroupDelete(enroll_event_group);
    enroll_event_group = NULL;
    // ESP_LOGI(TAG, "Enrollment event group deleted successfully.");
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

