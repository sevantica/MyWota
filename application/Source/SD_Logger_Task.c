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
#include "MIFARE_Transaction_Manager.h"
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

/**
 * @brief Check if SD logger is ready for logging operations
 * @return true if ready, false otherwise
 */
bool SD_Logger_IsReady(void)
{
    return sd_logger_context.filesystem_ready;
}

/**
 * @brief Log MIFARE card scan with all block data to SD card
 * @param card_uid Card unique identifier
 * @param uid_length Length of card UID (4 or 7 bytes)
 * @param card_data Pointer to MIFARE card data structure
 * @param log_type Type of log entry
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogMIFARECardScan(const uint8_t *card_uid, uint8_t uid_length, 
                                 const void *card_data, SDLogType_t log_type)
{
    if (!sd_logger_context.filesystem_ready || card_uid == NULL || card_data == NULL) {
        return false;
    }
    
    const MIFARE_CardData_t *mifare_data = (const MIFARE_CardData_t *)card_data;
    FIL file;
    FRESULT result;
    UINT bytes_written;
    char filename[64];
    char log_buffer[512];
    
    // Create filename based on card UID (e.g., "CARD_42680B06.log")
    snprintf(filename, sizeof(filename), "0:/CARD_");
    int offset = strlen(filename);
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        snprintf(filename + offset, sizeof(filename) - offset, "%02X", card_uid[i]);
        offset += 2;
    }
    snprintf(filename + offset, sizeof(filename) - offset, ".log");
    
    // Open/create card log file
    result = f_open(&file, filename, FA_OPEN_APPEND | FA_WRITE);
    if (result != FR_OK) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to open card log file: %d\r\n", result);
        return false;
    }
    
    // Get current tick count as timestamp
    uint32_t timestamp = xTaskGetTickCount();
    
    // Format log entry header
    int len = snprintf(log_buffer, sizeof(log_buffer),
                      "\r\n========================================\r\n"
                      "[%lu] MIFARE CARD SCAN - Log Type: %d\r\n"
                      "========================================\r\n",
                      timestamp, log_type);
    
    // Write header
    result = f_write(&file, log_buffer, len, &bytes_written);
    if (result != FR_OK) {
        f_close(&file);
        return false;
    }
    
    // Log Card Header (Block 4)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (HEADER):\r\n"
                  "  Magic: 0x%08lX\r\n"
                  "  Format Ver: %u\r\n"
                  "  Card Type: %u\r\n"
                  "  Header CRC: 0x%04X\r\n"
                  "  Serial: 0x%016llX\r\n",
                  MIFARE_BLOCK_HEADER,
                  mifare_data->header.magic_bytes,
                  mifare_data->header.format_version,
                  mifare_data->header.card_type,
                  mifare_data->header.header_crc,
                  (unsigned long long)mifare_data->header.card_serial);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log User Primary Data (Block 5)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (USER PRIMARY):\r\n"
                  "  Balance: %lu mL (%.2f L)\r\n"
                  "  Last Topup: %u mL\r\n"
                  "  Transaction Counter: %u\r\n"
                  "  Status Flags: 0x%02X\r\n"
                  "  Transaction State: 0x%02X\r\n",
                  MIFARE_BLOCK_USER_PRIMARY,
                  mifare_data->user_primary.balance_ml,
                  mifare_data->user_primary.balance_ml / 1000.0f,
                  mifare_data->user_primary.last_topup_amount_ml,
                  mifare_data->user_primary.transaction_counter,
                  mifare_data->user_primary.status_flags,
                  mifare_data->user_primary.transaction_state);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log User Backup Data (Block 6)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (USER BACKUP):\r\n"
                  "  Balance: %lu mL (%.2f L)\r\n"
                  "  Last Topup: %u mL\r\n"
                  "  Transaction Counter: %u\r\n"
                  "  Status Flags: 0x%02X\r\n"
                  "  Transaction State: 0x%02X\r\n",
                  MIFARE_BLOCK_USER_BACKUP,
                  mifare_data->user_backup.balance_ml,
                  mifare_data->user_backup.balance_ml / 1000.0f,
                  mifare_data->user_backup.last_topup_amount_ml,
                  mifare_data->user_backup.transaction_counter,
                  mifare_data->user_backup.status_flags,
                  mifare_data->user_backup.transaction_state);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Usage Data (Block 8)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (USAGE DATA):\r\n"
                  "  Total Purchased: %lu mL (%.2f L)\r\n"
                  "  Total Dispensed: %lu mL (%.2f L)\r\n",
                  MIFARE_BLOCK_USAGE_DATA,
                  mifare_data->usage_data.total_purchased_ml,
                  mifare_data->usage_data.total_purchased_ml / 1000.0f,
                  mifare_data->usage_data.total_dispensed_ml,
                  mifare_data->usage_data.total_dispensed_ml / 1000.0f);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Recovery Info (Block 12)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (RECOVERY INFO):\r\n"
                  "  Last Update Time: %lu\r\n"
                  "  Primary CRC: 0x%04X\r\n"
                  "  Backup CRC: 0x%04X\r\n"
                  "  Sequence Number: %u\r\n"
                  "  Recovery Attempts: %u\r\n"
                  "  Integrity Flags: 0x%04X\r\n",
                  MIFARE_BLOCK_RECOVERY_INFO,
                  mifare_data->recovery_info.last_update_time,
                  mifare_data->recovery_info.primary_data_crc,
                  mifare_data->recovery_info.backup_data_crc,
                  mifare_data->recovery_info.sequence_number,
                  mifare_data->recovery_info.recovery_attempts,
                  mifare_data->recovery_info.integrity_flags);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Fast Balance Primary (Block 13)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (FAST BALANCE PRIMARY):\r\n"
                  "  Balance: %lu mL (%.2f L)\r\n"
                  "  Sequence: %u\r\n"
                  "  Timestamp: %lu\r\n"
                  "  CRC32: 0x%08lX\r\n",
                  MIFARE_BLOCK_FAST_BALANCE_PRIMARY,
                  mifare_data->fast_balance_primary.balance_ml,
                  mifare_data->fast_balance_primary.balance_ml / 1000.0f,
                  mifare_data->fast_balance_primary.sequence_number,
                  mifare_data->fast_balance_primary.timestamp,
                  mifare_data->fast_balance_primary.crc32);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Fast Balance Backup (Block 14)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (FAST BALANCE BACKUP):\r\n"
                  "  Balance: %lu mL (%.2f L)\r\n"
                  "  Sequence: %u\r\n"
                  "  Timestamp: %lu\r\n"
                  "  CRC32: 0x%08lX\r\n",
                  MIFARE_BLOCK_FAST_BALANCE_BACKUP,
                  mifare_data->fast_balance_backup.balance_ml,
                  mifare_data->fast_balance_backup.balance_ml / 1000.0f,
                  mifare_data->fast_balance_backup.sequence_number,
                  mifare_data->fast_balance_backup.timestamp,
                  mifare_data->fast_balance_backup.crc32);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Account Data (Block 16)
    // Extract phone number from raw data
    char phone_str[12];
    memcpy(phone_str, &mifare_data->account_data.raw_data[ACCOUNT_DATA_PHONE_OFFSET], 
           ACCOUNT_DATA_PHONE_SIZE);
    phone_str[ACCOUNT_DATA_PHONE_SIZE] = '\0';
    
    uint8_t validity = mifare_data->account_data.raw_data[ACCOUNT_DATA_VALIDITY_OFFSET];
    uint16_t account_crc = (mifare_data->account_data.raw_data[ACCOUNT_DATA_CRC_OFFSET] << 8) |
                           mifare_data->account_data.raw_data[ACCOUNT_DATA_CRC_OFFSET + 1];
    
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (ACCOUNT DATA):\r\n"
                  "  Phone: %s\r\n"
                  "  Validity: %u\r\n"
                  "  CRC16: 0x%04X\r\n",
                  MIFARE_BLOCK_ACCOUNT_DATA,
                  phone_str,
                  validity,
                  account_crc);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Add footer
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "========================================\r\n\r\n");
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Close file
    f_close(&file);
    
    LOG_DEBUG_SD_LOGGER("[SD_LOGGER] MIFARE card scan logged: %s\r\n", filename);
    
    return true;
}

