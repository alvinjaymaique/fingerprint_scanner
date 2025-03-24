#ifndef FINGERPRINT_TEMPLATE_MANAGER_H
#define FINGERPRINT_TEMPLATE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    TEMPLATE_STATE_IDLE,
    TEMPLATE_STATE_COLLECTING,
    TEMPLATE_STATE_COMPLETE,
    TEMPLATE_STATE_ERROR
} template_state_t;

typedef struct {
    uint8_t* data;             // Pointer to template data buffer
    size_t size;               // Current size of template data
    size_t capacity;           // Total capacity of the buffer
    template_state_t state;    // Current state of template collection
    uint32_t start_time;       // When collection started (for timeout)
    bool found_terminator;     // Whether a terminator (FOOF or 0x08) was found
} fingerprint_template_t;

// Initialize the template manager
esp_err_t template_manager_init(void);

// Start template collection process
esp_err_t template_manager_start_collection(void);

// Process a data packet for template collection
esp_err_t template_manager_process_packet(const uint8_t* data, size_t length, uint8_t packet_id);

// Reset template manager (clear data and go to IDLE state)
void template_manager_reset(void);

// Get current template state
template_state_t template_manager_get_state(void);

// Get completed template data (NULL if not complete)
const fingerprint_template_t* template_manager_get_template(void);

// Clean up and free resources
void template_manager_cleanup(void);

#endif /* FINGERPRINT_TEMPLATE_MANAGER_H */