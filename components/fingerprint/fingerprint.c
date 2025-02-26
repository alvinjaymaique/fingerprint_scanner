#include "include/fingerprint.h"  // Direct include instead of "include/fingerprint.h"
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
    .parameters = {0x01}, // Buffer ID 1
    .checksum = 0x0008 // Hardcoded checksum
};

FingerprintPacket PS_GenChar2 = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x02, // Generate Character
    .parameters = {0x02}, // Buffer ID 2
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
    .parameters = {0x00, 0x00, 0x00, 0x00, 0x00}, // Buffer ID, Start Page, Number of Pages
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
    .parameters = {0x01, 0x00, 0x01}, // Buffer ID, Page ID
    .checksum = 0x000F // Hardcoded checksum
};

FingerprintPacket PS_DeletChar = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x0C, // Delete Fingerprint
    .parameters = {0x00, 0x01, 0x00, 0x01}, // Page ID, Number of Entries
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
    .parameters = {0x00, 0x00, 0x00, 0x02}, // New Address (modifiable)
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
    .parameters = {0x00, 0x01, 0x02, 0x00, 0x00}, // ID number, number of entries, parameter
    .checksum = 0x003A // Needs to be recalculated
};

FingerprintPacket PS_Autoldentify = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0006,
    .command = 0x32, // AutoIdentify command
    .parameters = {0x00, 0x12, 0x00}, // Score level, ID number
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
    .parameters = {0x01, 0x00, 0x00, 0xFF}, // Buffer ID, Start Page, Number of Pages
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
    .parameters = {0x00, 0x00, 0x00, 0x00}, // Password (modifiable)
    .checksum = 0x0019 // Needs to be recalculated
};

FingerprintPacket PS_VfyPwd = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x13, // Verify Password
    .parameters = {0x00, 0x00, 0x00, 0x00}, // Password
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


esp_err_t fingerprint_init(void) {
    ESP_LOGI(TAG, "Initializing fingerprint scanner...");
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

    if (param_length > 5) {  // Allow up to 5 bytes for commands like PS_Search
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

ExtendedPacket createExtendedPacket(FingerprintPacket base_packet, uint8_t page_number, const uint8_t *data, size_t data_size) {
    ExtendedPacket packet;
    packet.base = base_packet;
    packet.base.length = 0x24;  // Fixed length for WriteNotepad command
    packet.base.command = 0x18; // Command for WriteNotepad
    packet.base.parameters[0] = page_number; // Store page number

    // Ensure data_size is at most 32 bytes
    if (data_size > 32) {
        printf("Warning: Data exceeds 32 bytes, truncating...\n");
        data_size = 32;  // Truncate if larger
    }

    // Copy provided data and pad with zeros if needed
    memset(packet.data, 0, 32);  // Zero out entire buffer
    memcpy(packet.data, data, data_size);  // Copy actual data

    // Compute checksum
    uint16_t checksum = packet.base.packet_id + packet.base.length + packet.base.command + page_number;
    for (int i = 0; i < 32; i++) {
        checksum += packet.data[i];
    }
    packet.base.checksum = checksum;

    return packet;
}


uint16_t fingerprint_calculate_checksum(FingerprintPacket *cmd) {
    uint16_t sum = 0;
    sum += cmd->packet_id;
    sum += (cmd->length >> 8) & 0xFF; // High byte of length
    sum += cmd->length & 0xFF;        // Low byte of length
    sum += cmd->command;
    for (int i = 0; i < 4; i++) {
        sum += cmd->parameters[i]; // Sum all parameters
    }
    return sum;
}

esp_err_t fingerprint_send_command(FingerprintPacket *cmd, uint32_t address) {
    // Compute the checksum
    cmd->checksum = fingerprint_calculate_checksum(cmd);
    
    // Calculate actual packet size
    size_t packet_size = cmd->length + 9; // 9 bytes (header, address, packet ID, length) + data
    
    // Dynamically allocate buffer
    uint8_t *buffer = (uint8_t *)malloc(packet_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Memory allocation failed for fingerprint command.");
        return ESP_ERR_NO_MEM;  // Return memory allocation error
    }

    // Construct the packet
    buffer[0] = (cmd->header >> 8) & 0xFF;
    buffer[1] = cmd->header & 0xFF;
    buffer[2] = (address >> 24) & 0xFF;  // Use function parameter instead of cmd->address
    buffer[3] = (address >> 16) & 0xFF;
    buffer[4] = (address >> 8) & 0xFF;
    buffer[5] = address & 0xFF;
    buffer[6] = cmd->packet_id;
    buffer[7] = (cmd->length >> 8) & 0xFF;
    buffer[8] = cmd->length & 0xFF;
    buffer[9] = cmd->command;
    
    // Copy valid parameters (max 4 bytes)
    memcpy(&buffer[10], cmd->parameters, cmd->length - 3);

    // Append checksum
    buffer[packet_size - 2] = (cmd->checksum >> 8) & 0xFF;
    buffer[packet_size - 1] = cmd->checksum & 0xFF;

    // Send the packet over UART
    int bytes_written = uart_write_bytes(UART_NUM, (const char *)buffer, packet_size);
    if (bytes_written != packet_size) {
        ESP_LOGE(TAG, "Failed to send the complete fingerprint command.");
        free(buffer);
        return ESP_FAIL;  // Return failure if not all bytes were written
    }

    // Debug logging
    ESP_LOGI(TAG, "Sent fingerprint command: 0x%02X to address 0x%08X", cmd->command, (unsigned int)address);

    // Free the allocated buffer
    free(buffer);
    
    return ESP_OK;  // Return success
}

// Function to read the response packet from UART and return the FingerprintPacket structure
FingerprintPacket* fingerprint_read_response(void) {
    FingerprintPacket *packet = (FingerprintPacket*)malloc(sizeof(FingerprintPacket));
    if (!packet) {
        ESP_LOGE("Fingerprint", "Memory allocation failed!");
        return NULL;
    }

    uint8_t buffer[16];  // Adjust buffer size based on expected packet length
    int length = 0;

    memset(packet, 0, sizeof(FingerprintPacket));  // Initialize allocated memory

    // Read response packet from UART (waiting until timeout)
    length = uart_read_bytes(UART_NUM, buffer, sizeof(buffer), UART_READ_TIMEOUT);
    
    if (length <= 0) {
        ESP_LOGE("Fingerprint", "Failed to read data from UART");
        free(packet);  // Free memory before returning
        return NULL;
    }

    // Assuming the buffer contains a full packet, copy data to FingerprintPacket struct
    packet->header = (buffer[0] << 8) | buffer[1];
    packet->address = (buffer[2] << 24) | (buffer[3] << 16) | (buffer[4] << 8) | buffer[5];
    packet->packet_id = buffer[6];
    packet->length = (buffer[7] << 8) | buffer[8];
    packet->command = buffer[9];
    memcpy(packet->parameters, &buffer[10], sizeof(packet->parameters));
    packet->checksum = (buffer[length - 2] << 8) | buffer[length - 1];

    // Verify the checksum (optional but recommended)
    uint16_t computed_checksum = fingerprint_calculate_checksum(packet);
    if (computed_checksum != packet->checksum) {
        ESP_LOGE("Fingerprint", "Checksum mismatch! Computed: 0x%04X, Received: 0x%04X", computed_checksum, packet->checksum);
        free(packet);  // Free memory before returning
        return NULL;
    }

    ESP_LOGI("Fingerprint", "Response read successfully: Command 0x%02X", packet->command);
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

// Function to register the event handler
void register_fingerprint_event_handler(fingerprint_event_handler_t handler) {
    g_fingerprint_event_handler = handler;
}

// Function to trigger the event (you can call this inside your fingerprint processing flow)
void trigger_fingerprint_event(fingerprint_event_t event) {
    if (g_fingerprint_event_handler != NULL) {
        // Call the registered event handler
        g_fingerprint_event_handler(event);
    } else {
        // No handler registered, handle error or provide default behavior
        ESP_LOGE("Fingerprint", "No event handler registered.");
    }
}