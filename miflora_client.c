#include "miflora_client.h"
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "sd_logger.h" // Include for logging

#if 0
#define DEBUG_LOG(...) printf(__VA_ARGS__)
#else
#define DEBUG_LOG(...)
#endif

// --- Miflora Definitions ---
static bd_addr_t target_mac_addr; //
#define TARGET_SERVICE_UUID 0x1204 //
#define TARGET_CHAR_MODE_UUID 0x1A00 //
#define TARGET_CHAR_DATA_UUID 0x1A01 //
#define TARGET_CHAR_BATT_UUID 0x1A02 //
static uint8_t mode_command[2] = {0xA0, 0x1F}; //

// --- Global BLE State Variables ---
static miflora_state_t state = FLORA_OFF; //
static bd_addr_t server_addr;
static bd_addr_type_t server_addr_type;
static hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID; //
static gatt_client_service_t server_service; //
static gatt_client_characteristic_t char_mode; //
static gatt_client_characteristic_t char_data; //
static gatt_client_characteristic_t char_battery; //
static miflora_reading_t current_reading; //

// Temporary storage for read data
static uint8_t  temp_read_value[30]; //
static uint16_t temp_read_value_length; //

// --- Private Function Declarations ---
// *** FIX 1: Removed the static forward declaration for handle_gatt_client_event ***
static void parseSensorData(const uint8_t *data, uint16_t length, miflora_reading_t *reading);
static void parseBatteryData(const uint8_t *data, uint16_t length, miflora_reading_t *reading);

// --- Public Function Implementations ---

void miflora_client_init(const char *mac_string) {
    sscanf_bd_addr(mac_string, target_mac_addr); //
    state = FLORA_IDLE;
}

void miflora_client_start(void) {
    DEBUG_LOG("Start scanning for Miflora!\n");
    state = FLORA_W4_SCAN_RESULT; //
    gap_set_scan_parameters(0, 0x0030, 0x0030);
    gap_start_scan();
}

miflora_state_t miflora_client_get_state(void) {
    return state;
}

void miflora_client_set_state(miflora_state_t new_state) {
    state = new_state;
}

hci_con_handle_t miflora_client_get_con_handle(void) {
    return connection_handle;
}

void miflora_client_set_con_handle(hci_con_handle_t handle) {
    connection_handle = handle;
}

miflora_reading_t* miflora_client_get_last_reading(void) {
    return &current_reading;
}

void miflora_client_print_reading(void) {
    printf("\n--- Miflora Data ---\n");
    printf("  Temperature:  %.1f C\n", current_reading.temperature); //
    printf("  Light:        %lu lux\n", (unsigned long)current_reading.light); //
    printf("  Moisture:     %u %%\n", current_reading.moisture); //
    printf("  Conductivity: %u uS/cm\n", current_reading.conductivity); //
    printf("  Battery:      %u %%\n", current_reading.battery); //
    printf("--------------------\n");
}

// --- HCI Event Handler (Client-specific parts) ---

void miflora_client_handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t event_type = hci_event_packet_get_type(packet);
    
    switch (event_type) {
        case GAP_EVENT_ADVERTISING_REPORT:
            if (state != FLORA_W4_SCAN_RESULT) return; //
            
            bd_addr_t event_addr;
            gap_event_advertising_report_get_address(packet, event_addr); //
            
            if (memcmp(event_addr, target_mac_addr, 6) != 0) {
                return; // Not our device
            }

            printf("Found Miflora sensor: %s\n", bd_addr_to_str(event_addr)); //
            memcpy(server_addr, event_addr, 6); //
            server_addr_type = gap_event_advertising_report_get_address_type(packet); //
            
            state = FLORA_W4_CONNECT; //
            gap_stop_scan();
            printf("...connecting to check for service 0x%04X...\n", TARGET_SERVICE_UUID);
            gap_connect(server_addr, server_addr_type); //
            break;

        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                // This is our *client* connection *to* the MiFlora
                connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet); //
                printf("Connected to MiFlora. Searching for service 0x%04X.\n", TARGET_SERVICE_UUID); //
                state = FLORA_W4_SERVICE_RESULT; //
                // *** FIX 4: Update internal callback references ***
                gatt_client_discover_primary_services_by_uuid16(miflora_client_handle_gatt_event, connection_handle, TARGET_SERVICE_UUID); //
            }
            break;
        
        default:
            break;
    }
}

// --- Private Functions (Moved from main.c) ---

static void parseSensorData(const uint8_t *data, uint16_t length, miflora_reading_t *reading) {
    if (length < 16) {
        printf("Invalid data length: %u bytes, expected 16\n", length);
        return; //
    }
    int16_t temp_raw = (int16_t)little_endian_read_16(data, 0);
    reading->temperature = temp_raw / 10.0f;
    reading->light = little_endian_read_32(data, 3);
    reading->moisture = data[7]; //
    reading->conductivity = little_endian_read_16(data, 8); //
}

static void parseBatteryData(const uint8_t *data, uint16_t length, miflora_reading_t *reading) {
    if (length > 0) {
        reading->battery = data[0]; // Battery is byte 0
    }
}

/**
 * @brief Main GATT event handler and state machine
 * This is the original handle_gatt_client_event function, now public.
 */
// *** FIX 2: Renamed function to match header and removed 'static' ***
void miflora_client_handle_gatt_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type); //
    UNUSED(channel); //
    UNUSED(size); //

    uint8_t att_status;

    // Helper macro to handle disconnection on error
    #define CHECK_ATT_STATUS_AND_DISCONNECT(packet) \
        att_status = gatt_event_query_complete_get_att_status(packet); \
        if (att_status != ATT_ERROR_SUCCESS){ \
            printf("GATT Error 0x%02x, disconnecting.\n", att_status); \
            state = FLORA_IDLE; /* */ \
            gap_disconnect(connection_handle); /* */ \
            break; \
        } 

    switch(state){
        case FLORA_W4_SERVICE_RESULT:
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_SERVICE_QUERY_RESULT:
                    DEBUG_LOG("Storing service\n");
                    gatt_event_service_query_result_get_service(packet, &server_service); //
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    printf("Found service 0x%04X, discovering characteristics...\n", TARGET_SERVICE_UUID); //
                    state = FLORA_W4_CHARACTERISTICS_RESULT; //
                    char_mode.value_handle = 0;
                    char_data.value_handle = 0;
                    char_battery.value_handle = 0;
                    // *** FIX 4: Update internal callback references ***
                    gatt_client_discover_characteristics_for_service(miflora_client_handle_gatt_event, connection_handle, &server_service); //
                    break;
                default:
                    break;
            }
            break;
        case FLORA_W4_CHARACTERISTICS_RESULT: //
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
                    gatt_client_characteristic_t characteristic;
                    gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic); //
                    uint16_t uuid = characteristic.uuid16;

                    if (uuid == TARGET_CHAR_MODE_UUID) {
                        memcpy(&char_mode, &characteristic, sizeof(gatt_client_characteristic_t));
                        DEBUG_LOG("Found Mode Char (0x%04X)\n", uuid); //
                    } else if (uuid == TARGET_CHAR_DATA_UUID) {
                        memcpy(&char_data, &characteristic, sizeof(gatt_client_characteristic_t));
                        DEBUG_LOG("Found Data Char (0x%04X)\n", uuid); //
                    } else if (uuid == TARGET_CHAR_BATT_UUID) {
                        memcpy(&char_battery, &characteristic, sizeof(gatt_client_characteristic_t));
                        DEBUG_LOG("Found Battery Char (0x%04X)\n", uuid); //
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE:
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    if (char_mode.value_handle == 0 || char_data.value_handle == 0 || char_battery.value_handle == 0) { //
                        printf("Failed to find all required characteristics. Disconnecting.\n");
                        state = FLORA_IDLE; //
                        gap_disconnect(connection_handle);
                        break;
                    }
                    
                    printf("Found all characteristics. Writing mode command...\n");
                    state = FLORA_W4_WRITE_MODE_COMPLETE; //
                    // *** FIX 4: Update internal callback references ***
                    gatt_client_write_value_of_characteristic(miflora_client_handle_gatt_event, connection_handle, char_mode.value_handle, sizeof(mode_command), mode_command); //
                    break;
                default:
                    break;
            }
            break;
        case FLORA_W4_WRITE_MODE_COMPLETE: //
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_QUERY_COMPLETE:
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    printf("Mode write complete. Reading sensor data...\n"); //
                    state = FLORA_W4_READ_DATA_COMPLETE; //
                    // *** FIX 4: Update internal callback references ***
                    gatt_client_read_value_of_characteristic(miflora_client_handle_gatt_event, connection_handle, &char_data); //
                    break;
                default:
                    break;
            }
            break;
        case FLORA_W4_READ_DATA_COMPLETE: //
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
                    temp_read_value_length = gatt_event_characteristic_value_query_result_get_value_length(packet); //
                    const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet); //
                    if (temp_read_value_length > 0 && temp_read_value_length <= sizeof(temp_read_value)) {
                        memcpy(temp_read_value, value, temp_read_value_length); //
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE: {
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    parseSensorData(temp_read_value, temp_read_value_length, &current_reading); //
                    
                    printf("Data read complete. Reading battery...\n");
                    state = FLORA_W4_READ_BATT_COMPLETE; //
                    // *** FIX 4: Update internal callback references ***
                    gatt_client_read_value_of_characteristic(miflora_client_handle_gatt_event, connection_handle, &char_battery); //
                    break;
                }
                default:
                    break;
            }
            break;
        case FLORA_W4_READ_BATT_COMPLETE: //
             switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
                    temp_read_value_length = gatt_event_characteristic_value_query_result_get_value_length(packet); //
                    const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet); //
                    if (temp_read_value_length > 0 && temp_read_value_length <= sizeof(temp_read_value)) {
                        memcpy(temp_read_value, value, temp_read_value_length); //
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE: {
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) { //
                         printf("Battery read failed, error 0x%02x\n", att_status); //
                    } else {
                        parseBatteryData(temp_read_value, temp_read_value_length, &current_reading); //
                    }
                    
                    printf("Battery read complete.\n");
                    // 1. Print to console
                    miflora_client_print_reading(); //
                    // 2. Log to SD card
                    printf("Logging data to SD card...\n");
                    sd_logger_log_reading(&current_reading); //
                    
                    state = FLORA_IDLE; //
                    gap_disconnect(connection_handle); //
                    break;
                }
                default:
                    break;
            }
            break;
        default:
            DEBUG_LOG("Unhandled state %d, event 0x%02x\n", state, hci_event_packet_get_type(packet)); //
            break;
    }
}