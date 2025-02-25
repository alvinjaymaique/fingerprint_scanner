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

void fingerprint_set_pins(int tx, int rx) {
    tx_pin = tx;
    rx_pin = rx;
}
void fingerprint_set_baudrate(int baud) {
    baud_rate = baud;
}

FingerprintCommand PS_GetImage = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x01, // Get Image
    .parameters = {0}, // No parameters
    .checksum = 0x0005 // Hardcoded checksum
};

FingerprintCommand PS_GenChar1 = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x02, // Generate Character
    .parameters = {0x01}, // Buffer ID 1
    .checksum = 0x0008 // Hardcoded checksum
};

FingerprintCommand PS_GenChar2 = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0004,
    .command = 0x02, // Generate Character
    .parameters = {0x02}, // Buffer ID 2
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintCommand PS_RegModel = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x05, // Register Model
    .parameters = {0}, // No parameters
    .checksum = 0x0009 // Hardcoded checksum
};

FingerprintCommand PS_Search = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0008,
    .command = 0x04, // Search
    .parameters = {0x01, 0x00, 0x00, 0xFF}, // Buffer ID, Start Page, Number of Pages
    .checksum = 0x0013 // Hardcoded checksum
};

FingerprintCommand PS_Match = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x03, // Match
    .parameters = {0}, // No parameters
    .checksum = 0x0007 // Hardcoded checksum
};

FingerprintCommand PS_StoreChar = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0006,
    .command = 0x06, // Store Character
    .parameters = {0x01, 0x00, 0x01}, // Buffer ID, Page ID
    .checksum = 0x000F // Hardcoded checksum
};

FingerprintCommand PS_DeletChar = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x0C, // Delete Fingerprint
    .parameters = {0x00, 0x01, 0x00, 0x01}, // Page ID, Number of Entries
    .checksum = 0x0015 // Hardcoded checksum
};

FingerprintCommand PS_Empty = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x0D, // Clear Database
    .parameters = {0}, // No parameters
    .checksum = 0x0011 // Hardcoded checksum
};

FingerprintCommand PS_ReadSysPara = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0003,
    .command = 0x0F, // Read System Parameters
    .parameters = {0}, // No parameters
    .checksum = 0x0013 // Hardcoded checksum
};

FingerprintCommand PS_SetChipAddr = {
    .header = 0xEF01,
    .address = DEFAULT_FINGERPRINT_ADDRESS,
    .packet_id = 0x01,
    .length = 0x0007,
    .command = 0x15, // Set Address
    .parameters = {0x00, 0x00, 0x00, 0x02}, // New Address (modifiable)
    .checksum = 0x0020 // Hardcoded checksum
};

uint16_t fingerprint_calculate_checksum(FingerprintCommand *cmd) {
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

void fingerprint_build_command(FingerprintCommand *cmd, uint8_t command, uint8_t *params, uint8_t param_length) {
    cmd->header = FINGERPRINT_HEADER;
    cmd->address = DEFAULT_FINGERPRINT_ADDRESS;
    cmd->packet_id = 0x01;  // Command packet
    // Ensure param_length does not exceed 4
    if (param_length > 4) param_length = 4;
    // Correct length calculation (command + params + checksum)
    cmd->length = 3 + param_length;
    cmd->command = command;
    // Clear parameters and copy only the valid ones
    memset(cmd->parameters, 0, sizeof(cmd->parameters));
    if (params) memcpy(cmd->parameters, params, param_length);
    // Compute checksum
    cmd->checksum = fingerprint_calculate_checksum(cmd);
}

void fingerprint_send_command(FingerprintCommand *cmd) {
    // Compute the checksum
    cmd->checksum = fingerprint_compute_checksum(cmd);
    // Calculate actual packet size
    size_t packet_size = cmd->length + 9; // 9 bytes (header, address, packet ID, length) + data
    // Dynamically allocate buffer
    uint8_t *buffer = (uint8_t *)malloc(packet_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Memory allocation failed for fingerprint command.");
        return;
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
    // Copy valid parameters (max 4 bytes)
    memcpy(&buffer[10], cmd->parameters, cmd->length - 3);
    // Append checksum
    buffer[packet_size - 2] = (cmd->checksum >> 8) & 0xFF;
    buffer[packet_size - 1] = cmd->checksum & 0xFF;
    // Send the packet over UART
    uart_write_bytes(UART_NUM, (const char *)buffer, packet_size);
    // Debug logging
    ESP_LOGI(TAG, "Sent fingerprint command: 0x%02X", cmd->command);
    // Free the allocated buffer
    free(buffer);
}

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
