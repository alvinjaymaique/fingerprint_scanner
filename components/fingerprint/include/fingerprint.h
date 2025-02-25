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
 * ## Event-driven System
 * This system operates on an event-driven architecture where each key operation, such as image capture, feature extraction, or match status, triggers specific events. The events help the application know when an operation is complete or has failed. 
 * The following events are generated based on the corresponding system actions.
 */

 /**
 * @brief Event handler for processing fingerprint events.
 *
 * This function processes various fingerprint-related events and logs messages 
 * based on the event type. It can be used as a sample for handling fingerprint events 
 * in the application. The events are processed based on the `fingerprint_event_t` 
 * enumeration, and corresponding messages are logged using the ESP-IDF logging system.
 *
 * @param event The fingerprint event that occurred. This can be one of the following:
 *      - `EVENT_FINGER_DETECTED`: Triggered when a finger is detected.
 *      - `EVENT_IMAGE_CAPTURED`: Triggered when the fingerprint image is captured successfully.
 *      - `EVENT_FEATURE_EXTRACTED`: Triggered when the fingerprint features are extracted successfully.
 *      - `EVENT_MATCH_SUCCESS`: Triggered when a fingerprint match is successful.
 *      - `EVENT_MATCH_FAIL`: Triggered when a fingerprint mismatch occurs.
 *      - `EVENT_ERROR`: Triggered for general errors during fingerprint processing.
 *
 * This function logs the corresponding message for each event. It can be modified or 
 * extended as needed for custom event handling in the application.
 *
 * @note To use this function in the application, assign it to the global event handler 
 *       `g_fingerprint_event_handler` in the `app_main.c`.
 *
 * @code
 * // Sample code for event handler
 * void handle_fingerprint_event(fingerprint_event_t event) {
 *     switch (event) {
 *         case EVENT_FINGER_DETECTED:
 *             ESP_LOGI("Fingerprint", "Finger detected!");
 *             break;
 *         case EVENT_IMAGE_CAPTURED:
 *             ESP_LOGI("Fingerprint", "Fingerprint image captured successfully!");
 *             break;
 *         case EVENT_FEATURE_EXTRACTED:
 *             ESP_LOGI("Fingerprint", "Fingerprint features extracted successfully!");
 *             break;
 *         case EVENT_MATCH_SUCCESS:
 *             ESP_LOGI("Fingerprint", "Fingerprint match successful!");
 *             break;
 *         case EVENT_MATCH_FAIL:
 *             ESP_LOGI("Fingerprint", "Fingerprint mismatch.");
 *             break;
 *         case EVENT_ERROR:
 *             ESP_LOGI("Fingerprint", "An error occurred during fingerprint processing.");
 *             break;
 *         default:
 *             ESP_LOGI("Fingerprint", "Unknown event triggered.");
 *             break;
 *     }
 * }
 * @endcode
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
 * @brief Timeout value for UART read operations.
 *
 * Defines the maximum wait time for receiving data over UART in milliseconds.
 * Adjust this value based on the fingerprint module's response time.
 */
#define UART_READ_TIMEOUT 100  // Adjust based on hardware response time

/**
 * @struct FingerprintPacket
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
} FingerprintPacket;

/**
 * @brief Captures a fingerprint image from the scanner's sensor.
 */
extern FingerprintPacket PS_GetImage;

/**
 * @brief Generates a character file in Buffer 1.
 */
extern FingerprintPacket PS_GenChar1;

/**
 * @brief Generates a character file in Buffer 2.
 */
extern FingerprintPacket PS_GenChar2;

/**
 * @brief Combines feature templates stored in Buffer 1 and Buffer 2 into a single fingerprint model.
 */
extern FingerprintPacket PS_RegModel;

/**
 * @brief Searches for a fingerprint match in the database.
 */
extern FingerprintPacket PS_Search;

/**
 * @brief Matches two fingerprint templates stored in RAM.
 */
extern FingerprintPacket PS_Match;

/**
 * @brief Stores a fingerprint template from the buffer to the database.
 */
extern FingerprintPacket PS_StoreChar;

/**
 * @brief Deletes a specific fingerprint template from the database.
 */
extern FingerprintPacket PS_DeletChar;

/**
 * @brief Clears all stored fingerprints (factory reset).
 */
extern FingerprintPacket PS_Empty;

/**
 * @brief Reads system parameters from the fingerprint module.
 */
extern FingerprintPacket PS_ReadSysPara;

/**
 * @brief Sets the fingerprint scanner's device address.
 */
extern FingerprintPacket PS_SetChipAddr;

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
 * @brief Initializes the fingerprint scanner.
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t fingerprint_init(void);

/**
 * @brief Builds a fingerprint command packet.
 *
 * This function initializes a `FingerprintPacket` with the specified command and parameters.
 * It ensures the parameter length does not exceed 4 bytes and calculates the checksum.
 *
 * @param[out] cmd Pointer to the `FingerprintPacket` to be populated.
 * @param[in] command Command byte to be included in the packet.
 * @param[in] params Pointer to parameter array (can be NULL if no parameters).
 * @param[in] param_length Number of parameters (max 4).
 * @return 
 *      - `ESP_OK` on success.
 *      - `ESP_ERR_INVALID_ARG` if `cmd` is NULL.
 *      - `ESP_ERR_INVALID_SIZE` if `param_length` exceeds 4.
 */
esp_err_t fingerprint_build_command(FingerprintPacket *cmd, uint8_t command, uint8_t *params, uint8_t param_length);

/**
 * @brief Computes the checksum for a given FingerprintPacket structure.
 *
 * The checksum is the sum of all bytes from packet_id to parameters.
 *
 * @param[in] cmd Pointer to the FingerprintPacket structure.
 * @return The computed checksum.
 */
uint16_t fingerprint_calculate_checksum(FingerprintPacket *cmd);

/**
 * @brief Sends a fingerprint command packet to the fingerprint module.
 *
 * This function constructs a command packet, calculates its checksum,
 * and sends it via UART to the fingerprint module at the specified address.
 *
 * @param cmd Pointer to a FingerprintPacket structure containing the command details.
 * @param address The address of the fingerprint module. This can be configured as needed.
 * 
 * @return 
 * - ESP_OK on success
 * - ESP_ERR_NO_MEM if memory allocation fails
 * - ESP_FAIL if the command could not be fully sent over UART
 */
esp_err_t fingerprint_send_command(FingerprintPacket *cmd, uint32_t address);


/**
 * @brief Reads the response packet from UART and returns a dynamically allocated FingerprintPacket.
 *
 * @return Pointer to the received FingerprintPacket, or NULL on failure.
 *         The caller is responsible for freeing the allocated memory using `free()`.
 */
FingerprintPacket* fingerprint_read_response(void);

/**
 * @brief Get the status of the fingerprint operation from the response packet.
 *
 * This function extracts the confirmation code from the fingerprint sensor's response
 * and maps it to a predefined fingerprint_status_t enumeration.
 *
 * @param packet Pointer to the FingerprintPacket structure containing the response.
 *
 * @return fingerprint_status_t Status code representing the operation result.
 */
fingerprint_status_t fingerprint_get_status(FingerprintPacket *packet);

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





/**
 * @brief Enum to define various fingerprint events.
 *
 * These events represent significant milestones or errors during the fingerprint processing. 
 * They allow for event-driven management of the fingerprint sensor actions.
 * 
 * Each event is triggered when a certain action or error occurs, making it easier for the application to respond to changes in the system state.
 * 
 * @note The event codes are used in an event handler system where functions can subscribe and react to specific events.
 */
typedef enum {
    /**
     * @brief Event triggered when a finger is detected.
     *
     * Indicates that a finger has been placed on the fingerprint scanner.
     */
    EVENT_FINGER_DETECTED,          /**< Finger detected */

    /**
     * @brief Event triggered when a fingerprint image is captured successfully.
     *
     * Indicates that the fingerprint sensor has successfully captured an image.
     */
    EVENT_IMAGE_CAPTURED,           /**< Image captured */

    /**
     * @brief Event triggered when fingerprint features are successfully extracted.
     *
     * This event is generated after the sensor extracts features from the fingerprint image.
     */
    EVENT_FEATURE_EXTRACTED,        /**< Feature extraction completed */

    /**
     * @brief Event triggered when a fingerprint match is successful.
     *
     * Indicates that a fingerprint match has been found in the database.
     */
    EVENT_MATCH_SUCCESS,            /**< Fingerprint matched */

    /**
     * @brief Event triggered when a fingerprint mismatch occurs.
     *
     * Occurs when no matching fingerprint is found in the database.
     */
    EVENT_MATCH_FAIL,               /**< Fingerprint mismatch */

    /**
     * @brief A general error event, used for unexpected failures.
     *
     * This event signifies a general failure in the fingerprint system.
     */
    EVENT_ERROR,                    /**< General error */

    /**
     * @brief Event triggered when the fingerprint image capture fails.
     *
     * This event corresponds to the status `FINGERPRINT_IMAGE_FAIL`.
     */
    EVENT_IMAGE_FAIL,               /**< Image capture failure (FINGERPRINT_IMAGE_FAIL) */

    /**
     * @brief Event triggered when feature extraction fails.
     *
     * This event corresponds to the status `FINGERPRINT_TOO_FEW_POINTS`.
     */
    EVENT_FEATURE_EXTRACT_FAIL,     /**< Feature extraction failure (FINGERPRINT_TOO_FEW_POINTS) */

    /**
     * @brief Event triggered when the fingerprint database is full.
     *
     * This event corresponds to the status `FINGERPRINT_DB_FULL`.
     */
    EVENT_DB_FULL,                  /**< Database full (FINGERPRINT_DB_FULL) */

    /**
     * @brief Event triggered when there is a sensor operation failure.
     *
     * This event corresponds to the status `FINGERPRINT_SENSOR_OP_FAIL`.
     */
    EVENT_SENSOR_ERROR              /**< Sensor operation failure (FINGERPRINT_SENSOR_OP_FAIL) */
} fingerprint_event_t;

/**
 * @brief Typedef for the fingerprint event handler callback.
 *
 * This type is used to define a function pointer for handling fingerprint-related events.
 * The function pointed to by the callback should take a single parameter of type `fingerprint_event_t`,
 * representing the specific event that occurred.
 *
 * @param event The fingerprint event that occurred. This is an enumeration value of type `fingerprint_event_t`.
 */
typedef void (*fingerprint_event_handler_t)(fingerprint_event_t event);

/**
 * @brief Global pointer to the fingerprint event handler function.
 *
 * This pointer is used to assign a function that will handle fingerprint-related events.
 * The function should be defined by the user and should match the `fingerprint_event_handler_t` callback type.
 * The event handler is called whenever a fingerprint-related event occurs, such as detection, image capture, or match results.
 */
extern fingerprint_event_handler_t g_fingerprint_event_handler;

/**
 * @brief Function to register an event handler for fingerprint events.
 * 
 * This function allows the application to register a callback handler that will
 * be called when a fingerprint event is triggered. The handler will process the
 * event according to its type (e.g., finger detected, match success, etc.).
 * 
 * @param handler The function pointer to the event handler function.
 */
void register_fingerprint_event_handler(void (*handler)(fingerprint_event_t));

/**
 * @brief Function to trigger a fingerprint event.
 * 
 * This function is called to trigger a specific fingerprint event. It will invoke
 * the registered event handler (if any) with the specified event.
 * 
 * @param event The fingerprint event to trigger.
 */
void trigger_fingerprint_event(fingerprint_event_t event);

#ifdef __cplusplus
}
#endif

#endif // FINGERPRINT_H