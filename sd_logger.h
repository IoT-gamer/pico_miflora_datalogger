#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <stdbool.h>
#include "miflora_client.h" // For miflora_reading_t

/**
 * @brief Initialize and mount the SD card.
 * @return true if mount was successful, false otherwise.
 */
bool sd_logger_init(void);

/**
 * @brief Log a MiFlora reading to the SD card.
 * @param reading Pointer to the reading data to log.
 */
void sd_logger_log_reading(miflora_reading_t *reading);

#endif // SD_LOGGER_H