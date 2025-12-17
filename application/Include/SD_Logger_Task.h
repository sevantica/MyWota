/**
 ******************************************************************************
 * @file    SD_Logger_Task.h
 * @brief   SD Card Logger Task Header - State machine based initialization and logging
 * 
 * @attention
 * Copyright (c) Sevantica 2025
 * 
 ******************************************************************************
 */

#ifndef SD_LOGGER_TASK_H
#define SD_LOGGER_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

/* Exported types ------------------------------------------------------------*/

/**
 * @brief SD Log Entry Types
 */
typedef enum {
    SD_LOG_TYPE_SYSTEM_STARTUP,         // System startup event
    SD_LOG_TYPE_MIFARE_CARD_SCAN,       // MIFARE card detected and scanned
    SD_LOG_TYPE_TRANSACTION,            // Transaction event
    SD_LOG_TYPE_ERROR                   // Error event
} SDLogType_t;

/**
 * @brief SD Logger Task States
 */
typedef enum {
    SD_LOGGER_STATE_STARTUP,        // Initial power-on state
    SD_LOGGER_STATE_INIT_SD,        // Initializing SD card hardware
    SD_LOGGER_STATE_MOUNT_FS,       // Mounting FAT filesystem
    SD_LOGGER_STATE_READY,          // Ready for logging operations
    SD_LOGGER_STATE_ERROR,          // Error state
    SD_LOGGER_STATE_RETRY           // Retry initialization after error
} SDLoggerState_t;

/* Exported functions --------------------------------------------------------*/

/**
 * @brief Start SD Logger Task
 * @note Creates FreeRTOS task for SD card initialization and logging
 */
void Task_Start_SD_Logger_Task(void);

/**
 * @brief Log MIFARE card scan with all block data to SD card
 * @param card_uid Card unique identifier
 * @param uid_length Length of card UID (4 or 7 bytes)
 * @param card_data Pointer to MIFARE card data structure
 * @param log_type Type of log entry
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogMIFARECardScan(const uint8_t *card_uid, uint8_t uid_length, 
                                 const void *card_data, SDLogType_t log_type);

/**
 * @brief Check if SD logger is ready for logging operations
 * @return true if ready, false otherwise
 */
bool SD_Logger_IsReady(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_LOGGER_TASK_H */
