#include "sd_logger.h"
#include <stdio.h>
#include "hw_config.h" 
#include "f_util.h" 
#include "ff.h" 
#include "hardware/rtc.h" 
#include "pico/util/datetime.h" 

// --- SD Card Globals ---
static FATFS fs; 
static bool sd_mounted = false; 

bool sd_logger_init(void) {
    printf("Mounting SD card...\n");
    FRESULT fr = f_mount(&fs, "", 1); 
    if (FR_OK != fr) {
        printf("f_mount error: %s (%d)\n", FRESULT_str(fr), fr); 
        sd_mounted = false; 
    } else {
        printf("SD card mounted successfully.\n");
        sd_mounted = true; 
    }
    return sd_mounted;
}

void sd_logger_log_reading(miflora_reading_t *reading) {
    if (!sd_mounted) {
        printf("SD card not mounted. Skipping log.\n");
        return; 
    }

    // --- Get and format timestamp ---
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

    // --- Write timestamp + data as a CSV-like string ---
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