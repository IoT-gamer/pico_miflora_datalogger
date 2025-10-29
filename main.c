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
#include "hardware/rtc.h" 

// --- Project Modules ---
#include "miflora_client.h"
#include "ble_server.h"
#include "sd_logger.h"

#define LED_QUICK_FLASH_DELAY_MS 100 
#define LED_SLOW_FLASH_DELAY_MS 1000 

// --- Miflora Definitions ---
static const char * target_mac_string = "5C:85:7E:13:17:F9"; // Change to your sensor's MAC address 

// --- Global State ---
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t heartbeat; 

// --- Modal Logic Timers ---
static btstack_timer_source_t server_advertisement_timer; 
static btstack_timer_source_t start_scan_delay_timer; 

// --- Function Declarations ---
static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void heartbeat_handler(struct btstack_timer_source *ts);
static void server_timeout_handler(struct btstack_timer_source *ts);
static void enter_server_mode(void);
static void start_scan_handler(struct btstack_timer_source *ts);


/**
 * @brief Handler for the short delay after stopping ADV.
 * This function actually starts the MiFlora scan.
 */ 
static void start_scan_handler(struct btstack_timer_source *ts) {
    UNUSED(ts); 
    printf("ADV stop delay complete. Starting MiFlora scan.\n"); 
    // Now it's safe to start scanning
    miflora_client_start(); 
}

/**
 * @brief This handler fires if no phone connects to our server within the timeout.
 * It stops advertising and switches to client (MiFlora scan) mode
 * *if* the RTC has been synced.
 */ 
static void server_timeout_handler(struct btstack_timer_source *ts) {
    UNUSED(ts);
    
    ble_server_stop_advertising(); 

    if (ble_server_is_rtc_synced()) {
        printf("Server mode timed out, RTC is synced. Proceeding to scan.\n"); 
        // Set a 100ms timer to give the stack time to stop advertising
        // before we start scanning. 
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
    btstack_run_loop_remove_timer(&server_advertisement_timer); 
    
    printf("Entering server mode. Advertising for 30 seconds...\n"); 
    miflora_client_set_state(FLORA_IDLE); // Set state to idle (server) mode 
    ble_server_start_advertising(); // Start advertising as "MiFlora Logger" 
    
    // Now it is safe to set up and add the timer
    btstack_run_loop_set_timer_handler(&server_advertisement_timer, server_timeout_handler);
    btstack_run_loop_set_timer(&server_advertisement_timer, 30000); 
    btstack_run_loop_add_timer(&server_advertisement_timer); 
}


/**
 * @brief Main HCI event handler (scan, connect, disconnect)
 * This function now delegates to the client and server modules.
 */
static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(size); 
    UNUSED(channel); 
    bd_addr_t local_addr;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet); 
    
    // Delegate GATT client events
    if (event_type == GATT_EVENT_SERVICE_QUERY_RESULT ||
        event_type == GATT_EVENT_CHARACTERISTIC_QUERY_RESULT ||
        event_type == GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT ||
        event_type == GATT_EVENT_QUERY_COMPLETE) {
        
        if (miflora_client_get_con_handle() != HCI_CON_HANDLE_INVALID) {
            miflora_client_handle_gatt_event(packet_type, channel, packet, size);
        }
        return;
    }

    switch(event_type){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                gap_local_bd_addr(local_addr);
                printf("BTstack up and running on %s.\n", bd_addr_to_str(local_addr)); 
                // Start in server mode
                enter_server_mode(); 
            } else {
                miflora_client_set_state(FLORA_OFF); 
            }
            break;
            
        case GAP_EVENT_ADVERTISING_REPORT:
            // This is a client-role event
            miflora_client_handle_hci_event(packet_type, channel, packet, size); 
            break;
            
        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
                case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                    
                    if (miflora_client_get_state() == FLORA_W4_CONNECT) {
                        // This is the client connection *to* the MiFlora
                        miflora_client_handle_hci_event(packet_type, channel, packet, size); 
                    } 
                    else if (miflora_client_get_state() == FLORA_IDLE) 
                    {
                        // This is a server connection *to* us
                        btstack_run_loop_remove_timer(&server_advertisement_timer); 
                        ble_server_handle_hci_event(packet_type, channel, packet, size); 
                    }
                    else {
                        // We are in a busy state... ignore ghost/duplicate event 
                        printf("Ignoring duplicate connection event in state %d.\n", miflora_client_get_state()); 
                    }
                    break;
                default:
                    break;
            }
            break;
            
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            { 
                hci_con_handle_t disconnected_handle = hci_event_disconnection_complete_get_connection_handle(packet);
                
                if (ble_server_get_con_handle() == disconnected_handle){
                    ble_server_set_con_handle(HCI_CON_HANDLE_INVALID);
                    printf("Client disconnected from our server.\n"); 
                }
                
                if (miflora_client_get_con_handle() == disconnected_handle){
                    miflora_client_set_con_handle(HCI_CON_HANDLE_INVALID);
                    printf("Disconnected from MiFlora.\n"); 
                }
                
                // Only enter server mode if BOTH connections are invalid
                if (miflora_client_get_con_handle() == HCI_CON_HANDLE_INVALID && 
                    ble_server_get_con_handle() == HCI_CON_HANDLE_INVALID)
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
    if ((miflora_client_get_con_handle() != HCI_CON_HANDLE_INVALID || ble_server_get_con_handle() != HCI_CON_HANDLE_INVALID) && led_on) {
        quick_flash = !quick_flash; 
    } else if (miflora_client_get_con_handle() == HCI_CON_HANDLE_INVALID && ble_server_get_con_handle() == HCI_CON_HANDLE_INVALID) {
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
    
    // --- Initialize Modules ---
    miflora_client_init(target_mac_string);
    sd_logger_init();
    // -------------------------

    if (cyw43_arch_init()) {
        printf("failed to initialise cyw43_arch\n");
        return -1; 
    }

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    
    // Initialize BLE Server (which initializes ATT server)
    ble_server_init(hci_event_handler); 
    
    // Initialize BLE Client
    gatt_client_init(); 

    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration); 
    
    // Set up LED flashing timer
    heartbeat.process = &heartbeat_handler; 
    btstack_run_loop_set_timer(&heartbeat, LED_SLOW_FLASH_DELAY_MS);
    btstack_run_loop_add_timer(&heartbeat); 

    hci_power_control(HCI_POWER_ON);
    
    btstack_run_loop_execute();

    return 0; 
}