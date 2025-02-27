#include "fingerprint.h"  // Direct include instead of "include/fingerprint.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "FINGERPRINT"
#define UART_NUM UART_NUM_2  // Change based on your wiring
#define RX_BUF_SIZE 128  // Adjust based on fingerprint module response

static int tx_pin = DEFAULT_TX_PIN; // Default TX pin
static int rx_pin = DEFAULT_RX_PIN; // Default RX pin
static int baud_rate = DEFAULT_BAUD_RATE; // Default baud rate

// Define the global event handler function pointer
fingerprint_event_handler_t g_fingerprint_event_handler = NULL;

void fingerprint_set_pins(int tx, int rx) {
    tx_pin = tx;
    rx_pin = rx;
}
void fingerprint_set_baudrate(int baud) {
    baud_rate = baud;
}

FingerprintPacket PS_GetImage = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x01, // Get Image
    .parameters = {0}, // No parameters
    .checksum = 0x0005 // Hardcoded checksum
};

FingerprintPacket PS_GenChar1 = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x02, // Generate Character
    .parameters = {0}, // Buffer ID 1
    .checksum = 0x0008 // Hardcoded checksum
};

FingerprintPacket PS_GenChar2 = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x02, // Generate Character
    .parameters = {0}, // Buffer ID 2
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintPacket PS_RegModel = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x05, // Register Model
    .parameters = {0}, // No parameters
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintPacket PS_Search = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0008,
    .command = 0x04, // Search
    .parameters = {0}, // Buffer ID, Start Page, Number of Pages
    .checksum = 0x00 // Hardcoded checksum
};

FingerprintPacket PS_Match = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x03, // Match
    .parameters = {0}, // No parameters
    .checksum = 0x0007 // Hardcoded checksum
};

FingerprintPacket PS_StoreChar = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0006,
    .command = 0x06, // Store Character
    .parameters = {0}, // Buffer ID, Page ID
    .checksum = 0x000F // Hardcoded checksum
};

FingerprintPacket PS_DeletChar = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x0C, // Delete Fingerprint
    .parameters = {0}, // Page ID, Number of Entries
    .checksum = 0x0015 // Hardcoded checksum
};

FingerprintPacket PS_Empty = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x0D, // Clear Database
    .parameters = {0}, // No parameters
    .checksum = 0x0011 // Hardcoded checksum
};

FingerprintPacket PS_ReadSysPara = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x0F, // Read System Parameters
    .parameters = {0}, // No parameters
    .checksum = 0x0013 // Hardcoded checksum
};

FingerprintPacket PS_SetChipAddr = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x15, // Set Address
    .parameters = {0}, // New Address (modifiable)
    .checksum = 0x0020 // Hardcoded checksum
};

FingerprintPacket PS_Cancel = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x30, // Cancel command
    .parameters = {0}, // No parameters
    .checksum = 0x0033 // Needs to be recalculated
};

FingerprintPacket PS_AutoEnroll = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0008,
    .command = 0x31, // AutoEnroll command
    .parameters = {0}, // ID number, number of entries, parameter
    .checksum = 0x003A // Needs to be recalculated
};

FingerprintPacket PS_Autoldentify = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0006,
    .command = 0x32, // AutoIdentify command
    .parameters = {0}, // Score level, ID number
    .checksum = 0x003F // Needs to be recalculated
};

FingerprintPacket PS_GetKeyt = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0xE0, // Get key pair
    .parameters = {0}, // No parameters
    .checksum = 0x00E3 // Needs to be recalculated
};

FingerprintPacket PS_SecurityStoreChar = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0006,
    .command = 0xF2, // Secure Store Template
    .parameters = {0x01, 0x00, 0x01}, // Buffer ID, Page ID
    .checksum = 0x00FB // Needs to be recalculated
};

FingerprintPacket PS_SecuritySearch = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0008,
    .command = 0xF4, // Secure Search
    .parameters = {0}, // Buffer ID, Start Page, Number of Pages
    .checksum = 0x00FD // Needs to be recalculated
};

FingerprintPacket PS_Uplmage = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x0A, // Upload Image
    .parameters = {0}, // No parameters
    .checksum = 0x000D // Needs to be recalculated
};

FingerprintPacket PS_Downlmage = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x0B, // Download Image
    .parameters = {0}, // No parameters
    .checksum = 0x000E // Needs to be recalculated
};

FingerprintPacket PS_CheckSensor = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x36, // Check Sensor
    .parameters = {0}, // No parameters
    .checksum = 0x0039 // Needs to be recalculated
};

FingerprintPacket PS_RestSetting = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x3B, // Restore Factory Settings
    .parameters = {0}, // No parameters
    .checksum = 0x003E // Needs to be recalculated
};

FingerprintPacket PS_ReadINFpage = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x16, // Read Flash Information Page
    .parameters = {0}, // No parameters
    .checksum = 0x0019 // Needs to be recalculated
};

FingerprintPacket PS_BurnCode = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x1A, // Erase Code
    .parameters = {0x01}, // Default upgrade mode
    .checksum = 0x001F // Needs to be recalculated
};

FingerprintPacket PS_SetPwd = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x12, // Set Password
    .parameters = {0}, // Password (modifiable)
    .checksum = 0x0019 // Needs to be recalculated
};

FingerprintPacket PS_VfyPwd = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x13, // Verify Password
    .parameters = {0}, // Password
    .checksum = 0x001A // Needs to be recalculated
};

FingerprintPacket PS_GetRandomCode = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x14, // Get Random Number
    .parameters = {0}, // No parameters
    .checksum = 0x0017 // Needs to be recalculated
};

FingerprintPacket PS_WriteNotepad = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0023, // 32 bytes data
    .command = 0x18, // Write Notepad
    .parameters = {0}, // Data to write (to be filled)
    .checksum = 0x003B // Needs to be recalculated
};

FingerprintPacket PS_ReadNotepad = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x19, // Read Notepad
    .parameters = {0x00}, // Page number
    .checksum = 0x001E // Needs to be recalculated
};

FingerprintPacket PS_HandShake = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x35, // Handshake
    .parameters = {0}, // No parameters
    .checksum = 0x0039 // Needs to be recalculated
};

FingerprintPacket PS_ControlBLN = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x3C, // Control LED
    .parameters = {0}, // Example parameters: function, start color, end color, cycles
    .checksum = 0x0046 // Needs to be recalculated
};

FingerprintPacket PS_GetImageInfo = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x3D, // Get Image Information
    .parameters = {0}, // No parameters
    .checksum = 0x0041 // Needs to be recalculated
};

FingerprintPacket PS_SearchNow = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x3E, // Search Now
    .parameters = {0}, // Start Page, Number of Pages
    .checksum = 0x0046 // Needs to be recalculated
};

FingerprintPacket PS_ValidTempleteNum = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x1D, // Get number of valid templates
    .parameters = {0}, // No parameters
    .checksum = 0x0021 // Needs to be recalculated
};

FingerprintPacket PS_Sleep = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x33, // Enter sleep mode
    .parameters = {0}, // No parameters
    .checksum = 0x0037 // Needs to be recalculated
};

FingerprintPacket PS_LockKeyt = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0xE1, // Lock Key Pair
    .parameters = {0}, // No parameters
    .checksum = 0x00E4 // Needs to be recalculated
};

FingerprintPacket PS_GetCiphertext = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0xE2, // Get Ciphertext Random Number
    .parameters = {0}, // No parameters
    .checksum = 0x00E5 // Needs to be recalculated
};

FingerprintPacket PS_GetChipSN = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x13, // Get Chip Serial Number
    .parameters = {0}, // No parameters
    .checksum = 0x0016 // Needs to be recalculated
};

FingerprintPacket PS_GetEnrollImage = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x29, // Register Get Image
    .parameters = {0}, // No parameters
    .checksum = 0x002D // Needs to be recalculated
};

FingerprintPacket PS_WriteReg = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0005,
    .command = 0x0E, // Write System Register
    .parameters = {0}, // Register Number, Value (modifiable)
    .checksum = 0x0013 // Needs to be recalculated
};

FingerprintPacket PS_ReadIndexTable = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x1F, // Read Index Table
    .parameters = {0}, // Page Number
    .checksum = 0x0023 // Needs to be recalculated
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
    PS_GenChar1.parameters[0] = 0x01; // Buffer ID 1
    PS_GenChar2.parameters[0] = 0x02; // Buffer ID 2
    uart_config_t uart_config = {
        .baud_rate = 57600,  // Adjust based on your fingerprint module
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    esp_err_t err; 
    err = uart_driver_install(UART_NUM, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver");
        return err;
    }
    err = uart_param_config(UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART");
        return err;
    }
    err = uart_set_pin(UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
        return err;
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
    cmd->packet_id = 0x01;  // Command packet
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
    fingerprint_status_event_handler((fingerprint_status_t)packet->command);  // Trigger event based on status code
    return packet;  // Caller must free this memory after use
}


fingerprint_status_t fingerprint_get_status(FingerprintPacket *packet) {
    if (!packet) {
        return FINGERPRINT_ILLEGAL_DATA; // Return a default error if the packet is NULL
    }

    return (fingerprint_status_t)packet->command; // The command field stores the status code
}

fingerprint_status_t fingerprint_scan(void) {
    uint8_t command[] = {0x55, 0xAA, 0x01, 0x00, 0x01, 0x00};  // Example scan command
    uint8_t response[RX_BUF_SIZE];

    // Send the scan command
    esp_err_t err = uart_write_bytes(UART_NUM, (const char*)command, sizeof(command));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to send fingerprint scan command");
        // Trigger an error event for UART read failure
        fingerprint_status_event_handler(FINGERPRINT_PACKET_ERROR);  // Trigger event on error
        return FINGERPRINT_PACKET_ERROR;
    }

    memset(response, 0, RX_BUF_SIZE);

    // Read response from fingerprint module
    int len = uart_read_bytes(UART_NUM, response, RX_BUF_SIZE, 1000 / portTICK_PERIOD_MS);
    if (len <= 0) {
        ESP_LOGW(TAG, "No response from fingerprint module.");
        return FINGERPRINT_TIMEOUT;
    }

    // Ensure the response is valid
    if (len < 9) {  // Adjust this based on your fingerprint module's protocol
        ESP_LOGE(TAG, "Invalid response length: %d", len);
        return FINGERPRINT_PACKET_ERROR;
    }

    // Extract status code from response (adjust based on protocol format)
    uint8_t status_code = response[6];

    // Map the received status code to fingerprint_status_t
    fingerprint_status_t status = (fingerprint_status_t)status_code;

    // Log the status for debugging
    ESP_LOGI(TAG, "Fingerprint scan response: 0x%02X", status_code);

    return status;
}

esp_err_t fingerprint_enroll(int id) {
    ESP_LOGI(TAG, "Enrolling fingerprint with ID %d...", id);

    uint8_t enroll_command[] = {0x55, 0xAA, 0x02, id, 0x01, 0x00};  // Example command
    esp_err_t err = uart_write_bytes(UART_NUM, (const char*)enroll_command, sizeof(enroll_command));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to send enroll command");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(500));  // Simulate processing delay

    ESP_LOGI(TAG, "Fingerprint enrollment successful for ID %d", id);
    return ESP_OK;
}

esp_err_t fingerprint_delete(int id) {
    ESP_LOGI(TAG, "Deleting fingerprint ID %d...", id);

    uint8_t delete_command[] = {0x55, 0xAA, 0x03, id, 0x01, 0x00};  // Example command
    esp_err_t err = uart_write_bytes(UART_NUM, (const char*)delete_command, sizeof(delete_command));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to send delete command");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(300));  // Simulate processing delay

    ESP_LOGI(TAG, "Fingerprint ID %d deleted successfully", id);
    return ESP_OK;
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