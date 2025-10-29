/**
 * Miflora Sensor (BLE) for Raspberry Pi Pico W
 * * With SD Card Datalogging and Timestamps!
 * * Also acts as a peripheral to allow RTC syncing.
*/

#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

// --- SD Card Includes ---
#include "hw_config.h"
#include "f_util.h"
#include "ff.h"

// --- *** RTC Includes for Timestamp *** ---
#include "hardware/rtc.h"
#include "pico/util/datetime.h"

// --- *** Server Role Includes *** ---
#include "datalogger.h" // Generated from datalogger.gatt
// Note: We do NOT include "att_server.h"

#if 0
#define DEBUG_LOG(...) printf(__VA_ARGS__)
#else
#define DEBUG_LOG(...)
#endif

#define LED_QUICK_FLASH_DELAY_MS 100
#define LED_SLOW_FLASH_DELAY_MS 1000

// --- Miflora Definitions ---
static const char * target_mac_string = "5C:85:7E:13:17:F9";  // Change to your sensor's MAC address
static bd_addr_t target_mac_addr;

#define TARGET_SERVICE_UUID 0x1204
#define TARGET_CHAR_MODE_UUID 0x1A00
#define TARGET_CHAR_DATA_UUID 0x1A01
#define TARGET_CHAR_BATT_UUID 0x1A02

// Command to write to 0x1A00 to request data
static uint8_t mode_command[2] = {0xA0, 0x1F};
// Struct to hold the parsed sensor data
typedef struct {
    float temperature;
    uint32_t light;
    uint8_t moisture;
    uint16_t conductivity;
    uint8_t battery;
} miflora_reading_t;
// --- End Definitions ---

// Refactored state machine for the multi-step read
typedef enum {
    FLORA_OFF,
    FLORA_IDLE, // This is now our "Server Mode"
    FLORA_W4_SCAN_RESULT,
    FLORA_W4_CONNECT,
    FLORA_W4_SERVICE_RESULT,
    FLORA_W4_CHARACTERISTICS_RESULT, // Discovering all 3 chars
    FLORA_W4_WRITE_MODE_COMPLETE,    // Waiting for mode write to finish
    FLORA_W4_READ_DATA_COMPLETE,     // Waiting for main data read
    FLORA_W4_READ_BATT_COMPLETE      // Waiting for battery data read
} miflora_state_t;

// --- Global BLE State Variables ---
static btstack_packet_callback_registration_t hci_event_callback_registration;
static miflora_state_t state = FLORA_OFF;
static bd_addr_t server_addr;
static bd_addr_type_t server_addr_type;
static hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID; // Client connection (to MiFlora)
static gatt_client_service_t server_service;
static gatt_client_characteristic_t char_mode;
static gatt_client_characteristic_t char_data;
static gatt_client_characteristic_t char_battery;

static btstack_timer_source_t heartbeat;
static miflora_reading_t current_reading; // Store the latest complete reading

// --- Server Role Globals ---
static hci_con_handle_t server_con_handle = HCI_CON_HANDLE_INVALID;
// Server connection (from phone)
extern uint8_t const profile_data[]; // From datalogger.h

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

// --- Modal Logic Timers ---
static btstack_timer_source_t server_advertisement_timer;
static btstack_timer_source_t start_scan_delay_timer;
// Temporary storage for read data
static uint8_t  temp_read_value[30];
static uint16_t temp_read_value_length;

// --- SD Card Globals ---
static FATFS fs;
static bool sd_mounted = false;
static bool rtc_is_synced = false; // Flag to track RTC sync status
// ---------------------------------------------


// --- Function Declarations ---
static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void client_start(void);
static void parseSensorData(const uint8_t *data, uint16_t length, miflora_reading_t *reading);
static void print_reading(miflora_reading_t *reading);
static void log_reading_to_sd(miflora_reading_t *reading);
static void start_advertising(void);
static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size);
static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static void server_timeout_handler(struct btstack_timer_source *ts);
static void enter_server_mode(void);
static void start_scan_handler(struct btstack_timer_source *ts);


/**
 * @brief Start scanning for the sensor
 */
static void client_start(void){
    DEBUG_LOG("Start scanning for Miflora %s!\n", target_mac_string);
    state = FLORA_W4_SCAN_RESULT;
    gap_set_scan_parameters(0,0x0030, 0x0030);
    gap_start_scan();
}

/**
 * @brief Parse the 16-byte data from characteristic 0x1A01
 */
static void parseSensorData(const uint8_t *data, uint16_t length, miflora_reading_t *reading) {
    if (length < 16) {
        printf("Invalid data length: %u bytes, expected 16\n", length);
        return;
    }
    int16_t temp_raw = (int16_t)little_endian_read_16(data, 0);
    reading->temperature = temp_raw / 10.0f;
    reading->light = little_endian_read_32(data, 3);
    reading->moisture = data[7];
    reading->conductivity = little_endian_read_16(data, 8);
}

/**
 * @brief Parse the battery data from characteristic 0x1A02
 */
static void parseBatteryData(const uint8_t *data, uint16_t length, miflora_reading_t *reading) {
    if (length > 0) {
        reading->battery = data[0]; // Battery is byte 0
    }
}


/**
 * @brief Print all sensor readings to the console
 */
static void print_reading(miflora_reading_t *reading) {
    printf("\n--- Miflora Data ---\n");
    printf("  Temperature:  %.1f C\n", reading->temperature);
    printf("  Light:        %lu lux\n", (unsigned long)reading->light);
    printf("  Moisture:     %u %%\n", reading->moisture);
    printf("  Conductivity: %u uS/cm\n", reading->conductivity);
    printf("  Battery:      %u %%\n", reading->battery);
    printf("--------------------\n");
}

/**
 * @brief Log all sensor readings to the SD card, now with a timestamp
 */
static void log_reading_to_sd(miflora_reading_t *reading) {
    if (!sd_mounted) {
        printf("SD card not mounted. Skipping log.\n");
        return;
    }

    // --- *** Get and format timestamp *** ---
    datetime_t t;
    char timestamp_buf[32]; // Buffer for "YYYY-MM-DDTHH:MM:SS"
    
    if (!rtc_get_datetime(&t)) {
        printf("Failed to get RTC time. Using 'unknown'.\n");
        snprintf(timestamp_buf, sizeof(timestamp_buf), "unknown");
    } else {
        // Format as ISO 8601
        snprintf(timestamp_buf, sizeof(timestamp_buf),
                 "%04d-%02d-%02dT%02d:%02d:%02d",
                 t.year, t.month, t.day, t.hour, t.min, t.sec);
    }
    // ------------------------------------------

    FIL fil;
    const char* const filename = "miflora_log.txt";
    // Open file in append mode
    FRESULT fr = f_open(&fil, filename, FA_OPEN_APPEND | FA_WRITE);
    if (FR_OK != fr && FR_EXIST != fr) {
        printf("f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        return;
    }

    // --- *** Write timestamp + data as a CSV-like string *** ---
    int chars_written = f_printf(&fil, "%s,Temp:%.1f,Light:%lu,Moisture:%u,Conductivity:%u,Battery:%u\n",
            timestamp_buf,
            reading->temperature,
            reading->light,
            reading->moisture,
            reading->conductivity,
            reading->battery);

    if (chars_written < 0) {
        printf("f_printf failed\n");
    }

    // Close the file (this also flushes the write buffer)
    fr = f_close(&fil);
    if (FR_OK != fr) {
        printf("f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    } else {
        printf("Successfully logged reading to %s\n", filename);
    }
}


// +++ SERVER FUNCTIONS +++

/**
 * @brief Start advertising the Pico as a server
 * Based on server.c example
 */
static void start_advertising(void){
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

/**
 * @brief ATT read callback (not used for this characteristic)
 */
static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size) {
    UNUSED(connection_handle);
    UNUSED(att_handle);
    UNUSED(offset);
    UNUSED(buffer);
    UNUSED(buffer_size);
    return 0;
}

/**
 * @brief ATT write callback - This is where we sync the RTC
 * This function is called when a client writes to our 0xAAA1 characteristic.
*/
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

// +++ MODAL LOGIC FUNCTIONS +++

/**
 * @brief Handler for the short delay after stopping ADV.
* This function actually starts the MiFlora scan.
 */
static void start_scan_handler(struct btstack_timer_source *ts) {
    UNUSED(ts);
    printf("ADV stop delay complete. Starting MiFlora scan.\n");
    // Now it's safe to start scanning
    client_start();
}

/**
 * @brief This handler fires if no phone connects to our server within the timeout.
* It stops advertising and switches to client (MiFlora scan) mode.
 * Checks rtc_is_synced flag to decide whether to scan or re-enter server mode.
 */
static void server_timeout_handler(struct btstack_timer_source *ts) {
    UNUSED(ts);
    
    gap_advertisements_enable(0); // Stop advertising

    if (rtc_is_synced) {
        printf("Server mode timed out, RTC is synced. Proceeding to scan.\n");
        
        // DO NOT call client_start() here.
        // Set a 100ms timer to give the stack time to stop advertising
        // before we start scanning.
        // This prevents the race condition.
        btstack_run_loop_set_timer_handler(&start_scan_delay_timer, start_scan_handler);
        btstack_run_loop_set_timer(&start_scan_delay_timer, 100); // 100ms delay
        btstack_run_loop_add_timer(&start_scan_delay_timer);
    } else {
        printf("Server mode timed out. RTC NOT synced. Restarting server mode...\n");
        // Re-enter server mode to wait for a connection again.
        enter_server_mode();
    }
}

/**
 * @brief Enters the default "server" state.
 * Advertises as "MiFlora Logger" and sets a timer.
*/
static void enter_server_mode(void){
    // First, always remove the timer.
    // This is safe even if it's not on the list.
    // This prevents the "add_timer" crash if we get two
    // disconnect events in a row.
    btstack_run_loop_remove_timer(&server_advertisement_timer);
    printf("Entering server mode. Advertising for 30 seconds...\n");
    state = FLORA_IDLE; // Set state to idle (server) mode
    start_advertising(); // Start advertising as "MiFlora Logger"
    
    // Now it is safe to set up and add the timer
    btstack_run_loop_set_timer_handler(&server_advertisement_timer, server_timeout_handler);
    btstack_run_loop_set_timer(&server_advertisement_timer, 30000);
    btstack_run_loop_add_timer(&server_advertisement_timer);
}

/**
 * @brief Main GATT event handler and state machine
 */
static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    uint8_t att_status;

    // Helper macro to handle disconnection on error
    #define CHECK_ATT_STATUS_AND_DISCONNECT(packet) \
        att_status = gatt_event_query_complete_get_att_status(packet); \
        if (att_status != ATT_ERROR_SUCCESS){ \
            printf("GATT Error 0x%02x, disconnecting.\n", att_status); \
            state = FLORA_IDLE; \
            gap_disconnect(connection_handle);\
            break; \
        }

    switch(state){
        case FLORA_W4_SERVICE_RESULT:
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_SERVICE_QUERY_RESULT:
                    DEBUG_LOG("Storing service\n");
                    gatt_event_service_query_result_get_service(packet, &server_service);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    printf("Found service 0x%04X, discovering characteristics...\n", TARGET_SERVICE_UUID);
                    state = FLORA_W4_CHARACTERISTICS_RESULT;
                    char_mode.value_handle = 0;
                    char_data.value_handle = 0;
                    char_battery.value_handle = 0;
                    gatt_client_discover_characteristics_for_service(handle_gatt_client_event, connection_handle, &server_service);
                    break;
                default:
                    break;
            }
            break;
        case FLORA_W4_CHARACTERISTICS_RESULT:
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
                    gatt_client_characteristic_t characteristic;
                    gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
                    uint16_t uuid = characteristic.uuid16;

                    if (uuid == TARGET_CHAR_MODE_UUID) {
                        memcpy(&char_mode, &characteristic, sizeof(gatt_client_characteristic_t));
                        DEBUG_LOG("Found Mode Char (0x%04X)\n", uuid);
                    } else if (uuid == TARGET_CHAR_DATA_UUID) {
                        memcpy(&char_data, &characteristic, sizeof(gatt_client_characteristic_t));
                        DEBUG_LOG("Found Data Char (0x%04X)\n", uuid);
                    } else if (uuid == TARGET_CHAR_BATT_UUID) {
                        memcpy(&char_battery, &characteristic, sizeof(gatt_client_characteristic_t));
                        DEBUG_LOG("Found Battery Char (0x%04X)\n", uuid);
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE:
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    if (char_mode.value_handle == 0 || char_data.value_handle == 0 || char_battery.value_handle == 0) {
                        printf("Failed to find all required characteristics. Disconnecting.\n");
                        state = FLORA_IDLE;
                        gap_disconnect(connection_handle);
                        break;
                    }
                    
                    printf("Found all characteristics. Writing mode command...\n");
                    state = FLORA_W4_WRITE_MODE_COMPLETE;
                    gatt_client_write_value_of_characteristic(handle_gatt_client_event, connection_handle, char_mode.value_handle, sizeof(mode_command), mode_command);
                    break;
                default:
                    break;
            }
            break;
        case FLORA_W4_WRITE_MODE_COMPLETE:
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_QUERY_COMPLETE:
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    printf("Mode write complete. Reading sensor data...\n");
                    state = FLORA_W4_READ_DATA_COMPLETE;
                    gatt_client_read_value_of_characteristic(handle_gatt_client_event, connection_handle, &char_data);
                    break;
                default:
                    break;
            }
            break;
        case FLORA_W4_READ_DATA_COMPLETE:
            switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
                    temp_read_value_length = gatt_event_characteristic_value_query_result_get_value_length(packet);
                    const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet);
                    if (temp_read_value_length > 0 && temp_read_value_length <= sizeof(temp_read_value)) {
                        memcpy(temp_read_value, value, temp_read_value_length);
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE: {
                    CHECK_ATT_STATUS_AND_DISCONNECT(packet);
                    parseSensorData(temp_read_value, temp_read_value_length, &current_reading);
                    
                    printf("Data read complete. Reading battery...\n");
                    state = FLORA_W4_READ_BATT_COMPLETE;
                    gatt_client_read_value_of_characteristic(handle_gatt_client_event, connection_handle, &char_battery);
                    break;
                }
                default:
                    break;
            }
            break;
        case FLORA_W4_READ_BATT_COMPLETE:
             switch(hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
                    temp_read_value_length = gatt_event_characteristic_value_query_result_get_value_length(packet);
                    const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet);
                    if (temp_read_value_length > 0 && temp_read_value_length <= sizeof(temp_read_value)) {
                        memcpy(temp_read_value, value, temp_read_value_length);
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE: {
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                         printf("Battery read failed, error 0x%02x\n", att_status);
                    } else {
                        parseBatteryData(temp_read_value, temp_read_value_length, &current_reading);
                    }
                    
                    printf("Battery read complete.\n");
                    // 1. Print to console
                    print_reading(&current_reading);
                    // 2. Log to SD card
                    printf("Logging data to SD card...\n");
                    log_reading_to_sd(&current_reading); // <-- This function now handles timestamps
                    
                    state = FLORA_IDLE;
                    gap_disconnect(connection_handle);
                    break;
                }
                default:
                    break;
            }
            break;
        default:
            DEBUG_LOG("Unhandled state %d, event 0x%02x\n", state, hci_event_packet_get_type(packet));
            break;
    }
}


/**
 * @brief Main HCI event handler (scan, connect, disconnect)
 */
static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(size);
    UNUSED(channel);
    bd_addr_t local_addr;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    switch(event_type){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                gap_local_bd_addr(local_addr);
                printf("BTstack up and running on %s.\n", bd_addr_to_str(local_addr));

                // Start in server mode
                enter_server_mode();
            } else {
                state = FLORA_OFF;
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            if (state != FLORA_W4_SCAN_RESULT) return;
            
            bd_addr_t event_addr;
            gap_event_advertising_report_get_address(packet, event_addr);
            
            if (memcmp(event_addr, target_mac_addr, 6) != 0) {
                return; // Not our device
            }

            printf("Found Miflora sensor: %s\n", bd_addr_to_str(event_addr));
            memcpy(server_addr, event_addr, 6);
            server_addr_type = gap_event_advertising_report_get_address_type(packet);
            
            state = FLORA_W4_CONNECT;
            gap_stop_scan();
            printf("...connecting to check for service 0x%04X...\n", TARGET_SERVICE_UUID);
            gap_connect(server_addr, server_addr_type);
            break;
        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
                case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                    
                    if (state == FLORA_W4_CONNECT) {
                        // This is our *client* connection *to* the MiFlora
                        connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                        printf("Connected to MiFlora. Searching for service 0x%04X.\n", TARGET_SERVICE_UUID);
                        state = FLORA_W4_SERVICE_RESULT;
                        gatt_client_discover_primary_services_by_uuid16(handle_gatt_client_event, connection_handle, TARGET_SERVICE_UUID);
                    } 
                    else if (state == FLORA_IDLE) 
                    {
                        // This is a *server* connection *to* us (e.g., a phone)
                        printf("Client connected to our server. Staying in server mode.\n");
                        btstack_run_loop_remove_timer(&server_advertisement_timer);
                        server_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                        gap_advertisements_enable(0); // Stop advertising
                    }
                    else {
                        // We are in a busy state (e.g., FLORA_W4_SERVICE_RESULT)
                        // and received another CONNECTION_COMPLETE event.
                        // This is almost certainly a ghost/duplicate event from
                        // the MiFlora connection.
                        // The safest thing to do is just ignore it.
                        printf("Ignoring duplicate connection event in state %d.\n", state);
                        // By doing nothing, we stay connected to the MiFlora
                        // and proceed as normal.
                    }
                    break;
                default:
                    break;
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            { 
                hci_con_handle_t disconnected_handle = hci_event_disconnection_complete_get_connection_handle(packet);
                if (server_con_handle == disconnected_handle){
                    server_con_handle = HCI_CON_HANDLE_INVALID;
                    printf("Client disconnected from our server.\n");
                }
                
                if (connection_handle == disconnected_handle){
                    connection_handle = HCI_CON_HANDLE_INVALID;
                    printf("Disconnected from MiFlora.\n");
                }
                
                // Only enter server mode if BOTH connections are invalid
                if (connection_handle == HCI_CON_HANDLE_INVALID && server_con_handle == HCI_CON_HANDLE_INVALID){
                    printf("All connections closed. Re-entering server mode.\n");
                    enter_server_mode(); // Go back to advertising
                }

                if (connection_handle == HCI_CON_HANDLE_INVALID && 
                    server_con_handle == HCI_CON_HANDLE_INVALID && 
                    state != FLORA_IDLE)
                {
                    printf("All connections closed. Re-entering server mode.\n");
                    enter_server_mode(); // Go back to advertising
                }
            }
            break;
        default:
            break;
    }
}

/**
 * @brief Timer handler - Used for LED flash ONLY
 */
static void heartbeat_handler(struct btstack_timer_source *ts) {
    static bool quick_flash;
    static bool led_on = true;

    led_on = !led_on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
    // Quick flash if we are connected to *either* device
    if ((connection_handle != HCI_CON_HANDLE_INVALID || server_con_handle != HCI_CON_HANDLE_INVALID) && led_on) {
        quick_flash = !quick_flash;
    } else if (connection_handle == HCI_CON_HANDLE_INVALID && server_con_handle == HCI_CON_HANDLE_INVALID) {
        quick_flash = false;
    }

    // Restart timer
    btstack_run_loop_set_timer(ts, (led_on || quick_flash) ? LED_QUICK_FLASH_DELAY_MS : LED_SLOW_FLASH_DELAY_MS);
    btstack_run_loop_add_timer(ts);
}

int main() {
    stdio_init_all();

    sleep_ms(2000); // Wait for stdio
    
    // --- Initialize RTC ---
    printf("Initializing RTC...\n");
    rtc_init();

    printf("RTC initialized. Waiting for time sync from app...\n");
    // ------------------------------------

    printf("--- Pico W Miflora Datalogger ---\n");
    sscanf_bd_addr(target_mac_string, target_mac_addr);
    if (cyw43_arch_init()) {
        printf("failed to initialise cyw43_arch\n");
        return -1;
    }

    // --- *** Mount SD Card *** ---
    // Wait for USB serial to connect (optional, but helpful for debugging)
    // while (!stdio_usb_connected()) {
    //     sleep_ms(100);
    // }
    
    printf("Mounting SD card...\n");
    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf("f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        sd_mounted = false;
    } else {
        printf("SD card mounted successfully.\n");
        sd_mounted = true;
    }
    // ------------------------------------

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    // Initialize ATT server with our new profile and callbacks
    att_server_init(profile_data, att_read_callback, att_write_callback);

    gatt_client_init();

    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    
    // Register our HCI event handler to also receive ATT server events
    att_server_register_packet_handler(&hci_event_handler);
    // Set up LED flashing timer
    heartbeat.process = &heartbeat_handler;
    btstack_run_loop_set_timer(&heartbeat, LED_SLOW_FLASH_DELAY_MS);
    btstack_run_loop_add_timer(&heartbeat);

    hci_power_control(HCI_POWER_ON);
    
    btstack_run_loop_execute();

    return 0;
}