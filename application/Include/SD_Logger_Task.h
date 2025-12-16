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

#ifdef __cplusplus
}
#endif

#endif /* SD_LOGGER_TASK_H */
