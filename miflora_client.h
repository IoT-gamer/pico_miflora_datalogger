#ifndef MIFLORA_CLIENT_H
#define MIFLORA_CLIENT_H

#include <stdint.h>
#include "btstack.h"

// Struct to hold the parsed sensor data 
typedef struct {
    float temperature;
    uint32_t light;
    uint8_t moisture;
    uint16_t conductivity;
    uint8_t battery;
} miflora_reading_t;

// State machine for the multi-step read 
typedef enum {
    FLORA_OFF,
    FLORA_IDLE, // Server Mode 
    FLORA_W4_SCAN_RESULT,
    FLORA_W4_CONNECT,
    FLORA_W4_SERVICE_RESULT,
    FLORA_W4_CHARACTERISTICS_RESULT, // Discovering all 3 chars 
    FLORA_W4_WRITE_MODE_COMPLETE,    // Waiting for mode write to finish 
    FLORA_W4_READ_DATA_COMPLETE,     // Waiting for main data read 
    FLORA_W4_READ_BATT_COMPLETE      // Waiting for battery data read 
} miflora_state_t;

/**
 * @brief Initialize the MiFlora client with the target sensor's MAC address.
 */
void miflora_client_init(const char *mac_string);

/**
 * @brief Start scanning for the MiFlora sensor.
 */
void miflora_client_start(void);

/**
 * @brief Handle GATT client events (service/characteristic discovery, reads).
 */
void miflora_client_handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

/**
 * @brief Handle HCI events related to the client role (scan results, connection).
 */
void miflora_client_handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// --- State Management Functions ---
miflora_state_t miflora_client_get_state(void);
void miflora_client_set_state(miflora_state_t new_state);
hci_con_handle_t miflora_client_get_con_handle(void);
void miflora_client_set_con_handle(hci_con_handle_t handle);
miflora_reading_t* miflora_client_get_last_reading(void);

/**
 * @brief Print all sensor readings to the console.
 */
void miflora_client_print_reading(void);

#endif // MIFLORA_CLIENT_H