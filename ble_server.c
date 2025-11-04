#include "ble_server.h"
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "datalogger.h" // Generated from datalogger.gatt
#include "hardware/rtc.h"
#include "pico/util/datetime.h"
#include "ff.h"         // For FatFs file operations
#include "f_util.h"     // For FRESULT_str

// --- Server Role Globals ---
static hci_con_handle_t server_con_handle = HCI_CON_HANDLE_INVALID; 
extern uint8_t const profile_data[]; // From datalogger.h
static bool rtc_is_synced = false;
static FIL streaming_file;
static bool is_streaming = false;
static btstack_timer_source_t stream_timer;
extern void start_pump(void); // From main.c

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

#define STREAM_CHUNK_SIZE 64 // Size of each data packet
static uint8_t stream_buffer[STREAM_CHUNK_SIZE];

// --- Private Function Declarations ---
static void stream_timer_handler(btstack_timer_source_t *ts);
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

    // If the connection is dropped, stop any active stream
    if (handle == HCI_CON_HANDLE_INVALID && is_streaming) {
        printf("Stream abort: Client disconnected.\n");
        is_streaming = false;
        f_close(&streaming_file);
        btstack_run_loop_remove_timer(&stream_timer);
    }

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


// --- Private Functions (File Streaming) ---

/**
 * @brief This is the main streaming logic.
 * It's called by a timer to send one chunk at a time.
 */
static void stream_timer_handler(btstack_timer_source_t *ts) {
    if (!is_streaming) {
        return; // Stream was aborted
    }

    if (server_con_handle == HCI_CON_HANDLE_INVALID) {
        printf("Stream abort: Connection lost.\n");
        is_streaming = false;
        f_close(&streaming_file);
        return;
    }

    UINT bytes_read;
    FRESULT fr = f_read(&streaming_file, stream_buffer, STREAM_CHUNK_SIZE, &bytes_read);
    
    if (fr != FR_OK) {
        printf("Stream abort: File read error: %s\n", FRESULT_str(fr));
        is_streaming = false;
        f_close(&streaming_file);
        return;
    }

    if (bytes_read > 0) {
        // We have data, send it
        att_server_notify(server_con_handle, ATT_CHARACTERISTIC_0xAAA3_01_VALUE_HANDLE, stream_buffer, bytes_read);
        
        // Schedule the next chunk
        btstack_run_loop_set_timer(ts, 1); // 1ms delay
        btstack_run_loop_add_timer(ts);
    } else {
        // End of file (bytes_read == 0)
        printf("Stream complete. Sending EOT.\n");
        
        // Send EOT packet
        const char* eot_msg = "$$EOT$$";
        att_server_notify(server_con_handle, ATT_CHARACTERISTIC_0xAAA3_01_VALUE_HANDLE, (uint8_t*)eot_msg, strlen(eot_msg));
        
        // Clean up
        is_streaming = false;
        f_close(&streaming_file);
    }
}

/**
 * @brief Kicks off the file streaming process.
 * Opens the file and sets the first timer.
 */
static void start_streaming_file(const char* filename) {
    if (is_streaming) {
        printf("Stream already in progress. Ignoring new request.\n");
        return;
    }
    
    if (server_con_handle == HCI_CON_HANDLE_INVALID) {
        printf("Stream error: No valid connection.\n");
        return;
    }

    FRESULT fr = f_open(&streaming_file, filename, FA_READ);
    if (fr != FR_OK) {
        printf("Failed to open file '%s': %s\n", filename, FRESULT_str(fr));
        // TODO: Send an "ERROR:File Not Found" notification
        return;
    }
    
    printf("Starting stream for file: %s\n", filename);
    is_streaming = true;
    
    // Set up the timer
    btstack_run_loop_set_timer_handler(&stream_timer, stream_timer_handler);
    btstack_run_loop_set_timer(&stream_timer, 1); // 1ms delay to kick it off
    btstack_run_loop_add_timer(&stream_timer);
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

    // Check if the write is for our command characteristic
    if (att_handle == ATT_CHARACTERISTIC_0xAAA2_01_VALUE_HANDLE) {
        
        // Create a null-terminated string from the buffer
        char command_buffer[64]; // Assuming max filename + "GET:" is < 64
        uint16_t len = btstack_min(buffer_size, sizeof(command_buffer) - 1);
        memcpy(command_buffer, buffer, len);
        command_buffer[len] = '\0'; // Null terminate

        printf("Command received: %s\n", command_buffer);

        if (strncmp(command_buffer, "GET:", 4) == 0) {
            const char* filename = command_buffer + 4;
            start_streaming_file(filename);
        } else if (strncmp(command_buffer, "PUMP", 4) == 0) {
            printf("PUMP command received.\n");
            start_pump(); // Call the function from main.c
        } else if (strncmp(command_buffer, "LIST", 4) == 0) {
            // TODO: Implement file listing
            printf("File listing not yet implemented.\n");
        }
        return 0;
    }

    return 0;
}