/**
 * @file fingerprint.h
 * @brief Fingerprint sensor driver (ZW111) for ESP32
 *
 * ## 1. Hardware Interface Description
 * ### 1.1 UART
 * - Default baud rate: 57600 bps (8 data bits, 1 stop bit, no parity)
 * - Baud rate adjustable: 9600 to 115200 bps
 * - Direct connection to MCU (3.3V) or use RS232 level converter for PC
 * 
 * ### 1.2 Power-on Sequence:
 * 1. Host receives fingerprint module (FPM) interrupt wake-up signal.
 * 2. Host powers on Vmcu (MCU power supply) **before** initializing UART.
 * 3. After communication completes, pull down serial signal lines **before** powering off Vmcu.
 * 
 * ### 1.3 Connection
 * - **TX** → ESP32 GPIO17
 * - **RX** → ESP32 GPIO16
 * - **VCC** → 3.3V
 * - **GND** → GND
 *
 */

#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

/**
 * @brief Default UART baud rate for fingerprint module.
 */
#define DEFAULT_BAUD_RATE 57600

/**
 * @brief Default UART pins (modifiable at runtime).
 */
#define DEFAULT_TX_PIN 17
#define DEFAULT_RX_PIN 16

/**
 * @brief Default fingerprint module header identifier.
 */
#define FINGERPRINT_HEADER 0xEF01

/**
 * @brief Default fingerprint module address (broadcast address).
 */
#define DEFAULT_FINGERPRINT_ADDRESS 0xFFFFFFFF

/**
 * @struct FingerprintCommand
 * @brief Structure representing a fingerprint module command packet.
 *
 * This structure defines the format of a command packet sent to the fingerprint scanner.
 */
typedef struct {
    uint16_t header;      /**< Fixed header (0xEF01) indicating the start of a packet. */
    uint32_t address;     /**< Address of the fingerprint module (4 bytes). */
    uint8_t packet_id;    /**< Packet type identifier (e.g., 0x01 for command packets). */
    uint16_t length;      /**< Total length of the packet excluding header and address. */
    uint8_t command;      /**< Command ID (e.g., 0x01 for capturing an image). */
    uint8_t parameters[4];/**< Command-specific parameters (varies per command). */
    uint16_t checksum;    /**< Checksum for packet integrity validation. */
} FingerprintCommand;

/**
 * @brief Captures a fingerprint image from the scanner's sensor.
 */
extern FingerprintCommand PS_GetImage;

/**
 * @brief Generates a character file in Buffer 1.
 */
extern FingerprintCommand PS_GenChar1;

/**
 * @brief Generates a character file in Buffer 2.
 */
extern FingerprintCommand PS_GenChar2;

/**
 * @brief Combines feature templates stored in Buffer 1 and Buffer 2 into a single fingerprint model.
 */
extern FingerprintCommand PS_RegModel;

/**
 * @brief Searches for a fingerprint match in the database.
 */
extern FingerprintCommand PS_Search;

/**
 * @brief Matches two fingerprint templates stored in RAM.
 */
extern FingerprintCommand PS_Match;

/**
 * @brief Stores a fingerprint template from the buffer to the database.
 */
extern FingerprintCommand PS_StoreChar;

/**
 * @brief Deletes a specific fingerprint template from the database.
 */
extern FingerprintCommand PS_DeletChar;

/**
 * @brief Clears all stored fingerprints (factory reset).
 */
extern FingerprintCommand PS_Empty;

/**
 * @brief Reads system parameters from the fingerprint module.
 */
extern FingerprintCommand PS_ReadSysPara;

/**
 * @brief Sets the fingerprint scanner's device address.
 */
extern FingerprintCommand PS_SetChipAddr;

/**
 * @brief Fingerprint sensor status codes.
 */
typedef enum {
    FINGERPRINT_OK = 0x00,                      // Instruction execution completed
    FINGERPRINT_PACKET_ERROR = 0x01,            // Data packet reception error
    FINGERPRINT_NO_FINGER = 0x02,               // No finger detected
    FINGERPRINT_IMAGE_FAIL = 0x03,              // Failed to enter fingerprint image
    FINGERPRINT_TOO_DRY = 0x04,                 // Image too dry/light
    FINGERPRINT_TOO_WET = 0x05,                 // Image too wet/muddy
    FINGERPRINT_TOO_CHAOTIC = 0x06,             // Image too chaotic
    FINGERPRINT_TOO_FEW_POINTS = 0x07,          // Normal image, but not enough features
    FINGERPRINT_MISMATCH = 0x08,                // Fingerprint mismatch
    FINGERPRINT_NOT_FOUND = 0x09,               // No fingerprint found
    FINGERPRINT_MERGE_FAIL = 0x0A,              // Feature merging failed
    FINGERPRINT_DB_RANGE_ERROR = 0x0B,          // Address out of range in database
    FINGERPRINT_READ_TEMPLATE_ERROR = 0x0C,     // Error reading fingerprint template
    FINGERPRINT_UPLOAD_FEATURE_FAIL = 0x0D,     // Failed to upload features
    FINGERPRINT_DATA_PACKET_ERROR = 0x0E,       // Cannot receive subsequent data packets
    FINGERPRINT_UPLOAD_IMAGE_FAIL = 0x0F,       // Failed to upload image
    FINGERPRINT_DELETE_TEMPLATE_FAIL = 0x10,    // Failed to delete template
    FINGERPRINT_DB_CLEAR_FAIL = 0x11,           // Failed to clear database
    FINGERPRINT_LOW_POWER_FAIL = 0x12,          // Cannot enter low power mode
    FINGERPRINT_WRONG_PASSWORD = 0x13,          // Incorrect password
    FINGERPRINT_NO_VALID_IMAGE = 0x15,          // No valid original image in buffer
    FINGERPRINT_UPGRADE_FAIL = 0x16,            // Online upgrade failed
    FINGERPRINT_RESIDUAL_FINGER = 0x17,         // Residual fingerprint detected
    FINGERPRINT_FLASH_RW_ERROR = 0x18,          // Flash read/write error
    FINGERPRINT_RANDOM_GEN_FAIL = 0x19,         // Random number generation failed
    FINGERPRINT_INVALID_REGISTER = 0x1A,        // Invalid register number
    FINGERPRINT_REGISTER_SETTING_ERROR = 0x1B,  // Register setting content error
    FINGERPRINT_NOTEPAD_PAGE_ERROR = 0x1C,      // Incorrect notepad page number
    FINGERPRINT_PORT_OP_FAIL = 0x1D,            // Port operation failed
    FINGERPRINT_ENROLL_FAIL = 0x1E,             // Automatic registration failed
    FINGERPRINT_DB_FULL = 0x1F,                 // Fingerprint database full
    FINGERPRINT_DEVICE_ADDRESS_ERROR = 0x20,    // Device address error
    FINGERPRINT_TEMPLATE_NOT_EMPTY = 0x22,      // Template is not empty
    FINGERPRINT_TEMPLATE_EMPTY = 0x23,          // Template is empty
    FINGERPRINT_DB_EMPTY = 0x24,                // Database is empty
    FINGERPRINT_ENTRY_COUNT_ERROR = 0x25,       // Incorrect entry count
    FINGERPRINT_TIMEOUT = 0x26,                 // Timeout occurred
    FINGERPRINT_ALREADY_EXISTS = 0x27,          // Fingerprint already exists
    FINGERPRINT_FEATURES_RELATED = 0x28,        // Features are related
    FINGERPRINT_SENSOR_OP_FAIL = 0x29,          // Sensor operation failed
    FINGERPRINT_MODULE_INFO_NOT_EMPTY = 0x2A,   // Module info not empty
    FINGERPRINT_MODULE_INFO_EMPTY = 0x2B,       // Module info empty
    FINGERPRINT_OTP_FAIL = 0x2C,                // OTP operation failed
    FINGERPRINT_KEY_GEN_FAIL = 0x2D,            // Key generation failed
    FINGERPRINT_KEY_NOT_EXIST = 0x2E,           // Secret key does not exist
    FINGERPRINT_SECURITY_ALGO_FAIL = 0x2F,      // Security algorithm execution failed
    FINGERPRINT_ENCRYPTION_MISMATCH = 0x30,     // Encryption and function mismatch
    FINGERPRINT_KEY_LOCKED = 0x32,              // Secret key is locked
    FINGERPRINT_IMAGE_AREA_SMALL = 0x33,        // Image area too small
    FINGERPRINT_IMAGE_NOT_AVAILABLE = 0x34,     // Image not available
    FINGERPRINT_ILLEGAL_DATA = 0x35             // Illegal data
} fingerprint_status_t;

/**
 * @brief Computes the checksum for a given FingerprintCommand structure.
 *
 * The checksum is the sum of all bytes from packet_id to parameters.
 *
 * @param[in] cmd Pointer to the FingerprintCommand structure.
 * @return The computed checksum.
 */
uint16_t fingerprint_calculate_checksum(FingerprintCommand *cmd);

/**
 * @brief Sends a command packet to the fingerprint scanner.
 *
 * This function transmits a properly formatted command packet over UART 
 * to the fingerprint scanner.
 *
 * @param[in] cmd Pointer to the FingerprintCommand structure containing the command.
 */
void fingerprint_send_command(FingerprintCommand *cmd);

/**
 * @brief Builds a fingerprint command packet dynamically.
 *
 * This function initializes a FingerprintCommand structure with the given command
 * and parameters, computes its checksum, and prepares it for sending.
 *
 * @param[out] cmd Pointer to the FingerprintCommand structure to be populated.
 * @param[in] command The fingerprint command byte.
 * @param[in] params Pointer to an array of command-specific parameters.
 * @param[in] param_length Number of parameters (max 4).
 */
void fingerprint_build_command(FingerprintCommand *cmd, uint8_t command, uint8_t *params, uint8_t param_length);

/**
 * @brief Initializes the fingerprint scanner.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fingerprint_init(void);

/**
 * @brief Scans for a fingerprint and returns the status.
 *
 * This function sends a scan command to the fingerprint module,
 * waits for a response, and maps the received status code to the
 * corresponding fingerprint_status_t value.
 *
 * @return fingerprint_status_t The status of the fingerprint scan.
 */
fingerprint_status_t fingerprint_scan(void);

/**
 * @brief Enrolls a fingerprint.
 *
 * @param id ID of the fingerprint to enroll.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fingerprint_enroll(int id);

/**
 * @brief Deletes a stored fingerprint.
 *
 * @param id ID of the fingerprint to delete.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fingerprint_delete(int id);

/**
 * @brief Sets the UART TX and RX pins for the fingerprint sensor.
 *
 * This function allows dynamic configuration of the TX and RX pins
 * before initializing the fingerprint module.
 *
 * @param[in] tx GPIO number for the TX pin.
 * @param[in] rx GPIO number for the RX pin.
 */
void fingerprint_set_pins(int tx, int rx);

/**
 * @brief Sets the baud rate for fingerprint module communication.
 *
 * This function adjusts the baud rate used for UART communication
 * with the fingerprint scanner. It should be called before `fingerprint_init()`
 * to ensure proper communication settings.
 *
 * @param[in] baud The desired baud rate (e.g., 9600, 57600, 115200).
 */
void fingerprint_set_baudrate(int baud);

#ifdef __cplusplus
}
#endif

#endif // FINGERPRINT_H