/**
 * @file fingerprint.h
 * @brief Fingerprint sensor driver (ZW111) for ESP32
 *
 * ## 1. Introduction
 * This driver provides an interface for the ZW111 fingerprint sensor, enabling fingerprint 
 * enrollment, matching, and template storage. It is designed for embedded systems using the 
 * ESP32 microcontroller, providing an event-driven architecture for efficient processing.
 *
 * ### 1.1 Supported Platforms
 * - ESP32-based development boards
 * - ESP-IDF (Espressif IoT Development Framework)
 *
 * ### 1.2 Compiler and Toolchain
 * - GCC (ESP32 toolchain)
 * - Compatible with ESP-IDF v4.x or later
 *
 * ### 1.3 Dependencies
 * This driver requires the following ESP-IDF components:
 * - `esp_log.h` – Logging utilities for debugging and event tracking
 * - `driver/uart.h` – UART driver for serial communication with the fingerprint module
 * - `esp_err.h` – Standard ESP-IDF error handling
 * - `freertos/FreeRTOS.h` and `freertos/task.h` – FreeRTOS support for asynchronous processing
 * - `string.h` – Standard C library for string manipulation
 *
 *
 * ## 2. Hardware Interface Description
 * ### 2.1 UART
 * - Default baud rate: 57600 bps (8 data bits, 1 stop bit, no parity)
 * - Baud rate adjustable: 9600 to 115200 bps
 * - Direct connection to MCU (3.3V) or use RS232 level converter for PC
 * 
 * ### 2.2 Power-on Sequence:
 * 1. Host receives fingerprint module (FPM) interrupt wake-up signal.
 * 2. Host powers on Vmcu (MCU power supply) **before** initializing UART.
 * 3. After communication completes, pull down serial signal lines **before** powering off Vmcu.
 * 
 * ### 2.3 Connection
 * - **TX** → ESP32 GPIO17
 * - **RX** → ESP32 GPIO16
 * - **VCC** → 3.3V
 * - **GND** → GND
 *
 * ## 3. Event-driven System
 * This system operates on an event-driven architecture where each key operation, such as image capture, feature extraction, or match status, triggers specific events. The events help the application know when an operation is complete or has failed. 
 * The following events are generated based on the corresponding system actions.
 *
 * ### 3.1 Event Handler
 * @brief Event handler for processing fingerprint events.
 *
 * This function processes various fingerprint-related events and logs messages 
 * based on the event type. It can be used as a sample for handling fingerprint events 
 * in the application. The events are processed based on the `fingerprint_event_type_t` 
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
 *     switch (event.type) {
 *         case EVENT_SCANNER_READY:
 *             ESP_LOGI("Fingerprint", "Fingerprint scanner is ready for operation. Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_FINGER_DETECTED:
 *             ESP_LOGI("Fingerprint", "Finger detected! Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_IMAGE_CAPTURED:
 *             ESP_LOGI("Fingerprint", "Fingerprint image captured successfully! Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_FEATURE_EXTRACTED:
 *             ESP_LOGI("Fingerprint", "Fingerprint features extracted successfully! Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_MATCH_SUCCESS:
 *             ESP_LOGI("Fingerprint", "Fingerprint match successful! Status: 0x%02X", event.status);
 *             ESP_LOGI("Fingerprint", "Match found at Page ID: %d", 
 *                      (event.packet.parameters[1] << 8) | event.packet.parameters[0]);
 *             ESP_LOGI("Fingerprint", "Match score: %d", 
 *                      (event.packet.parameters[3] << 8) | event.packet.parameters[2]);
 *             break;
 *         case EVENT_MATCH_FAIL:
 *             ESP_LOGI("Fingerprint", "Fingerprint mismatch. Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_ERROR:
 *             ESP_LOGE("Fingerprint", "An error occurred during fingerprint processing. Status: 0x%02X", event.status);
 *             ESP_LOGE("Fingerprint", "Command: 0x%02X", event.packet.command);
 *             break;
 *         case EVENT_NO_FINGER_DETECTED:
 *             ESP_LOGI("Fingerprint", "No finger detected. Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_ENROLL_SUCCESS:
 *             ESP_LOGI("Fingerprint", "Fingerprint enrollment successful! Status: 0x%02X", event.status);
 *             ESP_LOGI("Fingerprint", "Event: 0x%02X", event.type);
 *             break;
 *         case EVENT_ENROLL_FAIL:
 *             ESP_LOGI("Fingerprint", "Fingerprint enrollment failed. Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_TEMPLATE_MERGED:
 *             ESP_LOGI("Fingerprint", "Fingerprint templates merged successfully. Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_TEMPLATE_STORE_SUCCESS:
 *             ESP_LOGI("Fingerprint", "Fingerprint template stored successfully. Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_SEARCH_SUCCESS:
 *             ESP_LOGI("Fingerprint", "Fingerprint search successful. Status: 0x%02X", event.status);
 *             ESP_LOGI("Fingerprint", "Match found at Page ID: %d", 
 *                      (event.packet.parameters[1] << 8) | event.packet.parameters[0]);
 *             ESP_LOGI("Fingerprint", "Match score: %d", 
 *                      (event.packet.parameters[3] << 8) | event.packet.parameters[2]);
 *             break;
 *         case EVENT_INDEX_TABLE_READ:
 *             ESP_LOGI("Fingerprint", "Index table read successful. Status: 0x%02X", event.status);
 *             break;
 *         default:
 *             ESP_LOGI("Fingerprint", "Unknown event triggered. Status: 0x%02X", event.status);
 *             break;
 *     }
 * }
 * @endcode
 * 
 *  * ## 4. Usage Guide
 * This section provides an overview of how to use the fingerprint library in an ESP-IDF project.
 *
 * ### 4.1 Initialization
 * Before sending commands, initialize the fingerprint module:
 *
 * @code
 * #include "fingerprint.h"
 *
 * void app_main() {
 *     if (fingerprint_init() == ESP_OK) {
 *         ESP_LOGI("Fingerprint", "Module initialized.");
 *     }
 * }
 * @endcode
 *
 * ### 4.2 Sending Commands
 * Use `fingerprint_send_command()` with predefined `PS_*` packets to communicate with the sensor.
 *
 * **Example: Capturing a fingerprint image**
 * @code
 * fingerprint_send_command(&PS_GetImage, DEFAULT_FINGERPRINT_ADDRESS);
 * @endcode
 *
 * **Example: Searching for a fingerprint**
 * @code
 * PS_Search.parameters[0] = 0x01;  // Buffer ID
 * PS_Search.parameters[1] = 0x00;  // Start Page
 * PS_Search.parameters[2] = 0x02;  // Number of Pages
 * PS_Search.checksum = fingerprint_calculate_checksum(&PS_Search);
 * fingerprint_send_command(&PS_Search, DEFAULT_FINGERPRINT_ADDRESS);
 * @endcode
 *
 * ### 4.3 Reading Responses
 * Use `fingerprint_read_response()` to retrieve the sensor’s response.
 *
 * @code
 * FingerprintPacket *response = fingerprint_read_response();
 * if (response) {
 *     ESP_LOGI("Fingerprint", "Received status: 0x%02X", response->command);
 *     free(response);  // Free allocated memory
 * }
 * @endcode
 *
 * ### 4.4 Handling Events
 * The system maps fingerprint sensor status codes to higher-level events.  
 * To handle events dynamically, register a callback function:
 *
 * @code
 * void my_event_handler(fingerprint_event_t event) {
 *     ESP_LOGI("Fingerprint", "Event: %d, Status: 0x%02X", event.type, event.status);
 * }
 *
 * void app_main() {
 *     register_fingerprint_event_handler(my_event_handler);
 * }
 * @endcode
 */


 #ifndef FINGERPRINT_H
 #define FINGERPRINT_H
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
#include "esp_err.h"
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
 
 /**
  * @brief Default UART baud rate for fingerprint module.
  */
 #define DEFAULT_BAUD_RATE 57600
 
 /**
  * @brief Default UART pins (modifiable at runtime).
  */
 #define DEFAULT_TX_PIN (GPIO_NUM_5) //17
 #define DEFAULT_RX_PIN (GPIO_NUM_6) //18
 
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
 * @brief Maximum number of parameters supported in a fingerprint packet.
 *
 * Defines the fixed-size buffer for command-specific parameters in fingerprint communication.
 * This value should be adjusted based on the largest expected parameter size to ensure
 * sufficient space while maintaining efficient memory usage.
 */
#define MAX_PARAMETERS 256  /**< Adjust based on the largest required parameter size. */

/**
 * @brief Packet header identifier for fingerprint module communication.
 * 
 * The header is a fixed 2-byte value used to indicate the start of a fingerprint module packet.
 */
#define FINGERPRINT_PACKET_HEADER 0xEF01

/**
 * @brief Packet ID indicating a command packet.
 * 
 * The packet ID is a 1-byte value that defines the type of packet being sent.
 * - 0x01: Command packet
 * - 0x02: Data packet
 * - 0x03: Acknowledgment packet
 * - 0x04: End of data packet
 */
#define FINGERPRINT_PACKET_ID_CMD 0x01

/**
 * @brief Defines the maximum number of fingerprint response packets in the queue.
 * 
 * This value determines the capacity of `fingerprint_response_queue`, 
 * which stores fingerprint responses for asynchronous processing.
 * 
 * @note A larger value increases buffering but consumes more memory. 
 *       Adjust as needed based on system constraints.
 */
#define QUEUE_SIZE 64

/**
 * @brief GPIO pin for the fingerprint sensor interrupt.
 * 
 * This pin is used to detect the interrupt signal from the fingerprint sensor.
 * When the sensor detects a finger, it triggers an interrupt on this GPIO pin.
 */
#define FINGERPRINT_GPIO_PIN (GPIO_NUM_15) // 15  // GPIO pin for fingerprint sensor interrupt

/**
 * @brief GPIO pin for the fingerprint sensor reset.
 */
// Add VIN control pin definition
#define FINGERPRINT_VIN_PIN GPIO_NUM_9  // D9 for VIN control
#define TEMPLATE_QUEUE_SIZE 10
#define TEMPLATE_MAX_SIZE 2048



/**
 * @enum fingerprint_command_t
 * @brief Enumeration of fingerprint module commands.
 *
 * This enumeration defines the command codes used to communicate with
 * the fingerprint sensor module. Each command corresponds to a specific
 * operation that the module can perform.
 */
typedef enum {
    /** Capture a fingerprint image */
    FINGERPRINT_CMD_GET_IMAGE = 0x01,
    
    /** Generate character file from image buffer */
    FINGERPRINT_CMD_GEN_CHAR = 0x02,
    
    /** Match two fingerprint templates */
    FINGERPRINT_CMD_MATCH = 0x03,
    
    /** Search for a fingerprint in the database */
    FINGERPRINT_CMD_SEARCH = 0x04,
    
    /** Generate a model from two fingerprint templates */
    FINGERPRINT_CMD_REG_MODEL = 0x05,
    
    /** Store fingerprint template in the module's database */
    FINGERPRINT_CMD_STORE_CHAR = 0x06,
    
    /** Delete a fingerprint template from the database */
    FINGERPRINT_CMD_DELETE_CHAR = 0x0C,
    
    /** Empty the fingerprint database */
    FINGERPRINT_CMD_EMPTY_DATABASE = 0x0D,
    
    /** Upload fingerprint image from the module */
    FINGERPRINT_CMD_UPLOAD_IMAGE = 0x0A,
    
    /** Download fingerprint image to the module */
    FINGERPRINT_CMD_DOWNLOAD_IMAGE = 0x0B,
    
    /** Read system parameters of the fingerprint module */
    FINGERPRINT_CMD_READ_SYS_PARA = 0x0F,
    
    /** Set fingerprint module's chip address */
    FINGERPRINT_CMD_SET_CHIP_ADDR = 0x15,
    
    /** Perform a handshake with the fingerprint module */
    FINGERPRINT_CMD_HANDSHAKE = 0x35,
    
    /** Cancel current fingerprint operation */
    FINGERPRINT_CMD_CANCEL = 0x30,
    
    /** Perform automatic fingerprint enrollment */
    FINGERPRINT_CMD_AUTO_ENROLL = 0x31,
    
    /** Automatically identify a fingerprint */
    FINGERPRINT_CMD_AUTO_IDENTIFY = 0x32,
    
    /** Check the fingerprint sensor status */
    FINGERPRINT_CMD_CHECK_SENSOR = 0x36,
    
    /** Factory reset the fingerprint module */
    FINGERPRINT_CMD_FACTORY_RESET = 0x3B,
    
    /** Read information page from the module */
    FINGERPRINT_CMD_READ_INF_PAGE = 0x16,
    
    /** Burn code into the fingerprint module */
    FINGERPRINT_CMD_BURN_CODE = 0x1A,
    
    /** Set password for the fingerprint module */
    FINGERPRINT_CMD_SET_PASSWORD = 0x12,
    
    /** Verify password for the fingerprint module */
    FINGERPRINT_CMD_VERIFY_PASSWORD = 0x13,
    
    /** Retrieve a random code from the fingerprint module */
    FINGERPRINT_CMD_GET_RANDOM_CODE = 0x14,
    
    /** Write data to notepad memory of the fingerprint module */
    FINGERPRINT_CMD_WRITE_NOTEPAD = 0x18,
    
    /** Read data from notepad memory of the fingerprint module */
    FINGERPRINT_CMD_READ_NOTEPAD = 0x19,
    
    /** Control the fingerprint module's LED indicator */
    FINGERPRINT_CMD_CONTROL_LED = 0x3C,
    
    /** Retrieve information about the captured fingerprint image */
    FINGERPRINT_CMD_GET_IMAGE_INFO = 0x3D,
    
    /** Search for a fingerprint instantly */
    FINGERPRINT_CMD_SEARCH_NOW = 0x3E,
    
    /** Get the number of valid fingerprint templates stored */
    FINGERPRINT_CMD_VALID_TEMPLATE_NUM = 0x1D,
    
    /** Put the fingerprint module into sleep mode */
    FINGERPRINT_CMD_SLEEP = 0x33,

    /** Retrieve a random code from the fingerprint module */
    FINGERPRINT_CMD_GETKEYT = 0xE0,

    /** Perform a security search in the fingerprint database */
    FINGERPRINT_CMD_SECURITY_SEARCH = 0xF4,

    /** Lock the fingerprint module to prevent unauthorized access */
    FINGERPRINT_CMD_LOCKEYT = 0xE1,

    /** Retrieve encrypted data from the fingerprint module */
    FINGERPRINT_CMD_GET_CIPHER_TEXT = 0xE2,

    /** Retrieve the serial number of the fingerprint module */
    FINGERPRINT_CMD_GETCHIP_SN = 0x34,

    /** Capture an enrollment image from the fingerprint sensor */
    FINGERPRINT_CMD_GET_ENROLL_IMAGE = 0x29,

    /** Write data to a specific register in the fingerprint module */
    FINGERPRINT_CMD_WRITE_REG = 0x0E,

    /** Read the index table of stored fingerprint templates */
    FINGERPRINT_CMD_READ_INDEX_TABLE = 0x1F,

    /** Upload a fingerprint template from the module buffer */
    FINGERPRINT_CMD_UP_CHAR = 0x08,

    /** Download a fingerprint template to the module buffer */
    FINGERPRINT_CMD_DOWN_CHAR = 0x09,

    /**
     * @brief Load a fingerprint template from flash memory into the module buffer.
     */
    FINGERPRINT_CMD_LOAD_CHAR = 0x07,

} fingerprint_command_t;

/**
 * @struct fingerprint_command_info_t
 * @brief Structure to store information about a sent fingerprint command.
 *
 * This structure holds metadata related to a fingerprint command, including
 * the command type and the timestamp when it was sent. It is useful for 
 * tracking commands in a queue and debugging response mismatches.
 */
typedef struct {
    fingerprint_command_t command;  /**< Command sent to the fingerprint sensor */
    uint32_t timestamp;             /**< Timestamp when the command was sent (in ticks) */
} fingerprint_command_info_t;

/**
 * @struct FingerprintPacket
 * @brief Structure representing a command or response packet for the fingerprint module.
 *
 * This structure is used for both sending commands to and receiving responses from the fingerprint module.
 * It follows the module's communication protocol, containing fields for the header, address, packet type, 
 * length, command/confirmation code, parameters, and checksum.
 *
 * In command packets, the `command` field specifies the requested operation (e.g., 0x01 for image capture).
 * In response packets, the `command` field serves as a confirmation code indicating the success or failure of the request.
 */
typedef struct {
    uint16_t header;      /**< Fixed packet header (0xEF01) indicating the start of a fingerprint packet. */
    uint32_t address;     /**< Address of the fingerprint module (default: 0xFFFFFFFF). */
    uint8_t packet_id;    /**< Packet identifier (e.g., 0x01 for command packets, 0x07 for responses). */
    uint16_t length;      /**< Length of the packet, excluding the header and address. */
    union {
        uint8_t command;  /**< Command ID for requests or confirmation code in responses. */
        uint8_t confirmation;   /**< Confirmation code for the command. */
    } code;
    uint8_t parameters[MAX_PARAMETERS]; /**< Command-specific parameters (variable length, up to MAX_PARAMETERS bytes). */
    uint16_t checksum;    /**< Checksum for packet integrity verification. */
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
  * 
  * This packet structure should be overwritten using fingerprint_set_command()
  * because its parameters (Buffer ID, Start Page, and Number of Pages) are 
  * dynamic and depend on the user's or developer's choice.
  * 
  * ### Packet Structure:
  * | Header  | Device Address | Packet ID | Packet Length | Command | Parameters                                           | Checksum             |
  * |---------|---------------|-----------|---------------|---------|-------------------------------------------------------|----------------------|
  * | 2 bytes | 4 bytes       | 1 byte    | 2 bytes       | 1 byte  | Buffer ID 1 bytes, Start Page 2 bytes, PageNum 2 bytes| 2 bytes              |
  * | 0xEF01  | 0xFFFFFFFF    | 0x01      | 0x0008        | 0x04    | Varies (set dynamically)                              | Computed dynamically |
  */
 extern FingerprintPacket PS_Search;
 
 
 /**
  * @brief Matches two fingerprint templates stored in RAM.
  */
 extern FingerprintPacket PS_Match;
 
 /**
  * @brief Stores a fingerprint template into the module’s database.
  * 
  * This packet structure should be overwritten using fingerprint_set_command()
  * because its parameters (Buffer ID and Page ID) are dynamic and depend on 
  * the user's or developer's choice.
  * 
  * ### Packet Structure:
  * | Header  | Device Address | Packet ID | Packet Length | Command | Parameters                                  | Checksum             |
  * |---------|---------------|-----------|---------------|---------|----------------------------------------------|----------------------|
  * | 2 bytes | 4 bytes       | 1 byte    | 2 bytes       | 1 byte  | Buffer ID 1 byte, Page ID 2 bytes          | 2 bytes              |
  * | 0xEF01  | 0xFFFFFFFF    | 0x01      | 0x0006        | 0x06    | Varies (set dynamically)                    | Computed dynamically |
  */
 extern FingerprintPacket PS_Store;
 
 
 /**
  * @brief Deletes a specific fingerprint template from the database.
  */
 extern FingerprintPacket PS_DeleteChar;
 
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
  * @brief Cancels the current fingerprint operation.
  *
  * This command has no parameters and is used to stop any ongoing process.
  */
 extern FingerprintPacket PS_Cancel;
 
 /**
  * @brief Automatically enrolls a fingerprint template.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameters (ID number, number of entries, and additional settings)
  * are dynamic and depend on the user's choice.
  *
  * ### Parameters:
  * - **ID Number** (1 byte): The fingerprint ID to assign.
  * - **Number of Entries** (1 byte): The number of fingerprint samples.
  * - **Parameter** (3 bytes): Additional settings (sensor-specific).
  */
 extern FingerprintPacket PS_AutoEnroll;
 
 /**
  * @brief Automatically identifies a fingerprint.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameters (score level and ID number) are dynamic.
  *
  * ### Parameters:
  * - **Score Level** (1 byte): The threshold for matching accuracy.
  * - **ID Number** (2 bytes): The ID range to search.
  */
 extern FingerprintPacket PS_Autoldentify;
 
 /**
  * @brief Retrieves the key pair from the fingerprint sensor.
  *
  * This command has no parameters.
  */
 extern FingerprintPacket PS_GetKeyt;
 
 /**
  * @brief Securely stores a fingerprint template.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameters (Buffer ID and Page ID) are dynamic.
  *
  * ### Parameters:
  * - **Buffer ID** (1 byte): Specifies the template buffer (1 or 2).
  * - **Page ID** (2 bytes): The database location to store the template.
  */
 extern FingerprintPacket PS_SecurityStoreChar;
 
 /**
  * @brief Searches for a fingerprint template in secure mode.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameters (Buffer ID, Start Page, Number of Pages) are dynamic.
  *
  * ### Parameters:
  * - **Buffer ID** (1 byte): Specifies which buffer to use.
  * - **Start Page** (2 bytes): The starting page index in the database.
  * - **Number of Pages** (2 bytes): The range of pages to search.
  */
 extern FingerprintPacket PS_SecuritySearch;
 
 /**
  * @brief Uploads the fingerprint image from the sensor.
  *
  * This command has no parameters.
  */
 extern FingerprintPacket PS_Uplmage;
 
 /**
  * @brief Downloads a fingerprint image to the sensor.
  *
  * This command has no parameters.
  */
 extern FingerprintPacket PS_Downlmage;
 
 /**
  * @brief Checks the status of the fingerprint sensor.
  *
  * This command has no parameters.
  */
 extern FingerprintPacket PS_CheckSensor;
 
 /**
  * @brief Restores the fingerprint sensor to factory settings.
  *
  * This command has no parameters.
  */
 extern FingerprintPacket PS_RestSetting;
 
 /**
  * @brief Reads the fingerprint sensor's flash information page.
  *
  * This command has no parameters.
  */
 extern FingerprintPacket PS_ReadINFpage;
 
 /**
  * @brief Erases the fingerprint sensor's firmware.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameter (Upgrade Mode) is dynamic.
  *
  * ### Parameters:
  * - **Upgrade Mode** (1 byte): Mode for firmware upgrade.
  */
 extern FingerprintPacket PS_BurnCode;
 
 /**
  * @brief Sets a password for the fingerprint sensor.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameter (Password) is user-defined.
  *
  * ### Parameters:
  * - **Password** (4 bytes): The new password for sensor access.
  */
 extern FingerprintPacket PS_SetPwd;
 
 /**
  * @brief Verifies the fingerprint sensor password.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameter (Password) is user-defined.
  *
  * ### Parameters:
  * - **Password** (4 bytes): The password to verify.
  */
 extern FingerprintPacket PS_VfyPwd;
 
 /**
  * @brief Requests a random number from the fingerprint sensor.
  *
  * This command has no parameters.
  */
 extern FingerprintPacket PS_GetRandomCode;
 
 /**
  * @brief Reads data from the fingerprint sensor's notepad memory.
  *
  * This packet structure should be overwritten using `fingerprint_set_command()`
  * because its parameter (Page Number) is dynamic.
  *
  * ### Parameters:
  * - **Page Number** (1 byte): Specifies which notepad page to read.
  */
 extern FingerprintPacket PS_ReadNotepad;
 
 /**
 * @brief Downloads a fingerprint template to the module.
 */
extern FingerprintPacket PS_DownChar;

/**
 * @brief Uploads a stored fingerprint template from the module.
 */
extern FingerprintPacket PS_UpChar;

/**
 * @brief Loads a fingerprint template from flash memory into the module's buffer.
 *
 * This command instructs the fingerprint module to retrieve a stored fingerprint 
 * template from the flash database and load it into a specified template buffer.
 *
 * @note The `parameters` field should be set before use:
 *       - `parameters[0]` = Buffer ID (target buffer)
 *       - `parameters[1]` = High byte of Page ID
 *       - `parameters[2]` = Low byte of Page ID
 *
 * @warning The checksum must be recalculated before sending the packet.
 */
extern FingerprintPacket PS_LoadChar;

/**
 * @brief Erases the module firmware and enters upgrade mode.
 */
extern FingerprintPacket PS_BurnCode;

/**
 * @brief Performs a handshake with the fingerprint module.
 *
 * Used to verify communication with the fingerprint module.
 */
extern FingerprintPacket PS_HandShake;

/**
 * @brief Controls the fingerprint sensor's LED light mode.
 *
 * This packet structure should be overwritten using `fingerprint_set_command()`
 * because its parameters (Function, Start Color, End Color, Cycles) are dynamic.
 *
 * ### Parameters:
 * - **Function** (1 byte): Specifies LED function.
 * - **Start Color** (1 byte): Starting color.
 * - **End Color** (1 byte): Ending color.
 * - **Cycles** (1 byte): Number of cycles.
 */
extern FingerprintPacket PS_ControlBLN;

/**
 * @brief Retrieves information about the current fingerprint image.
 *
 * This command requests metadata about the fingerprint image stored in the buffer.
 */
extern FingerprintPacket PS_GetImageInfo;

/**
 * @brief Performs an immediate fingerprint search in the database.
 *
 * This packet structure should be overwritten using `fingerprint_set_command()`
 * because its parameters (Start Page, Number of Pages) are dynamic.
 *
 * ### Parameters:
 * - **Start Page** (2 bytes): Page to start searching from.
 * - **Number of Pages** (2 bytes): Number of pages to search.
 */
extern FingerprintPacket PS_SearchNow;

/**
 * @brief Retrieves the number of valid fingerprint templates stored in the module.
 */
extern FingerprintPacket PS_ValidTempleteNum;

/**
 * @brief Puts the fingerprint module into sleep mode.
 */
extern FingerprintPacket PS_Sleep;

/**
 * @brief Locks the fingerprint module's key pair.
 */
extern FingerprintPacket PS_LockKeyt;

/**
 * @brief Retrieves an encrypted random number from the fingerprint module.
 */
extern FingerprintPacket PS_GetCiphertext;

/**
 * @brief Retrieves the fingerprint module's unique serial number.
 */
extern FingerprintPacket PS_GetChipSN;

/**
 * @brief Captures an image for fingerprint enrollment.
 */
extern FingerprintPacket PS_GetEnrollImage;

/**
 * @brief Writes to the fingerprint module's system register.
 *
 * This packet structure should be overwritten using `fingerprint_set_command()`
 * because its parameters (Register Number, Value) are dynamic.
 *
 * ### Parameters:
 * - **Register Number** (1 byte): Specifies which register to write to.
 * - **Value** (1 byte): Data to be written.
 */
extern FingerprintPacket PS_WriteReg;

/**
 * @brief Reads the fingerprint template index table.
 *
 * This packet structure should be overwritten using `fingerprint_set_command()`
 * because its parameter (Page Number) is dynamic.
 *
 * ### Parameters:
 * - **Page Number** (1 byte): Specifies which index table page to read.
 */
extern FingerprintPacket PS_ReadIndexTable;

/**
 * @brief Upload template from buffer to main control.
 * 
 * This command uploads the fingerprint template stored in the buffer 
 * to the main control unit. The function is supported when the 
 * encryption level is set to 0.
 */
extern FingerprintPacket PS_UpChar;

/**
 * @brief Download template to buffer.
 * 
 * This command allows the master device to download a fingerprint 
 * template to the module’s template buffer. The function is supported 
 * when the encryption level is set to 0.
 */
extern FingerprintPacket PS_DownChar;

/**
 * @brief External declaration of the Read Information Page command.
 *
 * This command is used to read the information page of the fingerprint module,
 * which typically contains device-specific details such as firmware version, 
 * module serial number, and other relevant metadata.
 *
 * @note Ensure that the fingerprint module is properly initialized before 
 * calling this command.
 */
extern FingerprintPacket PS_ReadINFPage;

 
 /**
  * @brief Fingerprint sensor status codes.
  *
  * This enumeration defines various return codes for the fingerprint sensor.
  * Each value corresponds to a specific error or status condition.
  */
 typedef enum {
     /** @brief Instruction execution completed. Value: `0x00` */
     FINGERPRINT_OK = 0x00,                     
 
     /** @brief Data packet reception error. Value: `0x01` */
     FINGERPRINT_PACKET_ERROR = 0x01,            
 
     /** @brief No finger detected. Value: `0x02` */
     FINGERPRINT_NO_FINGER = 0x02,               
 
     /** @brief Failed to enter fingerprint image. Value: `0x03` */
     FINGERPRINT_IMAGE_FAIL = 0x03,              
 
     /** @brief Image too dry/light. Value: `0x04` */
     FINGERPRINT_TOO_DRY = 0x04,                 
 
     /** @brief Image too wet/muddy. Value: `0x05` */
     FINGERPRINT_TOO_WET = 0x05,                 
 
     /** @brief Image too chaotic. Value: `0x06` */
     FINGERPRINT_TOO_CHAOTIC = 0x06,             
 
     /** @brief Normal image, but not enough features. Value: `0x07` */
     FINGERPRINT_TOO_FEW_POINTS = 0x07,         
 
     /** @brief Fingerprint mismatch. Value: `0x08` */
     FINGERPRINT_MISMATCH = 0x08,                
 
     /** @brief No fingerprint found. Value: `0x09` */
     FINGERPRINT_NOT_FOUND = 0x09,               
 
     /** @brief Feature merging failed. Value: `0x0A` */
     FINGERPRINT_MERGE_FAIL = 0x0A,              
 
     /** @brief Address out of range in database. Value: `0x0B` */
     FINGERPRINT_DB_RANGE_ERROR = 0x0B,          
 
     /** @brief Error reading fingerprint template. Value: `0x0C` */
     FINGERPRINT_READ_TEMPLATE_ERROR = 0x0C,     
 
     /** @brief Failed to upload features. Value: `0x0D` */
     FINGERPRINT_UPLOAD_FEATURE_FAIL = 0x0D,     
 
     /** @brief Cannot receive subsequent data packets. Value: `0x0E` */
     FINGERPRINT_DATA_PACKET_ERROR = 0x0E,        
 
     /** @brief Failed to upload image. Value: `0x0F` */
     FINGERPRINT_UPLOAD_IMAGE_FAIL = 0x0F,       
 
     /** @brief Failed to delete template. Value: `0x10` */
     FINGERPRINT_DELETE_TEMPLATE_FAIL = 0x10,    
 
     /** @brief Failed to clear database. Value: `0x11` */
     FINGERPRINT_DB_CLEAR_FAIL = 0x11,           
 
     /** @brief Cannot enter low power mode. Value: `0x12` */
     FINGERPRINT_LOW_POWER_FAIL = 0x12,          
 
     /** @brief Incorrect password. Value: `0x13` */
     FINGERPRINT_WRONG_PASSWORD = 0x13,          
 
     /** @brief No valid original image in buffer. Value: `0x15` */
     FINGERPRINT_NO_VALID_IMAGE = 0x15,          
 
     /** @brief Online upgrade failed. Value: `0x16` */
     FINGERPRINT_UPGRADE_FAIL = 0x16,            
 
     /** @brief Residual fingerprint detected. Value: `0x17` */
     FINGERPRINT_RESIDUAL_FINGER = 0x17,         
 
     /** @brief Flash read/write error. Value: `0x18` */
     FINGERPRINT_FLASH_RW_ERROR = 0x18,          
 
     /** @brief Random number generation failed. Value: `0x19` */
     FINGERPRINT_RANDOM_GEN_FAIL = 0x19,         
 
     /** @brief Invalid register number. Value: `0x1A` */
     FINGERPRINT_INVALID_REGISTER = 0x1A,        
 
     /** @brief Register setting content error. Value: `0x1B` */
     FINGERPRINT_REGISTER_SETTING_ERROR = 0x1B,  
 
     /** @brief Incorrect notepad page number. Value: `0x1C` */
     FINGERPRINT_NOTEPAD_PAGE_ERROR = 0x1C,      
 
     /** @brief Port operation failed. Value: `0x1D` */
     FINGERPRINT_PORT_OP_FAIL = 0x1D,            
 
     /** @brief Automatic registration failed. Value: `0x1E` */
     FINGERPRINT_ENROLL_FAIL = 0x1E,             
 
     /** @brief Fingerprint database full. Value: `0x1F` */
     FINGERPRINT_DB_FULL = 0x1F,                 
 
     /** @brief Device address error. Value: `0x20` */
     FINGERPRINT_DEVICE_ADDRESS_ERROR = 0x20,    
 
     /** @brief Template is not empty. Value: `0x22` */
     FINGERPRINT_TEMPLATE_NOT_EMPTY = 0x22,      
 
     /** @brief Template is empty. Value: `0x23` */
     FINGERPRINT_TEMPLATE_EMPTY = 0x23,          
 
     /** @brief Database is empty. Value: `0x24` */
     FINGERPRINT_DB_EMPTY = 0x24,                
 
     /** @brief Incorrect entry count. Value: `0x25` */
     FINGERPRINT_ENTRY_COUNT_ERROR = 0x25,       
 
     /** @brief Timeout occurred. Value: `0x26` */
     FINGERPRINT_TIMEOUT = 0x26,                 
 
     /** @brief Fingerprint already exists. Value: `0x27` */
     FINGERPRINT_ALREADY_EXISTS = 0x27,          
 
     /** @brief Features are related. Value: `0x28` */
     FINGERPRINT_FEATURES_RELATED = 0x28,        
 
     /** @brief Sensor operation failed. Value: `0x29` */
     FINGERPRINT_SENSOR_OP_FAIL = 0x29,          
 
     /** @brief Module info not empty. Value: `0x2A` */
     FINGERPRINT_MODULE_INFO_NOT_EMPTY = 0x2A,   
 
     /** @brief Module info empty. Value: `0x2B` */
     FINGERPRINT_MODULE_INFO_EMPTY = 0x2B,       
 
     /** @brief OTP operation failed. Value: `0x2C` */
     FINGERPRINT_OTP_FAIL = 0x2C,                
 
     /** @brief Key generation failed. Value: `0x2D` */
     FINGERPRINT_KEY_GEN_FAIL = 0x2D,            
 
     /** @brief Secret key does not exist. Value: `0x2E` */
     FINGERPRINT_KEY_NOT_EXIST = 0x2E,           
 
     /** @brief Security algorithm execution failed. Value: `0x2F` */
     FINGERPRINT_SECURITY_ALGO_FAIL = 0x2F,      
 
     /** @brief Encryption and function mismatch. Value: `0x30` */
     FINGERPRINT_ENCRYPTION_MISMATCH = 0x30,     

     /** @brief Function does not match the required encryption level. Value: `0x31` */
     FINGERPRINT_FUNCTION_ENCRYPTION_MISMATCH = 0x31,

     /** @brief Secret key is locked. Value: `0x32` */
     FINGERPRINT_KEY_LOCKED = 0x32,              
 
     /** @brief Image area too small. Value: `0x33` */
     FINGERPRINT_IMAGE_AREA_SMALL = 0x33,        
 
     /** @brief Image not available. Value: `0x34` */
     FINGERPRINT_IMAGE_NOT_AVAILABLE = 0x34,     
 
     /** @brief Illegal data. Value: `0x35` */
     FINGERPRINT_ILLEGAL_DATA = 0x35             
 
 } fingerprint_status_t;

/**
 * @brief Handles fingerprint status events and triggers corresponding high-level events.
 *
 * This function processes status codes received from the fingerprint module and
 * maps them to predefined high-level fingerprint events. These events help notify
 * the application about the outcome of fingerprint operations.
 *
 * The following status codes and their corresponding events are handled:
 * 
 * - `FINGERPRINT_OK` → `EVENT_FINGER_DETECTED`
 * - `FINGERPRINT_NO_FINGER` → `EVENT_IMAGE_CAPTURED`
 * - `FINGERPRINT_IMAGE_FAIL`, `FINGERPRINT_TOO_DRY`, `FINGERPRINT_TOO_WET`,
 *   `FINGERPRINT_TOO_CHAOTIC`, `FINGERPRINT_UPLOAD_IMAGE_FAIL`, 
 *   `FINGERPRINT_IMAGE_AREA_SMALL`, `FINGERPRINT_IMAGE_NOT_AVAILABLE` → `EVENT_IMAGE_FAIL`
 * - `FINGERPRINT_TOO_FEW_POINTS` → `EVENT_FEATURE_EXTRACT_FAIL`
 * - `FINGERPRINT_MISMATCH`, `FINGERPRINT_NOT_FOUND` → `EVENT_MATCH_FAIL`
 * - `FINGERPRINT_DB_FULL` → `EVENT_DB_FULL`
 * - `FINGERPRINT_TIMEOUT` → `EVENT_ERROR`
 * - Various sensor operation failures → `EVENT_SENSOR_ERROR`
 * - Other unexpected statuses → `EVENT_ERROR`
 *
 * @param status The fingerprint status received from the sensor.
 */
void fingerprint_status_event_handler(fingerprint_status_t status, FingerprintPacket *packet);
 
/**
 * @brief Initializes the fingerprint scanner module.
 *
 * This function sets up the UART interface, configures the necessary pins, 
 * installs the UART driver, and checks for the power-on handshake signal (0x55). 
 * If the handshake is not received, it waits for 200ms before proceeding.
 * It also initializes a FreeRTOS queue for response handling and creates 
 * a background task to continuously read responses.
 *
 * @return 
 *  - ESP_OK if initialization is successful.
 *  - ESP_FAIL if queue creation or task creation fails.
 *  - UART-related errors if UART initialization fails.
 */
esp_err_t fingerprint_init(void);

 
/**
  * @brief Initializes or updates a fingerprint command packet.
  *
  * This function modifies an existing `FingerprintPacket` structure to prepare a command
  * before sending it to the fingerprint sensor. It ensures that the parameter length 
  * does not exceed the allowed size and automatically computes the checksum.
  *
  * @warning This function **modifies** the provided `FingerprintPacket` structure in-place.
  *          Developers should ensure that any previous data in the structure is not needed 
  *          before calling this function, unless they create a **new** `FingerprintPacket` instance.
  *
  * @param[out] cmd Pointer to the `FingerprintPacket` to be modified.
  * @param[in] command Command byte to be set in the packet.
  * @param[in] params Pointer to a parameter array (can be NULL if no parameters).
  * @param[in] param_length Number of parameters (max `MAX_PARAMETERS`).
  * @return 
  *      - `ESP_OK` on success.
  *      - `ESP_ERR_INVALID_ARG` if `cmd` is NULL.
  *      - `ESP_ERR_INVALID_SIZE` if `param_length` exceeds `MAX_PARAMETERS`.
  *
  * @note This function does not allocate new memory. The caller is responsible for managing `cmd`.
  *       If modification of an existing `FingerprintPacket` is not desired, a **new instance**
  *       should be created before calling this function.
  */

 esp_err_t fingerprint_set_command(FingerprintPacket *cmd, uint8_t command, uint8_t *params, uint8_t param_length);
 
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
 * @brief Task to continuously read fingerprint responses from UART.
 * 
 * This FreeRTOS task reads fingerprint sensor responses and enqueues them 
 * into the response queue for further processing. It runs indefinitely, 
 * ensuring that responses are captured asynchronously.
 * 
 * @param pvParameter Unused parameter (NULL by default).
 * 
 * @note The function reads responses using `fingerprint_read_response()`, 
 *       validates them, and sends them to `fingerprint_response_queue`. 
 *       If the queue is full, the response is dropped with a warning log.
 * 
 * @warning This task should be created only once during initialization 
 *          and should not be terminated unexpectedly, as it ensures 
 *          continuous response handling.
 */
void read_response_task(void *pvParameter);

typedef struct {
    FingerprintPacket **packets;  // Array of packet pointers
    size_t count;                 // Number of packets found
    
    // Template collection fields
    bool collecting_template;     // Flag indicating active collection
    bool template_complete;       // Flag indicating template is complete
    uint32_t start_time;          // When collection started (for timeout)
    
    // Raw template data accumulation
    uint8_t *template_data;       // Combined template data buffer
    size_t template_size;         // Current size of accumulated template
    size_t template_capacity;     // Allocated capacity of template_data
} MultiPacketResponse;
 
/**
 * @brief Reads the response packet from UART and returns a dynamically allocated FingerprintPacket.
 *
 * @note The caller is responsible for freeing the allocated memory using `free()`
 *       after processing the response to avoid memory leaks.
 *
 * @return Pointer to the received FingerprintPacket, or NULL on failure.
 */
//  FingerprintPacket* fingerprint_read_response(void);
MultiPacketResponse* fingerprint_read_response(void);

/**
 * @brief Task function to process fingerprint scanner responses.
 *
 * This FreeRTOS task continuously reads responses from the fingerprint scanner,
 * dequeues them from the response queue, and processes them accordingly.
 *
 * @param[in] pvParameter Unused parameter (can be NULL).
 */
void process_response_task(void *pvParameter);

 /**
 * @brief Structure representing a fingerprint module response.
 *
 * This structure stores the status of the fingerprint response and the received packet data.
 * It is used to facilitate communication between tasks via the response queue.
 */
typedef struct {
    fingerprint_status_t status; /**< Status code of the fingerprint response */
    FingerprintPacket packet;    /**< Data packet received from the fingerprint module */
} fingerprint_response_t;

// /** 
//  * @brief Queue handle for storing fingerprint module responses.
//  *
//  * This queue is used to store responses received from the fingerprint module.
//  * Other components can retrieve responses from this queue for processing.
//  */
// extern QueueHandle_t fingerprint_response_queue;

 /**
  * @brief Get the status of the fingerprint operation from the response packet.
  *
  * This function extracts the confirmation code from the fingerprint sensor's response
  * and maps it to a predefined fingerprint_status_t enumeration.
  *
  * @param packet Pointer to the FingerprintPacket struScture containing the response.
  *
  * @return fingerprint_status_t Status code representing the operation result.
  */
 fingerprint_status_t fingerprint_get_status(FingerprintPacket *packet);
 
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
     * @brief Event representing no valid fingerprint event.
     *
     * This event is used as a default value when no other event is applicable.
     */
    EVENT_NONE = -1,                  /**< No event (Default value) */

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
     EVENT_SENSOR_ERROR,              /**< Sensor operation failure (FINGERPRINT_SENSOR_OP_FAIL) */

    /**
     * @brief Event triggered when a fingerprint is enrolled successfully.
     *
     * This event corresponds to the status FINGERPRINT_ENROLL_SUCCESS.
     */
    EVENT_ENROLL_SUCCESS,               /**< Fingerprint enrolled successfully (FINGERPRINT_ENROLL_SUCCESS) */

    /**
     * @brief Event triggered when fingerprint enrollment fails.
     *
     * This event corresponds to the status FINGERPRINT_ENROLL_FAIL.
     */
    EVENT_ENROLL_FAIL,                  /**< Fingerprint enrollment failed (FINGERPRINT_ENROLL_FAIL) */

    /**
     * @brief Event triggered when a fingerprint template is stored.
     *
     * This event corresponds to the status FINGERPRINT_TEMPLATE_STORED.
     */
    EVENT_TEMPLATE_STORED,              /**< Fingerprint template stored (FINGERPRINT_TEMPLATE_STORED) */

    /**
     * @brief Event triggered when a fingerprint template is deleted.
     *
     * This event corresponds to the status FINGERPRINT_TEMPLATE_DELETED.
     */
    EVENT_TEMPLATE_DELETED,             /**< Fingerprint template deleted (FINGERPRINT_TEMPLATE_DELETED) */

    /**
     * @brief Event triggered when a fingerprint template is deleted.
     *
     * This event corresponds to the status FINGERPRINT_TEMPLATE_DELETE_FAIL.
     */
    EVENT_TEMPLATE_DELETE_FAIL,         /**< Fingerprint template deletion failed (FINGERPRINT_DELETE_TEMPLATE_FAIL) */

    /**
     * @brief Event triggered when the system enters low power mode.
     *  
     * This event corresponds to the status FINGERPRINT_LOW_POWER_MODE.
     */
    EVENT_LOW_POWER_MODE,               /**< Entered low power mode (FINGERPRINT_LOW_POWER_MODE) */

    /**
     * @brief Event triggered when an operation times out.
     *
     * This event corresponds to the status FINGERPRINT_TIMEOUT.
     */
    EVENT_TIMEOUT,                   /**< Operation timed out (FINGERPRINT_TIMEOUT) */

    /**
     * @brief Event triggered when no finger is detected on the sensor.
     *
     * This event corresponds to the status FINGERPRINT_NO_FINGER_DETECTED.
     */
    EVENT_NO_FINGER_DETECTED,           /**< No finger detected (FINGERPRINT_NO_FINGER_DETECTED) */

    /**
     * @brief Event triggered when the fingerprint scanner is ready for operation.
     *
     * This event indicates that the fingerprint module has completed initialization
     * and is ready to process fingerprint-related commands.
     */
    EVENT_SCANNER_READY,                 /**< Scanner is ready for operation */

    /**
     * @brief Event triggered when two fingerprint templates are successfully merged.
     *
     * This event corresponds to the status FINGERPRINT_TEMPLATE_MERGED.
     */
    EVENT_TEMPLATE_MERGED,              /**< Fingerprint templates merged successfully */

    /**
     * @brief Event triggered when a fingerprint template is successfully stored in flash memory.
     *
     * This event corresponds to the confirmation code 0x00 (successful storage).
     */
    EVENT_TEMPLATE_STORE_SUCCESS, /**< Fingerprint template successfully stored */

    /**
     * @brief Event triggered when an error occurs in receiving the data packet.
     *
     * This event corresponds to the confirmation code 0x01 (packet reception error).
     */
    EVENT_TEMPLATE_STORE_PACKET_ERROR, /**< Error in receiving the fingerprint storage packet */

    /**
     * @brief Event triggered when the PageID for storing the fingerprint template is out of range.
     *
     * This event corresponds to the confirmation code 0x0B (PageID out of range).
     */
    EVENT_TEMPLATE_STORE_OUT_OF_RANGE, /**< PageID is out of range */

    /**
     * @brief Event triggered when an error occurs while writing the fingerprint template to FLASH memory.
     *
     * This event corresponds to the confirmation code 0x18 (FLASH write error).
     */
    EVENT_TEMPLATE_STORE_FLASH_ERROR, /**< Error writing to FLASH memory */

    /**
     * @brief Event triggered when the function does not match the encryption level.
     *
     * This event corresponds to the confirmation code 0x31 (encryption level mismatch).
     */
    EVENT_TEMPLATE_STORE_ENCRYPTION_MISMATCH, /**< Function does not match encryption level */

    /**
     * @brief Event triggered when illegal data is detected during fingerprint template storage.
     *
     * This event corresponds to the confirmation code 0x35 (illegal data).
     */
    EVENT_TEMPLATE_STORE_ILLEGAL_DATA, /**< Illegal data detected during storage */

    /**
     * @brief Event triggered when a fingerprint search operation is successful.
     *
     * This event is dispatched when the PS_Search command successfully finds
     * a matching fingerprint template in the database. It indicates that 
     * the scanned fingerprint corresponds to an existing stored template.
     *
     * @note This event will not be triggered if the search fails or no match is found.
     */
    EVENT_SEARCH_SUCCESS,  /**< Event code for successful fingerprint search */

    /**
     * @brief Event triggered when the template count is updated.
     *
     * This event corresponds to the status `FINGERPRINT_TEMPLATE_COUNT`.
     */
    EVENT_TEMPLATE_COUNT, /**< Template count updated (FINGERPRINT_TEMPLATE_COUNT) */

    /**
     * @brief Event triggered when an index table read operation is completed.
     *
     * This event corresponds to the status `FINGERPRINT_INDEX_TABLE_READ`.
     */
    EVENT_INDEX_TABLE_READ, /**< Index table read completed (FINGERPRINT_INDEX_TABLE_READ) */

    /**
     * @brief Event triggered when a fingerprint model is successfully created.
     *
     * This event corresponds to the status `FINGERPRINT_MODEL_CREATED`.
     */
    EVENT_MODEL_CREATED, /**< Fingerprint model successfully created (FINGERPRINT_MODEL_CREATED) */

    /**
     * @brief Event triggered when a fingerprint template is successfully uploaded from the module.
     *
     * This occurs when a stored fingerprint template is transferred to the host.
     */
    EVENT_TEMPLATE_UPLOADED,  /**< Template uploaded from module */

    /**
     * @brief Event triggered when a fingerprint template is successfully downloaded to the module.
     *
     * This occurs when a fingerprint template is written into the fingerprint module’s memory.
     */
    EVENT_TEMPLATE_DOWNLOADED,  /**< Template downloaded to module */

    /**
     * @brief Event triggered when the fingerprint database is cleared.
     *
     * This confirms that all fingerprint templates have been deleted.
     */
    EVENT_DB_CLEARED,  /**< Database emptied successfully */

    /**
     * @brief Event triggered when system parameters are successfully read.
     *
     * This event indicates that the fingerprint module's system parameters, 
     * such as device address, security level, and baud rate, have been retrieved successfully.
     */
    EVENT_SYS_PARAMS_READ,  /**< System parameters read successfully */

    /**
     * @brief Event triggered when a fingerprint template exists in the buffer.
     *
     * This event indicates that the fingerprint module has successfully loaded 
     * a fingerprint template into the specified template buffer. It confirms 
     * that the buffer contains valid fingerprint data, which can be used for 
     * further operations such as matching or storage.
     */
    EVENT_TEMPLATE_EXISTS,  /**< Fingerprint template successfully loaded into buffer */

    /**
     * @brief Event triggered when uploading a fingerprint feature/template fails.
     *
     * This event indicates that the fingerprint module encountered an issue 
     * while attempting to upload a fingerprint template to the host system. 
     * Possible causes may include communication errors, buffer issues, 
     * or an invalid template format.
     */
    EVENT_TEMPLATE_UPLOAD_FAIL,  /**< Fingerprint template upload failed */

    /**
     * @brief Event triggered when the information page is successfully read.
     *
     * This event is generated after executing the Read Information Page command 
     * and receiving a valid response from the fingerprint module. It indicates 
     * that the module's details, such as firmware version and serial number, 
     * have been retrieved.
     */
    EVENT_INFO_PAGE_READ,

    /**
     * @brief Event triggered when a fingerprint template is successfully loaded from flash memory into the buffer.
     *
     * This event occurs after executing the `PS_LoadChar` command, indicating that the fingerprint template 
     * has been retrieved from the module’s internal storage and is now available for further operations such as 
     * matching, uploading, or storing in another location.
     *
     * @note Ensure that the `PS_LoadChar` command completes successfully before relying on this event.
     *       If the template is corrupt or missing, subsequent operations like `PS_UpChar` may return zeroed data.
     *
     * @see PS_LoadChar
     * @see EVENT_TEMPLATE_UPLOADED
     * @see EVENT_TEMPLATE_MATCHED
     */
    EVENT_TEMPLATE_LOADED,

    EVENT_PACKET_RECEPTION_FAIL, /**< Failed to receive subsequent data packets */

    EVENT_ENROLLMENT_COMPLETE,   /**< Fingerprint enrollment process completed */

    EVENT_ENROLLMENT_FAIL,       /**< Fingerprint enrollment process failed */

 } fingerprint_event_type_t;

 /**
 * @struct fingerprint_sys_params_t
 * @brief Structure containing system parameters from the fingerprint module.
 *
 * This structure stores various system-level parameters retrieved from the 
 * fingerprint module, including device configuration, security settings, 
 * and communication parameters.
 */
typedef struct {
    /**
     * @brief Status register of the fingerprint module.
     *
     * This register contains the current operational status and any error codes
     * related to the fingerprint module.
     */
    uint16_t status_register;

    /**
     * @brief Template size for fingerprint data.
     * 
     * Specifies the size of a single fingerprint template in bytes.
     */
    uint16_t template_size;

    /**
     * @brief Number of fingerprints that can be stored in the library.
     *
     * Represents the maximum capacity of stored fingerprint templates
     * in the module's internal storage.
     */
    uint16_t database_size;

    /**
     * @brief Security level setting of the fingerprint module.
     *
     * Determines the strictness of fingerprint matching. Higher values
     * indicate stricter matching conditions.
     */
    uint16_t security_level;

    /**
     * @brief Device address used for module communication.
     *
     * The fingerprint module communicates using a unique 32-bit address.
     * The default value is `0xFFFFFFFF` (broadcast mode).
     */
    uint32_t device_address;

    /**
     * @brief Data packet size for communication.
     *
     * Specifies the length of data packets exchanged between the fingerprint
     * module and the microcontroller. Common values: 32, 64, 128, or 256 bytes.
     */
    uint16_t data_packet_size;

    /**
     * @brief Baud rate setting for UART communication.
     *
     * Determines the communication speed between the fingerprint module
     * and the microcontroller. Typically set in multiples of 9600 baud.
     */
    uint16_t baud_rate;


} fingerprint_sys_params_t;


/**
 * @struct fingerprint_match_info_t
 * @brief Stores detailed match information when a fingerprint match is successful.
 *
 * This structure holds the match result details, including the raw page ID 
 * from the fingerprint sensor, the converted template ID, and the match score.
 */
typedef struct {
    uint16_t page_id;     /**< Raw page ID returned by the fingerprint module */
    uint16_t template_id; /**< Converted template ID for application use */
    uint16_t match_score; /**< Confidence score of the match */
} fingerprint_match_info_t;

/**
 * @struct fingerprint_template_count_t
 * @brief Stores the count of enrolled fingerprints in the module.
 *
 * This structure is used when retrieving the number of stored fingerprint templates.
 */
typedef struct {
    uint16_t count; /**< Number of enrolled fingerprints */
} fingerprint_template_count_t;

// In fingerprint.h - Add this structure
typedef struct {
    uint8_t* data;    // Pointer to complete template data
    size_t size;      // Size of template data in bytes
    bool is_complete; // Flag indicating if template is complete
} fingerprint_template_buffer_t;

/**
 * @struct fingerprint_enrollment_info_t
 * @brief Stores information about a completed fingerprint enrollment.
 *
 * This structure contains details about a fingerprint enrollment operation,
 * including the template ID and storage location of the enrolled fingerprint.
 */
typedef struct {
    uint16_t template_id;    /**< The ID assigned to the newly enrolled fingerprint template */
    bool is_duplicate;       /**< Flag indicating if this fingerprint matches an existing one */
    uint8_t attempts;        /**< Number of attempts made during the enrollment process */
} fingerprint_enrollment_info_t;

/**
 * @struct fingerprint_event_t
 * @brief Defines a generic fingerprint event structure with flexible response types.
 *
 * This structure represents an event triggered by the fingerprint module. It 
 * includes the event type, response status, and a flexible data union that can 
 * store different types of structured response data.
 */
typedef struct {
    fingerprint_event_type_t type;  /**< The type of fingerprint event */
    fingerprint_status_t status;    /**< Status code returned from the fingerprint module */
    FingerprintPacket packet;       /**< Raw response packet (for backward compatibility) */
    MultiPacketResponse *multi_packet; /**< Multi-packet response data */
    uint8_t command;                /**< Command byte associated with the event */
    /**
     * @union data
     * @brief Stores structured response data depending on the event type.
     *
     * This union allows different event types to carry relevant data, improving 
     * readability and reducing the need for manual parsing in the main application.
     */
    union {
        fingerprint_match_info_t match_info;   /**< Data for fingerprint match events */
        fingerprint_template_count_t template_count; /**< Data for template count events */
        fingerprint_sys_params_t sys_params;     // Added system parameters
        fingerprint_template_buffer_t template_data; /**< Fingerprint template data */
        fingerprint_enrollment_info_t enrollment_info; /**< Enrollment information */
        // Extend with additional structured types as needed
    } data;
} fingerprint_event_t;

 
 /**
  * @brief Typedef for the fingerprint event handler callback.
  *
  * This type is used to define a function pointer for handling fingerprint-related events.
  * The function pointed to by the callback should take a single parameter of type `fingerprint_event_type_t`,
  * representing the specific event that occurred.
  *
  * @param event The fingerprint event that occurred. This is an enumeration value of type `fingerprint_event_type_t`.
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
 * @brief Triggers a fingerprint event and processes it.
 * 
 * This function initializes a fingerprint event structure with the given 
 * event type and status, then passes it to the event handler.
 * 
 * @param event_type The high-level event type to trigger.
 * @param status The original fingerprint status that caused the event.
 */
void trigger_fingerprint_event(fingerprint_event_t event);

/**
 * @brief Performs manual fingerprint enrollment at a specified storage location.
 *
 * This function executes a step-by-step fingerprint enrollment process, handling
 * user interaction and ensuring proper fingerprint capture and storage at the given location.
 * 
 * ## Enrollment Process:
 * 1. Wait for a finger to be placed on the scanner (`PS_GetImage`).
 * 2. If a fingerprint is detected, generate the first fingerprint template (`PS_GenChar1`).
 * 3. Prompt the user to remove their finger and wait for confirmation of removal.
 * 4. Once the finger is removed, wait for the same finger to be placed again.
 * 5. Capture the fingerprint again (`PS_GetImage`) and generate the second template (`PS_GenChar2`).
 * 6. Merge both templates into a single fingerprint model (`PS_RegModel`).
 * 7. Store the fingerprint template in the fingerprint module’s database at the specified `location` (`PS_StoreChar`).
 *
 * ## Error Handling:
 * - If a finger is not detected within the allowed retries, the function will return an error.
 * - If the finger is not removed within a timeout period, the process will restart.
 * - If enrollment fails at any step, it retries up to three times before failing.
 *
 * @note This function does not run as a FreeRTOS task but can be called from one.
 *       It dynamically creates an event group for synchronization and deletes it upon completion.
 *
 * @param[in] location  The storage location (ID) where the fingerprint template will be saved.
 *
 * @return
 * - `ESP_OK` if fingerprint enrollment is successful.
 * - `ESP_FAIL` if enrollment fails after the maximum number of attempts.
 * - `ESP_ERR_NO_MEM` if memory allocation for the event group fails.
 */
esp_err_t enroll_fingerprint(uint16_t location);


/**
 * @brief Event bit indicating a successful fingerprint enrollment.
 * 
 * This bit is set in the `enroll_event_group` when a fingerprint enrollment 
 * operation completes successfully.
 */
#define ENROLL_BIT_SUCCESS BIT0

/**
 * @brief Event bit indicating a failed fingerprint enrollment.
 * 
 * This bit is set in the `enroll_event_group` when a fingerprint enrollment 
 * operation fails due to issues such as poor fingerprint quality or sensor errors.
 */
#define ENROLL_BIT_FAIL BIT1

/**
 * @brief Event bit flag for checking if a fingerprint template location is available.
 * 
 * This bit is set in the event group to indicate that the system is currently verifying 
 * whether a specified fingerprint template location is occupied.
 * 
 * @note Used in conjunction with `enroll_event_group` to synchronize location checking.
 */
#define CHECKING_LOCATION_BIT BIT2

#define INFO_PAGE_COMPLETE_BIT BIT3  // You may need to adjust the bit position

#define DUPLICATE_FOUND_BIT BIT4 

/**
 * @brief Event group handle for fingerprint enrollment status.
 * 
 * This FreeRTOS event group is used to synchronize fingerprint enrollment 
 * operations by setting event bits based on success or failure.
 */
static EventGroupHandle_t enroll_event_group = NULL;

/**
 * @brief Verifies a fingerprint by capturing an image, extracting features, and searching in the database.
 *
 * This function attempts to verify a fingerprint up to a maximum number of attempts.
 * It captures a fingerprint image, processes it, and searches for a match in the database.
 * If a match is found, the function returns ESP_OK; otherwise, it returns ESP_FAIL after all attempts.
 *
 * @return 
 *      - ESP_OK if the fingerprint matches an entry in the database.
 *      - ESP_FAIL if verification fails after the maximum attempts.
 *      - ESP_ERR_NO_MEM if the event group could not be created.
 */
esp_err_t verify_fingerprint(void);

/**
 * @brief Deletes a fingerprint template from the specified storage location.
 *
 * This function sends a command to delete a fingerprint stored at the given
 * location in the fingerprint module. It waits for a response event to confirm success.
 *
 * @param[in] location The storage location of the fingerprint template (valid range depends on module).
 * @return
 *      - ESP_OK on successful deletion.
 *      - ESP_ERR_NO_MEM if event group creation fails.
 *      - ESP_FAIL if deletion is unsuccessful.
 */
esp_err_t delete_fingerprint(uint16_t location);

/**
 * @brief Clears the entire fingerprint database.
 *
 * This function sends a command to erase all stored fingerprints in the fingerprint module.
 * It waits for a response event to confirm success.
 *
 * @return
 *      - ESP_OK on successful database clearance.
 *      - ESP_ERR_NO_MEM if event group creation fails.
 *      - ESP_FAIL if clearance fails.
 */
esp_err_t clear_database(void);

/**
 * @brief Checks if the scanned fingerprint already exists in the database.
 *
 * This function sends a search command to the fingerprint module to compare 
 * the scanned fingerprint against stored templates within a predefined range.
 *
 * @return 
 *      - ESP_OK if the search command is sent successfully.
 *      - ESP_FAIL if there is an error sending the search command.
 */
esp_err_t check_duplicate_fingerprint(void);

/**
 * @brief Validates whether a given template storage location is within range.
 *
 * This function sends a command to the fingerprint module to check if the specified 
 * template location exists in the storage index table.
 *
 * @param[in] location The template storage location to validate.
 *
 * @return 
 *      - ESP_OK if the location is valid.
 *      - ESP_FAIL if the index table read operation fails.
 */
esp_err_t validate_template_location(uint16_t location);


/**
 * @brief Get the number of enrolled fingerprints.
 *
 * This function retrieves the count of fingerprints that have been enrolled in the fingerprint scanner.
 *
 * @param[out] count Pointer to a variable where the count of enrolled fingerprints will be stored.
 *
 * @return
 *     - ESP_OK: Successfully retrieved the count.
 *     - ESP_ERR_INVALID_ARG: The provided argument is invalid.
 *     - ESP_FAIL: Failed to retrieve the count due to other reasons.
 */
esp_err_t get_enrolled_count(void);

/**
 * @brief Indicates whether an enrollment process is currently in progress.
 * 
 * This external boolean variable is used to track the state of the fingerprint
 * enrollment process. When `true`, it signifies that the enrollment process
 * is ongoing. When `false`, it indicates that no enrollment is currently
 * taking place.
 */
extern bool enrollment_in_progress;

/**
 * @brief Converts fingerprint module page ID to a sequential page index
 * 
 * Maps page IDs to their actual indices:
 * - Page ID 0   -> Index 0
 * - Page ID 256 -> Index 1
 * - Page ID 512 -> Index 2
 * - Page ID 768 -> Index 3
 * 
 * @param page_id The raw page ID from the fingerprint module
 * @return uint16_t The sequential page index
 */
uint16_t convert_page_id_to_index(uint16_t page_id);

/**
 * @brief Converts a sequential page index back to the fingerprint module page ID
 * 
 * Maps indices back to their page IDs:
 * - Index 0 -> Page ID 0
 * - Index 1 -> Page ID 256
 * - Index 2 -> Page ID 512
 * - Index 3 -> Page ID 768
 * 
 * @param index The sequential page index
 * @return uint16_t The corresponding page ID
 */
uint16_t convert_index_to_page_id(uint16_t index);

/**
 * @brief Reads system parameters from the fingerprint module.
 */
esp_err_t read_system_parameters(void);

/**
 * @brief Loads a fingerprint template from flash storage into the module's template buffer.
 *
 * This function reads a stored fingerprint template from the flash database and loads 
 * it into the specified template buffer for further processing (e.g., matching, modification).
 *
 * @param template_id The unique ID of the fingerprint template in flash.
 * @param buffer_id The buffer ID where the template should be loaded.
 * @return ESP_OK on success, ESP_FAIL on failure.
 */
esp_err_t load_template_to_buffer(uint16_t template_id, uint8_t buffer_id);

/**
 * @brief Uploads a fingerprint template from the module's buffer to the host system.
 *
 * This function transfers a fingerprint template stored in the module's buffer to the host
 * (e.g., microcontroller or computer) for backup, analysis, or transmission.
 *
 * @param buffer_id The buffer ID containing the fingerprint template.
 * @param template_data Pointer to the buffer where the uploaded template will be stored.
 * @param template_size Pointer to a variable that will store the size of the template.
 * @return ESP_OK on success, ESP_FAIL on failure.
 */
esp_err_t upload_template(uint8_t buffer_id, uint8_t* template_data, size_t* template_size);

/**
 * @brief Stores a fingerprint template from the buffer into the module's flash memory.
 *
 * This function saves a fingerprint template currently in the module's buffer 
 * into the permanent flash database for later use.
 *
 * @param buffer_id The buffer ID containing the fingerprint template.
 * @param template_id The unique ID under which the template should be stored.
 * @return ESP_OK on success, ESP_FAIL on failure.
 */
esp_err_t store_template(uint8_t buffer_id, uint16_t template_id);

/**
 * @brief Backs up a fingerprint template by reading it from flash and storing it in a host system.
 *
 * This function retrieves a fingerprint template from the module's flash storage,
 * uploads it to the host, and allows it to be stored elsewhere (e.g., external memory).
 *
 * @param template_id The ID of the fingerprint template to be backed up.
 * @return ESP_OK on success, ESP_FAIL on failure.
 */
esp_err_t backup_template(uint16_t template_id);

/**
 * @brief Initializes the enrollment event group if it doesn't exist and clears any pending event bits.
 *
 * This function ensures that the `enroll_event_group` is properly initialized before use.
 * If the event group does not exist, it will be created. Additionally, it clears
 * `ENROLL_BIT_SUCCESS` and `ENROLL_BIT_FAIL` to remove any previous events.
 *
 * @return ESP_OK if successful, ESP_ERR_NO_MEM if event group creation fails.
 */
esp_err_t initialize_event_group(void);

/**
 * @brief Cleans up the enrollment event group.
 *
 * Deletes the event group if it exists and sets it to NULL.
 * Ensures that no invalid memory is accessed after deletion.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if the event group was already NULL.
 */
esp_err_t cleanup_event_group(void);

/**
 * @brief Reads the information page from the fingerprint module.
 *
 * This function sends the Read Information Page command to the fingerprint module 
 * and retrieves details such as firmware version, serial number, and other metadata.
 *
 * @return 
 *      - `ESP_OK` if the information page is successfully read.
 *      - `ESP_ERR_INVALID_RESPONSE` if an unexpected response is received.
 *      - `ESP_FAIL` if communication with the module fails.
 */
esp_err_t read_info_page(void);

typedef enum {
    WAIT_HEADER,       // Waiting for 0xEF01 header
    READ_ADDRESS,      // Reading 4-byte address
    READ_PACKET_ID,    // Reading packet type
    READ_LENGTH,       // Reading 2-byte length
    READ_CONTENT,      // Reading packet content
    READ_CHECKSUM      // Reading 2-byte checksum
} ParserState;

#define TEMPLATE_UPLOAD_COMPLETE_BIT (1 << 3) // Bit 3 for template upload complete

FingerprintPacket* extract_packet_from_raw_data(uint8_t* data, size_t data_len, uint8_t target_packet_id);

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
esp_err_t restore_template_from_multipacket(uint16_t template_id, MultiPacketResponse *response);

/**
 * @brief Task that waits for finger detection interrupts and processes them.
 * 
 * This task waits for signals from the finger detection interrupt and
 * initiates the fingerprint capture and processing when a finger is detected.
 * 
 * @param pvParameter Unused parameter (NULL by default).
 */
void finger_detection_task(void *pvParameter);

/**
 * @brief Fingerprint operation modes for the finger detection task
 */
typedef enum {
    FINGER_OP_NONE = 0,       /*!< No specific operation, default search behavior */
    FINGER_OP_VERIFY,         /*!< Verification mode - search database for match */
    FINGER_OP_ENROLL_FIRST,   /*!< First step of enrollment - capture first image */
    FINGER_OP_ENROLL_SECOND,  /*!< Second step of enrollment - capture second image */
    FINGER_OP_CUSTOM          /*!< Custom operation mode - caller handles next steps */
} finger_operation_mode_t;

/**
 * @brief Set the operation mode for the finger detection task
 * 
 * This function configures how the finger detection task will process
 * the next detected fingerprint.
 * 
 * @param mode The operation mode to set
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t fingerprint_set_operation_mode(finger_operation_mode_t mode);

/**
 * @brief Get the current operation mode of the finger detection task
 * 
 * @return The current operation mode
 */
finger_operation_mode_t fingerprint_get_operation_mode(void);

/**
 * @brief Wait for a finger to be detected and processed according to the current operation mode
 * 
 * This function blocks until a finger is detected and processed, or until the timeout expires.
 * 
 * @param timeout_ms Timeout in milliseconds, or 0 for no timeout
 * @return ESP_OK if finger was detected and processed successfully,
 *         ESP_ERR_TIMEOUT if the timeout expired,
 *         or another error code on failure
 */
esp_err_t fingerprint_wait_for_finger(uint32_t timeout_ms);

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
esp_err_t fingerprint_check_template_exists(uint16_t template_id);



esp_err_t fingerprint_power_control(bool power_on);


typedef struct {
    uint8_t *data;
    size_t size;
    bool is_final;
} template_data_chunk_t;




 #ifdef __cplusplus
 }
 #endif
 
 #endif // FINGERPRINT_H