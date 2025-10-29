#include "ble_server.h"
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "datalogger.h" // Generated from datalogger.gatt
#include "hardware/rtc.h"
#include "pico/util/datetime.h"

// --- Server Role Globals ---
static hci_con_handle_t server_con_handle = HCI_CON_HANDLE_INVALID; 
extern uint8_t const profile_data[]; // From datalogger.h
static bool rtc_is_synced = false; 

// Define our advertisement data
static uint8_t adv_data[] = {
    // Flags general discoverable
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    // Name
    0x0F, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 'M', 'i', 'F', 'l', 'o', 'r', 'a', ' ', 'L', 'o', 'g', 'g', 'e', 'r',
    // List of 16-bit Service UUIDs
    0x03, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS, 0xA0, 0xAA, // Our custom service 0xAAA0
};
static const uint8_t adv_data_len = sizeof(adv_data); 

// --- Private Function Declarations ---
static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size);
static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);

// --- Public Function Implementations ---

void ble_server_init(btstack_packet_handler_t att_packet_handler) {
    // Initialize ATT server with our new profile and callbacks
    att_server_init(profile_data, att_read_callback, att_write_callback);
    // Register our HCI event handler to also receive ATT server events
    att_server_register_packet_handler(att_packet_handler);
}

void ble_server_start_advertising(void) {
    printf("Starting BLE advertising...\n");
    uint16_t adv_int_min = 800; 
    uint16_t adv_int_max = 800; 
    uint8_t adv_type = 0;
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0, null_addr, 0x07, 0x00); 
    assert(adv_data_len <= 31); // ble limitation
    gap_advertisements_set_data(adv_data_len, (uint8_t*) adv_data); 
    gap_advertisements_enable(1); 
}

void ble_server_stop_advertising(void) {
    gap_advertisements_enable(0); 
}

bool ble_server_is_rtc_synced(void) {
    return rtc_is_synced;
}

hci_con_handle_t ble_server_get_con_handle(void) {
    return server_con_handle;
}

void ble_server_set_con_handle(hci_con_handle_t handle) {
    server_con_handle = handle;
}

void ble_server_handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);
    
    if (hci_event_packet_get_type(packet) == HCI_EVENT_LE_META &&
        hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
        
        // This is a *server* connection *to* us (e.g., a phone)
        printf("Client connected to our server. Staying in server mode.\n"); 
        server_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet); 
        ble_server_stop_advertising(); 
    }
}

// --- Private Functions (ATT Callbacks) ---

static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size) {
    UNUSED(connection_handle); 
    UNUSED(att_handle); 
    UNUSED(offset); 
    UNUSED(buffer); 
    UNUSED(buffer_size); 
    return 0;
}

static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size) {
    UNUSED(connection_handle); 
    UNUSED(transaction_mode); 
    UNUSED(offset); 

    // Check if the write is for our custom timestamp characteristic
    if (att_handle == ATT_CHARACTERISTIC_0xAAA1_01_VALUE_HANDLE) { 
        
        // We expect 7 bytes: [Year_H, Year_L, Month, Day, Hour, Min, Sec]
        if (buffer_size != 7) {
            printf("RTC Write: Invalid buffer size. Expected 7, got %u\n", buffer_size);
            return 0; // Error
        }

        datetime_t t;
        t.year  = little_endian_read_16(buffer, 0); 
        t.month = buffer[2]; 
        t.day   = buffer[3]; 
        t.hour  = buffer[4]; 
        t.min   = buffer[5]; 
        t.sec   = buffer[6]; 
        t.dotw  = 0; // Day of week is not critical

        printf("RTC Write: Received new time %04d-%02d-%02dT%02d:%02d:%02d\n",
                t.year, t.month, t.day, t.hour, t.min, t.sec); 
        
        if (!rtc_set_datetime(&t)) { 
            printf("RTC Write: FAILED to set new time.\n"); 
        } else {
            printf("RTC Write: SUCCESS. RTC has been synced.\n"); 
            rtc_is_synced = true; // Set flag on success
        }
        return 0;
    }
    return 0;
}