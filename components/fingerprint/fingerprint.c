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

#define TAG "FINGERPRINT"
#define UART_NUM UART_NUM_1  // Change based on your wiring
#define RX_BUF_SIZE 128  // Adjust based on fingerprint module response

static int tx_pin = DEFAULT_TX_PIN; // Default TX pin
static int rx_pin = DEFAULT_RX_PIN; // Default RX pin
static int baud_rate = DEFAULT_BAUD_RATE; // Default baud rate

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
static QueueHandle_t fingerprint_response_queue;

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
    .command = FINGERPRINT_CMD_HANDSHAKE, // Handshake
    .parameters = {0}, // No parameters
    .checksum = 0x0039 // Needs to be recalculated
};

FingerprintPacket PS_GetImage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_GET_IMAGE, // Get Image
    .parameters = {0}, // No parameters
    .checksum = 0x0005 // Hardcoded checksum
};

FingerprintPacket PS_GenChar1 = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .command = FINGERPRINT_CMD_GEN_CHAR, // Generate Character
    .parameters = {0}, // Buffer ID 1
    .checksum = 0x0008 // Hardcoded checksum
};

FingerprintPacket PS_GenChar2 = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .command = FINGERPRINT_CMD_GEN_CHAR, // Generate Character
    .parameters = {0}, // Buffer ID 2
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintPacket PS_RegModel = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_REG_MODEL, // Register Model
    .parameters = {0}, // No parameters
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintPacket PS_Search = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0008,
    .command = FINGERPRINT_CMD_SEARCH, // Search
    .parameters = {0}, // Buffer ID, Start Page, Number of Pages
    .checksum = 0x00 // Hardcoded checksum
};

FingerprintPacket PS_Match = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_MATCH, // Match
    .parameters = {0}, // No parameters
    .checksum = 0x0007 // Hardcoded checksum
};

FingerprintPacket PS_StoreChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0006,
    .command = FINGERPRINT_CMD_STORE_CHAR, // Store Character
    .parameters = {0}, // Buffer ID, Page ID
    .checksum = 0x000F // Hardcoded checksum
};

FingerprintPacket PS_DeletChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .command = FINGERPRINT_CMD_DELETE_CHAR, // Delete Fingerprint
    .parameters = {0}, // Page ID, Number of Entries
    .checksum = 0x0015 // Hardcoded checksum
};

FingerprintPacket PS_Empty = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_EMPTY_DATABASE, // Clear Database
    .parameters = {0}, // No parameters
    .checksum = 0x0011 // Hardcoded checksum
};

FingerprintPacket PS_ReadSysPara = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_READ_SYS_PARA, // Read System Parameters
    .parameters = {0}, // No parameters
    .checksum = 0x0013 // Hardcoded checksum
};

FingerprintPacket PS_SetChipAddr = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .command = FINGERPRINT_CMD_SET_CHIP_ADDR, // Set Address
    .parameters = {0}, // New Address (modifiable)
    .checksum = 0x0020 // Hardcoded checksum
};

FingerprintPacket PS_Cancel = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_CANCEL, // Cancel command
    .parameters = {0}, // No parameters
    .checksum = 0x0033 // Needs to be recalculated
};

FingerprintPacket PS_AutoEnroll = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0008,
    .command = FINGERPRINT_CMD_AUTO_ENROLL, // AutoEnroll command
    .parameters = {0}, // ID number, number of entries, parameter
    .checksum = 0x003A // Needs to be recalculated
};

FingerprintPacket PS_Autoldentify = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0006,
    .command = FINGERPRINT_CMD_AUTO_IDENTIFY, // AutoIdentify command
    .parameters = {0}, // Score level, ID number
    .checksum = 0x003F // Needs to be recalculated
};

FingerprintPacket PS_GetKeyt = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_GETKEYT, // Get key pair
    .parameters = {0}, // No parameters
    .checksum = 0x00E3 // Needs to be recalculated
};

FingerprintPacket PS_SecurityStoreChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0006,
    .command = 0xF2, // Secure Store Template
    .parameters = {0}, // Buffer ID, Page ID
    .checksum = 0x00FB // Needs to be recalculated
};

FingerprintPacket PS_SecuritySearch = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0008,
    .command = FINGERPRINT_CMD_SECURITY_SEARCH, // Secure Search
    .parameters = {0}, // Buffer ID, Start Page, Number of Pages
    .checksum = 0x00FD // Needs to be recalculated
};

FingerprintPacket PS_Uplmage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_UPLOAD_IMAGE, // Upload Image
    .parameters = {0}, // No parameters
    .checksum = 0x000D // Needs to be recalculated
};

FingerprintPacket PS_Downlmage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_DOWNLOAD_IMAGE, // Download Image
    .parameters = {0}, // No parameters
    .checksum = 0x000E // Needs to be recalculated
};

FingerprintPacket PS_CheckSensor = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_CHECK_SENSOR, // Check Sensor
    .parameters = {0}, // No parameters
    .checksum = 0x0039 // Needs to be recalculated
};

FingerprintPacket PS_RestSetting = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_FACTORY_RESET, // Restore Factory Settings
    .parameters = {0}, // No parameters
    .checksum = 0x003E // Needs to be recalculated
};

FingerprintPacket PS_ReadINFpage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_READ_INF_PAGE, // Read Flash Information Page
    .parameters = {0}, // No parameters
    .checksum = 0x0019 // Needs to be recalculated
};

FingerprintPacket PS_BurnCode = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .command = FINGERPRINT_CMD_BURN_CODE, // Erase Code
    .parameters = {0}, // Default upgrade mode
    .checksum = 0x001F // Needs to be recalculated
};

FingerprintPacket PS_SetPwd = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .command = FINGERPRINT_CMD_SET_PASSWORD, // Set Password
    .parameters = {0}, // Password (modifiable)
    .checksum = 0x0019 // Needs to be recalculated
};

FingerprintPacket PS_VfyPwd = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .command = FINGERPRINT_CMD_VERIFY_PASSWORD, // Verify Password
    .parameters = {0}, // Password
    .checksum = 0x001A // Needs to be recalculated
};

FingerprintPacket PS_GetRandomCode = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_GET_RANDOM_CODE, // Get Random Number
    .parameters = {0}, // No parameters
    .checksum = 0x0017 // Needs to be recalculated
};

FingerprintPacket PS_WriteNotepad = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0023, // 32 bytes data
    .command = FINGERPRINT_CMD_WRITE_NOTEPAD, // Write Notepad
    .parameters = {0}, // Data to write (to be filled)
    .checksum = 0x003B // Needs to be recalculated
};

FingerprintPacket PS_ReadNotepad = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .command = FINGERPRINT_CMD_READ_NOTEPAD, // Read Notepad
    .parameters = {0}, // Page number
    .checksum = 0x001E // Needs to be recalculated
};

FingerprintPacket PS_ControlBLN = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .command = FINGERPRINT_CMD_CONTROL_LED, // Control LED
    .parameters = {0}, // Example parameters: function, start color, end color, cycles
    .checksum = 0x0046 // Needs to be recalculated
};

FingerprintPacket PS_GetImageInfo = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_GET_IMAGE_INFO, // Get Image Information
    .parameters = {0}, // No parameters
    .checksum = 0x0041 // Needs to be recalculated
};

FingerprintPacket PS_SearchNow = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0007,
    .command = FINGERPRINT_CMD_SEARCH_NOW, // Search Now
    .parameters = {0}, // Start Page, Number of Pages
    .checksum = 0x0046 // Needs to be recalculated
};

FingerprintPacket PS_ValidTempleteNum = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_VALID_TEMPLATE_NUM, // Get number of valid templates
    .parameters = {0}, // No parameters
    .checksum = 0x0021 // Needs to be recalculated
};

FingerprintPacket PS_Sleep = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_SLEEP, // Enter sleep mode
    .parameters = {0}, // No parameters
    .checksum = 0x0037 // Needs to be recalculated
};

FingerprintPacket PS_LockKeyt = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_LOCKEYT, // Lock Key Pair
    .parameters = {0}, // No parameters
    .checksum = 0x00E4 // Needs to be recalculated
};

FingerprintPacket PS_GetCiphertext = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_GET_CIPHER_TEXT, // Get Ciphertext Random Number
    .parameters = {0}, // No parameters
    .checksum = 0x00E5 // Needs to be recalculated
};

FingerprintPacket PS_GetChipSN = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_GETCHIP_SN, // Get Chip Serial Number
    .parameters = {0}, // No parameters
    .checksum = 0x0016 // Needs to be recalculated
};

FingerprintPacket PS_GetEnrollImage = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0003,
    .command = FINGERPRINT_CMD_GET_ENROLL_IMAGE, // Register Get Image
    .parameters = {0}, // No parameters
    .checksum = 0x002D // Needs to be recalculated
};

FingerprintPacket PS_WriteReg = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0005,
    .command = FINGERPRINT_CMD_WRITE_REG, // Write System Register
    .parameters = {0}, // Register Number, Value (modifiable)
    .checksum = 0x0013 // Needs to be recalculated
};

FingerprintPacket PS_ReadIndexTable = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .command = FINGERPRINT_CMD_READ_INDEX_TABLE, // Read Index Table
    .parameters = {0}, // Page Number
    .checksum = 0x0023 // Needs to be recalculated
};

FingerprintPacket PS_UpChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .command = FINGERPRINT_CMD_UP_CHAR, // Upload template from buffer
    .parameters = {0}, // BufferID
    .checksum = 0x000D // Needs to be recalculated
};

FingerprintPacket PS_DownChar = {
    .header = FINGERPRINT_PACKET_HEADER,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = FINGERPRINT_PACKET_ID_CMD,
    .length = 0x0004,
    .command = FINGERPRINT_CMD_DOWN_CHAR, // Download template to buffer
    .parameters = {0}, // BufferID
    .checksum = 0x000E // Needs to be recalculated
};

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

    // Configure UART
    err = uart_param_config(UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART");
        return err;
    }

    // Set UART pins
    err = uart_set_pin(UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
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

    // Handshake detection (0x55)
    uint8_t handshake;
    int length = uart_read_bytes(UART_NUM, &handshake, 1, pdMS_TO_TICKS(200));  // Timeout 200ms

    if (length > 0 && handshake == 0x55) {
        ESP_LOGI(TAG, "Fingerprint module handshake received.");
    } else {
        ESP_LOGW(TAG, "No handshake received, waiting 200ms before communication.");
        vTaskDelay(pdMS_TO_TICKS(200));  // Delay to ensure module is ready
    }

    // Create Queue for Response Handling
    fingerprint_response_queue = xQueueCreate(RESPONSE_QUEUE_SIZE, sizeof(fingerprint_response_t));
    if (fingerprint_response_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create response queue");
        return ESP_FAIL;
    }

    // Create Response Handling Task
    if (xTaskCreate(read_response_task, "FingerprintReadResponse", 4096, NULL, 5, &fingerprint_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fingerprint response task");
        return ESP_FAIL;
    }

    // Create Processing Task (processes responses from queue)
    if (xTaskCreate(process_fingerprint_responses_task, "FingerprintProcessResponse", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fingerprint processing task");
        return ESP_FAIL;
    }

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
    cmd->command = command;

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
    // Include address bytes in checksum calculation
    sum += (cmd->address >> 24) & 0xFF;
    sum += (cmd->address >> 16) & 0xFF;
    sum += (cmd->address >> 8) & 0xFF;
    sum += cmd->address & 0xFF;

    sum += cmd->packet_id;
    sum += (cmd->length >> 8) & 0xFF; // High byte of length
    sum += cmd->length & 0xFF;        // Low byte of length
    sum += cmd->command;
    // Dynamically sum all parameters based on packet length
    for (int i = 0; i < cmd->length - 3; i++) {  // Exclude command + checksum
        sum += cmd->parameters[i];
    }

    return sum;
}


esp_err_t fingerprint_send_command(FingerprintPacket *cmd, uint32_t address) {
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;  // Check for NULL pointer
    }

    // Set the correct address before computing checksum
    cmd->address = address;

    // Compute the checksum AFTER setting address
    cmd->checksum = fingerprint_calculate_checksum(cmd);
    
    // Correct packet size calculation
    size_t packet_size = cmd->length + 9; // Header (2) + Address (4) + Packet ID (1) + Length (2) + Data (cmd->length)

    // Allocate buffer dynamically
    uint8_t *buffer = (uint8_t *)malloc(packet_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Memory allocation failed for fingerprint command.");
        return ESP_ERR_NO_MEM;
    }

    // Construct the packet
    buffer[0] = (cmd->header >> 8) & 0xFF;
    buffer[1] = cmd->header & 0xFF;
    buffer[2] = (cmd->address >> 24) & 0xFF;
    buffer[3] = (cmd->address >> 16) & 0xFF;
    buffer[4] = (cmd->address >> 8) & 0xFF;
    buffer[5] = cmd->address & 0xFF;
    buffer[6] = cmd->packet_id;
    buffer[7] = (cmd->length >> 8) & 0xFF;
    buffer[8] = cmd->length & 0xFF;
    buffer[9] = cmd->command;

    // Copy only the required parameters (not limited to 4 bytes)
    memcpy(&buffer[10], cmd->parameters, cmd->length - 3);

    // Append checksum
    buffer[packet_size - 2] = (cmd->checksum >> 8) & 0xFF;
    buffer[packet_size - 1] = cmd->checksum & 0xFF;

    // Send the packet over UART
    int bytes_written = uart_write_bytes(UART_NUM, (const char *)buffer, packet_size);
    if (bytes_written != packet_size) {
        ESP_LOGE(TAG, "Failed to send the complete fingerprint command.");
        free(buffer);
        return ESP_FAIL;
    }

    // Debug logging
    ESP_LOGI(TAG, "Sent fingerprint command: 0x%02X to address 0x%08X", cmd->command, (unsigned int)address);

    // Free the allocated buffer
    free(buffer);
    
    return ESP_OK;
}


// Function to read the response packet from UART and return the FingerprintPacket structure
FingerprintPacket* fingerprint_read_response(void) {
    uint8_t buffer[MAX_PARAMETERS + 12];  // Allocate max possible response size dynamically
    int length = uart_read_bytes(UART_NUM, buffer, sizeof(buffer), UART_READ_TIMEOUT);

    if (length <= 0) {
        ESP_LOGE("Fingerprint", "Failed to read data from UART");
        // Trigger an error event for UART read failure
        fingerprint_status_event_handler(FINGERPRINT_SENSOR_OP_FAIL);  // Error in sensor operation
        return NULL;
    }

    // Check packet length to prevent out-of-bounds errors
    if (length < 12) {  // Minimum valid packet size
        ESP_LOGE("Fingerprint", "Invalid packet length: %d", length);
        // Trigger an event for invalid packet length
        fingerprint_status_event_handler(FINGERPRINT_PACKET_ERROR);  // Error in receiving packet
        return NULL;
    }

    // Compute checksum before allocating memory
    uint16_t received_checksum = (buffer[length - 2] << 8) | buffer[length - 1];
    uint16_t computed_checksum = 0;

    for (int i = 6; i < length - 2; i++) {  // Skip header and address in checksum
        computed_checksum += buffer[i];
    }

    if (computed_checksum != received_checksum) {
        ESP_LOGE("Fingerprint", "Checksum mismatch! Computed: 0x%04X, Received: 0x%04X", computed_checksum, received_checksum);
        // Trigger an event for checksum mismatch
        fingerprint_status_event_handler(FINGERPRINT_DATA_PACKET_ERROR);  // Error in data packet
        return NULL;
    }

    // Allocate packet only if checksum is valid
    FingerprintPacket *packet = (FingerprintPacket*)malloc(sizeof(FingerprintPacket));
    if (!packet) {
        ESP_LOGE("Fingerprint", "Memory allocation failed!");
        // Trigger an event for memory allocation failure
        fingerprint_status_event_handler(FINGERPRINT_SENSOR_OP_FAIL);  // Sensor operation failure
        return NULL;
    }

    memset(packet, 0, sizeof(FingerprintPacket));  // Initialize allocated memory

    // Copy response data into the packet structure
    packet->header = (buffer[0] << 8) | buffer[1];
    packet->address = (buffer[2] << 24) | (buffer[3] << 16) | (buffer[4] << 8) | buffer[5];
    packet->packet_id = buffer[6];
    packet->length = (buffer[7] << 8) | buffer[8];
    packet->command = buffer[9];

    // Copy only valid parameters, prevent buffer overrun
    int param_size = min(packet->length - 3, MAX_PARAMETERS);
    memcpy(packet->parameters, &buffer[10], param_size);

    packet->checksum = received_checksum;  // Store checksum after validation

    ESP_LOGI("Fingerprint", "Response read successfully: Command 0x%02X", packet->command);
    // fingerprint_status_event_handler((fingerprint_status_t)packet->command);  // Trigger event based on status code
    return packet;  // Caller must free this memory after use
}

// Task to continuously read responses and send to queue
void read_response_task(void *pvParameter) {
    while (1) {
        FingerprintPacket *response = fingerprint_read_response();

        if (response != NULL) {
            fingerprint_response_t event;
            event.packet = *response;
            free(response);  // Free after copying

            if (!xQueueSend(fingerprint_response_queue, &event, pdMS_TO_TICKS(100))) {
                ESP_LOGW(TAG, "Response queue full, dropping packet.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // Avoid excessive CPU usage
    }
}

// Function to get the next response from the queue
bool fingerprint_get_next_response(fingerprint_response_t *response, TickType_t timeout) {
    return xQueueReceive(fingerprint_response_queue, response, timeout) == pdTRUE;
}

// Function to process fingerprint responses
void process_fingerprint_responses_task(void *pvParameter) {
    fingerprint_response_t event;

    while (1) {
        if (fingerprint_get_next_response(&event, portMAX_DELAY)) {  // Wait indefinitely for a response
            ESP_LOGI("Fingerprint", "Processing queued response: Command 0x%02X", event.packet.command);

            // Trigger the event handler here
            fingerprint_status_event_handler((fingerprint_status_t)event.packet.command);
        }
    }
}


fingerprint_status_t fingerprint_get_status(FingerprintPacket *packet) {
    if (!packet) {
        return FINGERPRINT_ILLEGAL_DATA; // Return a default error if the packet is NULL
    }

    return (fingerprint_status_t)packet->command; // The command field stores the status code
}

// Event status handler function
void fingerprint_status_event_handler(fingerprint_status_t status) {
    fingerprint_event_type_t event_type;

    switch (status) {
        case FINGERPRINT_OK:
            event_type = EVENT_FINGER_DETECTED;
            break;

        case FINGERPRINT_NO_FINGER:
            event_type = EVENT_IMAGE_CAPTURED;
            break;

        case FINGERPRINT_IMAGE_FAIL:
        case FINGERPRINT_TOO_DRY:
        case FINGERPRINT_TOO_WET:
        case FINGERPRINT_TOO_CHAOTIC:
        case FINGERPRINT_UPLOAD_IMAGE_FAIL:
        case FINGERPRINT_IMAGE_AREA_SMALL:
        case FINGERPRINT_IMAGE_NOT_AVAILABLE:
            event_type = EVENT_IMAGE_FAIL;
            break;

        case FINGERPRINT_TOO_FEW_POINTS:
            event_type = EVENT_FEATURE_EXTRACT_FAIL;
            break;

        case FINGERPRINT_MISMATCH:
        case FINGERPRINT_NOT_FOUND:
            event_type = EVENT_MATCH_FAIL;
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
        case FINGERPRINT_DELETE_TEMPLATE_FAIL:
        case FINGERPRINT_DB_EMPTY:
        case FINGERPRINT_ENTRY_COUNT_ERROR:
        case FINGERPRINT_ALREADY_EXISTS:
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
            ESP_LOGE("Fingerprint", "Unknown status: 0x%02X", status);
            event_type = EVENT_ERROR;
            break;
    }

    ESP_LOGI("Fingerprint", "Triggering event: %d for status: 0x%02X", event_type, status);
    trigger_fingerprint_event(event_type, status);
}

// Function to register the event handler
void register_fingerprint_event_handler(fingerprint_event_handler_t handler) {
    g_fingerprint_event_handler = handler;
}

// Function to trigger the event (you can call this inside your fingerprint processing flow)
void trigger_fingerprint_event(fingerprint_event_type_t event_type, fingerprint_status_t status) {
    if (g_fingerprint_event_handler != NULL) {
        fingerprint_event_t event = {event_type, status};
        g_fingerprint_event_handler(event);  /**< Call the registered event handler. */
    } else {
        ESP_LOGE("Fingerprint", "No event handler registered.");
    }
}