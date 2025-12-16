/**
 ******************************************************************************
 * @file    SD_Logger_Task.c
 * @brief   SD Card Logger Task Implementation - State machine based initialization and logging
 * 
 * @attention
 * Copyright (c) Sevantica 2025
 * 
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include "USB_Logging.h"
#include "SD_Logger_Task.h"
#include "SD_SPI_Driver.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>

/* Private defines -----------------------------------------------------------*/
#define SD_INIT_RETRY_DELAY_MS      5000    // Wait 5 seconds before retrying initialization
#define SD_MOUNT_RETRY_DELAY_MS     2000    // Wait 2 seconds before retrying mount
#define SD_MAX_RETRY_COUNT          5       // Maximum initialization attempts before giving up

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_SD_LOGGER_EN      1
#define LOG_CRITICAL_SD_LOGGER_EN   1
#define LOG_ERROR_SD_LOGGER_EN      1

#if LOG_DEBUG_SD_LOGGER_EN
    #define LOG_DEBUG_SD_LOGGER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_SD_LOGGER(...)
#endif

#if LOG_CRITICAL_SD_LOGGER_EN
    #define LOG_CRITICAL_SD_LOGGER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_SD_LOGGER(...)
#endif

#if LOG_ERROR_SD_LOGGER_EN
    #define LOG_ERROR_SD_LOGGER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_SD_LOGGER(...)
#endif

/* Private typedefs ----------------------------------------------------------*/
static TaskHandle_t SD_Logger_Task_TaskHandle = NULL;

/* State Machine Context */
typedef struct {
    SDLoggerState_t current_state;
    SDLoggerState_t previous_state;
    uint32_t state_entry_time;
    uint32_t time_in_state;
    uint32_t retry_count;
    bool filesystem_ready;
    FATFS fatfs;                            // FAT filesystem object
} SDLoggerContext_t;

static SDLoggerContext_t sd_logger_context = {
    .current_state = SD_LOGGER_STATE_STARTUP,
    .previous_state = SD_LOGGER_STATE_STARTUP,
    .state_entry_time = 0,
    .time_in_state = 0,
    .retry_count = 0,
    .filesystem_ready = false
};

/* Private function prototypes -----------------------------------------------*/
static void SD_Logger_Task(void* argument);
static void change_state(SDLoggerState_t new_state);

/* State machine function prototypes */
static void state_startup(void);
static void state_init_sd(void);
static void state_mount_fs(void);
static void state_ready(void);
static void state_error(void);
static void state_retry(void);

/* Helper function prototypes */
static FRESULT log_startup_event(void);

/* ========================================================================== */
/*                            MAIN TASK FUNCTION                              */
/* ========================================================================== */

static void SD_Logger_Task(void* argument)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    
    // Initialize state machine
    sd_logger_context.current_state = SD_LOGGER_STATE_STARTUP;
    sd_logger_context.state_entry_time = xTaskGetTickCount();
    
    LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Task started\r\n");
    
    for(;;)
    {
        TASK_HEARTBEAT_EVERY_SECOND("SD_Logger");
        
        // Update time in current state
        sd_logger_context.time_in_state = xTaskGetTickCount() - sd_logger_context.state_entry_time;
        
        // Execute current state
        switch (sd_logger_context.current_state) {
            case SD_LOGGER_STATE_STARTUP:
                state_startup();
                break;
            
            case SD_LOGGER_STATE_INIT_SD:
                state_init_sd();
                break;
            
            case SD_LOGGER_STATE_MOUNT_FS:
                state_mount_fs();
                break;
            
            case SD_LOGGER_STATE_READY:
                state_ready();
                break;
            
            case SD_LOGGER_STATE_ERROR:
                state_error();
                break;
            
            case SD_LOGGER_STATE_RETRY:
                state_retry();
                break;
            
            default:
                LOG_ERROR_SD_LOGGER("[SD_LOGGER] ERROR: Unknown state %d\r\n", sd_logger_context.current_state);
                change_state(SD_LOGGER_STATE_ERROR);
                break;
        }
        
        // Run every 100ms
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
    }
}

/* ========================================================================== */
/*                         STATE MACHINE IMPLEMENTATION                       */
/* ========================================================================== */

/**
 * @brief Change to a new state
 * @param new_state The state to transition to
 */
static void change_state(SDLoggerState_t new_state)
{
    if (sd_logger_context.current_state != new_state) {
        LOG_DEBUG_SD_LOGGER("[SD_LOGGER] State: %d -> %d (time: %lu ms)\r\n", 
                       sd_logger_context.current_state, new_state, sd_logger_context.time_in_state);
        
        sd_logger_context.previous_state = sd_logger_context.current_state;
        sd_logger_context.current_state = new_state;
        sd_logger_context.state_entry_time = xTaskGetTickCount();
        sd_logger_context.time_in_state = 0;
    }
}

/**
 * @brief STARTUP state - Wait for system to stabilize
 */
static void state_startup(void)
{
    /* Wait 1 second for other systems to initialize */
    if (sd_logger_context.time_in_state >= pdMS_TO_TICKS(1000)) {
        change_state(SD_LOGGER_STATE_INIT_SD);
    }
}

/**
 * @brief INIT_SD state - Initialize SD card hardware
 */
static void state_init_sd(void)
{
    LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Initializing SD card hardware (attempt %lu/%d)...\r\n", 
                   sd_logger_context.retry_count + 1, SD_MAX_RETRY_COUNT);
    
    SD_Status_t status = SD_Init();
    
    if (status == SD_OK) {
        LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] SD card initialized successfully\r\n");
        
        // Get card info
        SD_CardInfo_t card_info;
        if (SD_GetCardInfo(&card_info) == SD_OK) {
            LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Card Type: %d, Capacity: %lu MB\r\n", 
                           card_info.card_type, card_info.capacity_mb);
        }
        
        // Reset retry count and move to mount filesystem
        sd_logger_context.retry_count = 0;
        change_state(SD_LOGGER_STATE_MOUNT_FS);
    } else {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] SD card initialization failed: %s\r\n", SD_GetStatusString(status));
        sd_logger_context.retry_count++;
        
        if (sd_logger_context.retry_count >= SD_MAX_RETRY_COUNT) {
            LOG_ERROR_SD_LOGGER("[SD_LOGGER] Maximum retry count reached, entering error state\r\n");
            change_state(SD_LOGGER_STATE_ERROR);
        } else {
            change_state(SD_LOGGER_STATE_RETRY);
        }
    }
}

/**
 * @brief MOUNT_FS state - Mount FAT filesystem
 */
static void state_mount_fs(void)
{
    LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Mounting FAT filesystem...\r\n");
    
    // Mount the filesystem (drive 0)
    FRESULT result = f_mount(&sd_logger_context.fatfs, "0:", 1);
    
    if (result == FR_OK) {
        LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Filesystem mounted successfully\r\n");
        
        // Get filesystem info
        FATFS *fs;
        DWORD fre_clust, fre_sect, tot_sect;
        
        result = f_getfree("0:", &fre_clust, &fs);
        if (result == FR_OK) {
            tot_sect = (fs->n_fatent - 2) * fs->csize;
            fre_sect = fre_clust * fs->csize;
            
            LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Total: %lu KB, Free: %lu KB\r\n",
                           tot_sect / 2, fre_sect / 2);
        }
        
        // Log startup event
        log_startup_event();
        
        // Mark filesystem as ready
        sd_logger_context.filesystem_ready = true;
        sd_logger_context.retry_count = 0;
        change_state(SD_LOGGER_STATE_READY);
    } else {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] Filesystem mount failed: %d\r\n", result);
        sd_logger_context.retry_count++;
        
        if (sd_logger_context.retry_count >= SD_MAX_RETRY_COUNT) {
            LOG_ERROR_SD_LOGGER("[SD_LOGGER] Maximum retry count reached, entering error state\r\n");
            change_state(SD_LOGGER_STATE_ERROR);
        } else {
            change_state(SD_LOGGER_STATE_RETRY);
        }
    }
}

/**
 * @brief READY state - Filesystem ready for logging
 */
static void state_ready(void)
{
    // In ready state, just maintain heartbeat
    // Logging functions can be called from other tasks
    
    // Periodically check SD card status (every 10 seconds)
    if (sd_logger_context.time_in_state >= pdMS_TO_TICKS(10000)) {
        // Check if SD card is still present and responding
        if (!SD_IsReady()) {
            LOG_ERROR_SD_LOGGER("[SD_LOGGER] SD card no longer ready, reinitializing...\r\n");
            sd_logger_context.filesystem_ready = false;
            sd_logger_context.retry_count = 0;
            change_state(SD_LOGGER_STATE_INIT_SD);
        }
        // Reset timer for next check
        sd_logger_context.state_entry_time = xTaskGetTickCount();
    }
}

/**
 * @brief ERROR state - Handle error condition
 */
static void state_error(void)
{
    // Mark filesystem as not ready
    sd_logger_context.filesystem_ready = false;
    
    // Stay in error state indefinitely
    // Log error periodically (every 60 seconds)
    if (sd_logger_context.time_in_state >= pdMS_TO_TICKS(60000)) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] In error state - SD card initialization failed after %lu attempts\r\n",
                       SD_MAX_RETRY_COUNT);
        // Reset timer for next log
        sd_logger_context.state_entry_time = xTaskGetTickCount();
    }
}

/**
 * @brief RETRY state - Wait before retrying initialization
 */
static void state_retry(void)
{
    uint32_t retry_delay = (sd_logger_context.previous_state == SD_LOGGER_STATE_INIT_SD) 
                           ? SD_INIT_RETRY_DELAY_MS 
                           : SD_MOUNT_RETRY_DELAY_MS;
    
    if (sd_logger_context.time_in_state >= pdMS_TO_TICKS(retry_delay)) {
        LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Retrying initialization (attempt %lu/%d)...\r\n",
                       sd_logger_context.retry_count + 1, SD_MAX_RETRY_COUNT);
        
        // Return to previous initialization state
        if (sd_logger_context.previous_state == SD_LOGGER_STATE_INIT_SD) {
            change_state(SD_LOGGER_STATE_INIT_SD);
        } else {
            change_state(SD_LOGGER_STATE_MOUNT_FS);
        }
    }
}

/* ========================================================================== */
/*                            HELPER FUNCTIONS                                */
/* ========================================================================== */

/**
 * @brief Log system startup event to SD card
 */
static FRESULT log_startup_event(void)
{
    FIL file;
    FRESULT result;
    UINT bytes_written;
    char log_buffer[128];
    
    // Open/create system log file
    result = f_open(&file, "0:/system_log.txt", FA_OPEN_APPEND | FA_WRITE);
    if (result != FR_OK) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to open system log file: %d\r\n", result);
        return result;
    }
    
    // Get current tick count as timestamp
    uint32_t timestamp = xTaskGetTickCount();
    
    // Format log message
    snprintf(log_buffer, sizeof(log_buffer), 
             "[%lu] System startup - SD Logger initialized\r\n", timestamp);
    
    // Write to file
    result = f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    if (result != FR_OK || bytes_written != strlen(log_buffer)) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to write to system log: %d\r\n", result);
    }
    
    // Close file
    f_close(&file);
    
    if (result == FR_OK) {
        LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Startup event logged to SD card\r\n");
    }
    
    return result;
}

/* ========================================================================== */
/*                           EXPORTED FUNCTIONS                               */
/* ========================================================================== */

/**
 * @brief Start SD Logger Task
 */
void Task_Start_SD_Logger_Task(void)
{
    xTaskCreate(SD_Logger_Task, 
                "SD_Logger", 
                SD_LOGGER_TASK_STACK_WORDS, 
                NULL, 
                SD_LOGGER_TASK_PRIORITY, 
                &SD_Logger_Task_TaskHandle);
}
