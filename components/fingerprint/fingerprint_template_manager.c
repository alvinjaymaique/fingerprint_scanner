#include "fingerprint_template_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>

#define TAG "TEMPLATE_MGR"
#define TEMPLATE_INITIAL_CAPACITY 2048
#define TEMPLATE_TIMEOUT_MS 5000

static fingerprint_template_t template_data = {0};
static SemaphoreHandle_t template_mutex = NULL;

// Find "FOOF" marker in a data buffer
static bool find_foof_marker(const uint8_t* data, size_t length, size_t* position) {
    for (size_t i = 0; i < length - 3; i++) {
        if (data[i] == 'F' && data[i+1] == 'O' && 
            data[i+2] == 'O' && data[i+3] == 'F') {
            if (position) *position = i;
            return true;
        }
    }
    return false;
}

esp_err_t template_manager_init(void) {
    if (template_mutex == NULL) {
        template_mutex = xSemaphoreCreateMutex();
        if (template_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create template mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    template_manager_reset();
    return ESP_OK;
}

esp_err_t template_manager_start_collection(void) {
    if (xSemaphoreTake(template_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire template mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    // Clean up any previous data
    if (template_data.data) {
        free(template_data.data);
    }
    
    // Allocate initial buffer
    template_data.data = heap_caps_malloc(TEMPLATE_INITIAL_CAPACITY, MALLOC_CAP_8BIT);
    if (template_data.data == NULL) {
        xSemaphoreGive(template_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    template_data.capacity = TEMPLATE_INITIAL_CAPACITY;
    template_data.size = 0;
    template_data.state = TEMPLATE_STATE_COLLECTING;
    template_data.start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    template_data.found_terminator = false;
    
    ESP_LOGI(TAG, "Template collection started");
    xSemaphoreGive(template_mutex);
    return ESP_OK;
}

esp_err_t template_manager_process_packet(const uint8_t* data, size_t length, uint8_t packet_id) {
    if (data == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(template_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire template mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    // Only process if in collecting state
    if (template_data.state != TEMPLATE_STATE_COLLECTING) {
        xSemaphoreGive(template_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check for final packet
    if (packet_id == 0x08) {
        ESP_LOGI(TAG, "Received final template packet (ID=0x08)");
        template_data.found_terminator = true;
        template_data.state = TEMPLATE_STATE_COMPLETE;
        xSemaphoreGive(template_mutex);
        return ESP_OK;
    }
    
    // CRITICAL FIX: Add maximum length check to prevent ridiculous allocation attempts
    if (length > 4096) {
        ESP_LOGW(TAG, "Limiting excessive packet length from %d to 4096 bytes", length);
        length = 4096;
    }
    
    // Check for "FOOF" marker in this data
    size_t foof_position = 0;
    if (find_foof_marker(data, length, &foof_position)) {
        ESP_LOGI(TAG, "FOOF marker found at position %d", foof_position);
        
        // Only copy data up to and including FOOF
        length = foof_position + 4;
        template_data.found_terminator = true;
        template_data.state = TEMPLATE_STATE_COMPLETE;
    }
    
    // Set a reasonable maximum total template size
    const size_t MAX_TEMPLATE_SIZE = 16384;  // 16KB should be more than enough
    
    // Check if adding this data would exceed our maximum size
    if (template_data.size + length > MAX_TEMPLATE_SIZE) {
        ESP_LOGW(TAG, "Template would exceed maximum size of %d bytes, truncating", MAX_TEMPLATE_SIZE);
        length = MAX_TEMPLATE_SIZE - template_data.size;
        if (length <= 0) {
            // We're already at max size
            ESP_LOGW(TAG, "Template already at maximum size, marking as complete");
            template_data.state = TEMPLATE_STATE_COMPLETE;
            xSemaphoreGive(template_mutex);
            return ESP_OK;
        }
    }
    
    // Check if we need to expand buffer
    if (template_data.size + length > template_data.capacity) {
        size_t new_capacity = template_data.capacity * 2;
        // Cap the new capacity to a reasonable maximum
        if (new_capacity > MAX_TEMPLATE_SIZE) {
            new_capacity = MAX_TEMPLATE_SIZE;
        }
        
        // Ensure the new capacity is large enough
        if (new_capacity < template_data.size + length) {
            new_capacity = template_data.size + length;
        }
        
        uint8_t* new_data = heap_caps_realloc(
            template_data.data, 
            new_capacity, 
            MALLOC_CAP_8BIT
        );
        
        if (new_data == NULL) {
            ESP_LOGE(TAG, "Failed to expand template buffer");
            template_data.state = TEMPLATE_STATE_ERROR;
            xSemaphoreGive(template_mutex);
            return ESP_ERR_NO_MEM;
        }
        
        template_data.data = new_data;
        template_data.capacity = new_capacity;
        ESP_LOGD(TAG, "Template buffer expanded to %d bytes", new_capacity);
    }
    
    // Copy new data
    memcpy(template_data.data + template_data.size, data, length);
    template_data.size += length;
    
    // Log progress occasionally
    if (template_data.size % 512 == 0) {
        ESP_LOGI(TAG, "Template size now %d bytes", template_data.size);
    }
    
    // Check for timeout
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (current_time - template_data.start_time > TEMPLATE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Template collection timed out");
        if (template_data.size > 0) {
            // If we have data, assume it's complete despite timeout
            template_data.state = TEMPLATE_STATE_COMPLETE;
        } else {
            template_data.state = TEMPLATE_STATE_ERROR;
        }
    }
    
    xSemaphoreGive(template_mutex);
    return ESP_OK;
}

void template_manager_reset(void) {
    // Check if mutex exists before trying to use it
    if (template_mutex == NULL) {
        ESP_LOGE(TAG, "Template mutex not initialized before reset");
        return;
    }
    
    if (xSemaphoreTake(template_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire template mutex for reset");
        return;
    }
    
    // Free existing data
    if (template_data.data) {
        free(template_data.data);
        template_data.data = NULL;
    }
    
    template_data.size = 0;
    template_data.capacity = 0;
    template_data.state = TEMPLATE_STATE_IDLE;
    template_data.start_time = 0;
    template_data.found_terminator = false;
    
    xSemaphoreGive(template_mutex);
}

template_state_t template_manager_get_state(void) {
    template_state_t state;
    
    if (xSemaphoreTake(template_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire template mutex for get_state");
        return TEMPLATE_STATE_ERROR;
    }
    
    state = template_data.state;
    xSemaphoreGive(template_mutex);
    
    return state;
}

const fingerprint_template_t* template_manager_get_template(void) {
    if (xSemaphoreTake(template_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire template mutex for get_template");
        return NULL;
    }
    
    if (template_data.state != TEMPLATE_STATE_COMPLETE || 
        template_data.data == NULL || 
        template_data.size == 0) {
        xSemaphoreGive(template_mutex);
        return NULL;
    }
    
    xSemaphoreGive(template_mutex);
    return &template_data;
}

void template_manager_cleanup(void) {
    if (xSemaphoreTake(template_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire template mutex for cleanup");
        return;
    }
    
    if (template_data.data) {
        free(template_data.data);
        template_data.data = NULL;
    }
    
    template_data.size = 0;
    template_data.capacity = 0;
    template_data.state = TEMPLATE_STATE_IDLE;
    
    xSemaphoreGive(template_mutex);
    
    if (template_mutex) {
        vSemaphoreDelete(template_mutex);
        template_mutex = NULL;
    }
}