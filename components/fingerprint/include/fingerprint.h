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
 *             break;
 *         case EVENT_MATCH_FAIL:
 *             ESP_LOGI("Fingerprint", "Fingerprint mismatch. Status: 0x%02X", event.status);
 *             break;
 *         case EVENT_ERROR:
 *             ESP_LOGI("Fingerprint", "An error occurred during fingerprint processing. Status: 0x%02X", event.status);
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
 * @brief Maximum number of parameters supported in a fingerprint packet.
 *
 * Defines the fixed-size buffer for command-specific parameters in fingerprint communication.
 * This value should be adjusted based on the largest expected parameter size to ensure
 * sufficient space while maintaining efficient memory usage.
 */
#define MAX_PARAMETERS 32  /**< Adjust based on the largest required parameter size. */
 
/**
 * @struct FingerprintPacket
 * @brief Structure representing a command or response packet for the fingerprint module.
 *
 * This structure is used to send commands to and receive responses from the fingerprint module.
 * It follows the module's communication protocol, containing fields for the header, address,
 * packet type, length, command, parameters, and checksum.
 */
typedef struct {
    uint16_t header;      /**< Fixed packet header (0xEF01) indicating the start of a fingerprint packet. */
    uint32_t address;     /**< Address of the fingerprint module (default: 0xFFFFFFFF). */
    uint8_t packet_id;    /**< Packet identifier (e.g., 0x01 for command packets, 0x07 for responses). */
    uint16_t length;      /**< Length of the packet, excluding the header and address. */
    uint8_t command;      /**< Command ID specifying the requested operation (e.g., 0x01 for image capture). */
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
 * @brief Reads the flash information page of the fingerprint module.
 */
extern FingerprintPacket PS_ReadINFpage;

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
void fingerprint_status_event_handler(fingerprint_status_t status);
 
 /**
  * @brief Initializes the fingerprint scanner.
  *
  * @return ESP_OK on success, error code otherwise.
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
 * @brief Reads the response packet from UART and returns a dynamically allocated FingerprintPacket.
 *
 * @note The caller is responsible for freeing the allocated memory using `free()`
 *       after processing the response to avoid memory leaks.
 *
 * @return Pointer to the received FingerprintPacket, or NULL on failure.
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
 } fingerprint_event_type_t;

 /**
 * @brief Structure representing a fingerprint event.
 * 
 * This structure contains the high-level event type and the original
 * fingerprint status that triggered the event.
 */
typedef struct {
    fingerprint_event_type_t type;  /**< The high-level fingerprint event type. */
    fingerprint_status_t status;    /**< The original fingerprint status code. */
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
void trigger_fingerprint_event(fingerprint_event_type_t event_type, fingerprint_status_t status);
 
 #ifdef __cplusplus
 }
 #endif
 
 #endif // FINGERPRINT_H