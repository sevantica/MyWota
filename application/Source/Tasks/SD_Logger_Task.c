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
#include "queue.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "USB_Logging.h"
#include "SD_Logger_Task.h"
#include "SD_SPI_Driver.h"
#include "MIFARE_Transaction_Core.h"
#include "System_Config.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "RTC_Manager.h"
#include "RS485_Slave_Common_Handlers.h"

/* Private defines -----------------------------------------------------------*/
#define SD_INIT_RETRY_DELAY_MS      5000    // Wait 5 seconds before retrying initialization
#define SD_MOUNT_RETRY_DELAY_MS     2000    // Wait 2 seconds before retrying mount
#define SD_MAX_RETRY_COUNT          1       // Maximum initialization attempts before giving up

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_SD_LOGGER_EN      0
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
static TaskHandle_t sd_logger_task_handle = NULL;
static StaticTask_t sd_logger_task_tcb;
static StackType_t sd_logger_task_stack[SD_LOGGER_TASK_STACK_WORDS];

static SemaphoreHandle_t sd_file_mutex = NULL;  // Mutex for SD file operations
static StaticSemaphore_t sd_file_mutex_buffer;

static QueueHandle_t sd_log_queue = NULL;       // Queue for async log messages
static StaticQueue_t sd_log_queue_buffer;
static uint8_t sd_log_queue_storage[SD_LOG_QUEUE_LENGTH * sizeof(SDLogQueueMsg_t)];

/* State Machine Context */
typedef struct {
    SDLoggerState_t current_state;
    SDLoggerState_t previous_state;
    uint32_t state_entry_time;
    uint32_t time_in_state;
    uint32_t retry_count;
    bool filesystem_ready;
    bool error_notified;                    // Track if system task was notified of error
    FATFS fatfs;                            // FAT filesystem object
} SDLoggerContext_t;

static SDLoggerContext_t sd_logger_context = {
    .current_state = SD_LOGGER_STATE_STARTUP,
    .previous_state = SD_LOGGER_STATE_STARTUP,
    .state_entry_time = 0,
    .time_in_state = 0,
    .retry_count = 0,
    .filesystem_ready = false,
    .error_notified = false
};

/* Private function prototypes -----------------------------------------------*/
static void SD_Logger_Task(void* argument);
static void change_state(SDLoggerState_t new_state);
static void process_log_queue(void);
static bool write_log_to_file(const char *message, uint32_t timestamp);

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
    
    // Create mutex for SD file operations (if not already created)
    if (sd_file_mutex == NULL) {
        sd_file_mutex = xSemaphoreCreateMutexStatic(&sd_file_mutex_buffer);
        if (sd_file_mutex == NULL) {
            LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to create file mutex!\r\n");
        }
    }
    
    // Create queue for async log messages
    if (sd_log_queue == NULL) {
        sd_log_queue = xQueueCreateStatic(SD_LOG_QUEUE_LENGTH, sizeof(SDLogQueueMsg_t), sd_log_queue_storage, &sd_log_queue_buffer);
        if (sd_log_queue == NULL) {
            LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to create log queue!\r\n");
        }
    }
    
    // Initialize state machine
    sd_logger_context.current_state = SD_LOGGER_STATE_STARTUP;
    sd_logger_context.state_entry_time = xTaskGetTickCount();
    
    LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Task started\r\n");
    
    for(;;)
    {
        TASK_HEARTBEAT_EVERY_SECOND("SD_Logger");        System_ReportTaskStatus(SYSTEM_TASK_ID_SD_LOGGER, true);        
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
        
        /* Reset error_notified when leaving ERROR state so re-entry will re-notify */
        if (sd_logger_context.current_state == SD_LOGGER_STATE_ERROR) {
            sd_logger_context.error_notified = false;
        }
        
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
            LOG_CRITICAL_SD_LOGGER("[✗] SD Logger Task initialization FAILED\r\n");
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
        DWORD fre_clust; /* fre_sect, tot_sect; */
        
        result = f_getfree("0:", &fre_clust, &fs);
        if (result == FR_OK) {
            /* tot_sect = (fs->n_fatent - 2) * fs->csize; */
            /* fre_sect = fre_clust * fs->csize; */
            
            LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Free: %lu KB\r\n",
                           (DWORD)(fre_clust * fs->csize) / 2);
        }
        
        // Load system configuration from SD card
        LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Loading system configuration...\r\n");
        Config_Result_t cfg_result = Config_LoadFromMountedFS();
        if (cfg_result == CONFIG_FILE_NOT_FOUND) {
            LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Config file created with defaults\r\n");
        } else if (cfg_result != CONFIG_OK) {
            LOG_ERROR_SD_LOGGER("[SD_LOGGER] Config load failed (%d)\r\n", cfg_result);
        }
        
        // Log startup event
        log_startup_event();
        
        // Mark filesystem as ready
        sd_logger_context.filesystem_ready = true;
        sd_logger_context.retry_count = 0;
        LOG_CRITICAL_SD_LOGGER("[✓] SD Logger Task initialized successfully\r\n");
        
        // Notify System task that SD mounting is complete
        extern TaskHandle_t task_get_handle_System_Task(void);
        TaskHandle_t system_handle = task_get_handle_System_Task();
        if (system_handle != NULL) {
            xTaskNotifyGive(system_handle);
            LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Notified System task of mount success\r\n");
        }
        
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
    // Process any pending log messages from the queue
    process_log_queue();
    
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
    
    // Notify System task once when entering error state
    if (!sd_logger_context.error_notified) {
        extern TaskHandle_t task_get_handle_System_Task(void);
        TaskHandle_t system_handle = task_get_handle_System_Task();
        if (system_handle != NULL) {
            xTaskNotifyGive(system_handle);
            LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Notified System task of mount failure\r\n");
        }
        sd_logger_context.error_notified = true;
    }
    
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
    
    // Get current RTC time
    RTC_DateTime_t get_time;
    char time_str[32];
    uint32_t timestamp = xTaskGetTickCount();
    
    if (RTC_GetDateTime(&get_time) == RTC_OK && get_time.year > 2020) {
        snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
                 get_time.year, get_time.month, get_time.day,
                 get_time.hour, get_time.minute, get_time.second);
    } else {
        snprintf(time_str, sizeof(time_str), "%lu", timestamp);
    }
    
    // Write boot header
    snprintf(log_buffer, sizeof(log_buffer), 
             "\r\n======== SYSTEM BOOT [%s] ========\r\n", time_str);
    f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    
    // Log SD Logger init
    snprintf(log_buffer, sizeof(log_buffer), 
             "[%s] SD_Logger: Initialized\r\n", time_str);
    f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    
    // Log config info from loaded configuration
    const SystemConfig_t* sys_cfg = Config_Get();
    snprintf(log_buffer, sizeof(log_buffer), 
             "[%s] Config: Device=%s Site=%s\r\n", 
             time_str, sys_cfg->system.device_id, sys_cfg->system.site_id);
    f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    
    snprintf(log_buffer, sizeof(log_buffer), 
             "[%s] Mode: %s\r\n", 
             time_str, sys_cfg->system.test_mode_enabled ? "TEST" : "PRODUCTION");
    result = f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    
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
    sd_logger_task_handle = xTaskCreateStatic(SD_Logger_Task, 
                "SD_Logger", 
                SD_LOGGER_TASK_STACK_WORDS, 
                NULL, 
                SD_LOGGER_TASK_PRIORITY, 
                sd_logger_task_stack,
                &sd_logger_task_tcb);
}

TaskHandle_t SD_Logger_Task_GetHandle(void)
{
    return sd_logger_task_handle;
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
    
    // Try to acquire mutex with timeout
    if (sd_file_mutex == NULL || xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] LogMIFARECardScan: Failed to acquire mutex\r\n");
        return false;
    }
    
    const MIFARE_CardData_t *mifare_data = (const MIFARE_CardData_t *)card_data;
    FIL file;
    FRESULT result;
    UINT bytes_written;
    char filename[64];
    char log_buffer[256];  // Reduced from 512
    
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
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    // Get RTC time
    RTC_DateTime_t now;
    RTC_GetDateTime(&now);
    uint16_t ms = xTaskGetTickCount() % 1000;
    
    // Format log entry header with full timestamp for card logs
    int len = snprintf(log_buffer, sizeof(log_buffer),
                      "\r\n========================================\r\n"
                      "[%04d-%02d-%02d %02d:%02d:%02d:%03d] MIFARE CARD SCAN - Log Type: %d\r\n"
                      "========================================\r\n",
                      now.year, now.month, now.day, now.hour, now.minute, now.second, ms, log_type);
    
    // Write header
    result = f_write(&file, log_buffer, len, &bytes_written);
    if (result != FR_OK) {
        f_close(&file);
        xSemaphoreGive(sd_file_mutex);
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
                  "  Token Count: %lu tokens\r\n"
                  "  Last Topup: %lu tokens\r\n"
                  "  Transaction Counter: %u\r\n"
                  "  Status Flags: 0x%02X\r\n"
                  "  Transaction State: 0x%02X\r\n",
                  MIFARE_BLOCK_USER_PRIMARY,
                  mifare_data->user_primary.balance,
                  mifare_data->user_primary.last_topup,
                  mifare_data->user_primary.transaction_counter,
                  mifare_data->user_primary.status_flags,
                  mifare_data->user_primary.transaction_state);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log User Backup Data (Block 6)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (USER BACKUP):\r\n"
                  "  Token Count: %lu tokens\r\n"
                  "  Last Topup: %lu tokens\r\n"
                  "  Transaction Counter: %u\r\n"
                  "  Status Flags: 0x%02X\r\n"
                  "  Transaction State: 0x%02X\r\n",
                  MIFARE_BLOCK_USER_BACKUP,
                  mifare_data->user_backup.balance,
                  mifare_data->user_backup.last_topup,
                  mifare_data->user_backup.transaction_counter,
                  mifare_data->user_backup.status_flags,
                  mifare_data->user_backup.transaction_state);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Usage Data (Block 56)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (USAGE DATA):\r\n"
                  "  Total Tokens Purchased: %lu tokens\r\n"
                  "  Total Washes Completed: %lu washes\r\n"
                  "  Total Volume Dispensed: %lu ml\r\n",
                  MIFARE_BLOCK_USAGE_DATA,
                  mifare_data->usage_data.total_volume_purchased,
                  mifare_data->usage_data.total_dispenses_completed,
                  mifare_data->usage_data.total_volume_dispensed);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Loyalty Data (Block 53 for car wash)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (LOYALTY DATA):\r\n"
                  "  Loyalty Points: %u pts\r\n"
                  "  Free Credits: %u\r\n",
                  MIFARE_BLOCK_LOYALTY,
                  mifare_data->loyalty_data.loyalty_points,
                  mifare_data->loyalty_data.free_credits);
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
                  MIFARE_BLOCK_ACCOUNT_DATA,
                  mifare_data->recovery_info.last_update_time,
                  mifare_data->recovery_info.primary_data_crc,
                  mifare_data->recovery_info.backup_data_crc,
                  mifare_data->recovery_info.sequence_number,
                  mifare_data->recovery_info.recovery_attempts,
                  mifare_data->recovery_info.integrity_flags);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Token Cache Primary (Block 13)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (TOKEN CACHE PRIMARY):\r\n"
                  "  Token Count: %lu tokens\r\n"
                  "  Sequence: %u\r\n"
                  "  Timestamp: %lu\r\n"
                  "  CRC32: 0x%08lX\r\n",
                  MIFARE_BLOCK_USER_PRIMARY,
                  mifare_data->token_cache_primary.balance,
                  mifare_data->token_cache_primary.sequence_number,
                  mifare_data->token_cache_primary.timestamp,
                  mifare_data->token_cache_primary.crc32);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Token Cache Backup (Block 14)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (TOKEN CACHE BACKUP):\r\n"
                  "  Token Count: %lu tokens\r\n"
                  "  Sequence: %u\r\n"
                  "  Timestamp: %lu\r\n"
                  "  CRC32: 0x%08lX\r\n",
                  MIFARE_BLOCK_USER_BACKUP,
                  mifare_data->token_cache_backup.balance,
                  mifare_data->token_cache_backup.sequence_number,
                  mifare_data->token_cache_backup.timestamp,
                  mifare_data->token_cache_backup.crc32);
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
    
    xSemaphoreGive(sd_file_mutex);
    
    LOG_DEBUG_SD_LOGGER("[SD_LOGGER] MIFARE card scan logged: %s\r\n", filename);
    
    return true;
}

/**
 * @brief Print card transaction log to USB terminal
 * @param card_uid Card unique identifier
 * @param uid_length Length of card UID (4 or 7 bytes)
 * @return true if log was printed successfully, false otherwise
 */
bool SD_Logger_PrintCardLog(const uint8_t *card_uid, uint8_t uid_length)
{
    if (!sd_logger_context.filesystem_ready || card_uid == NULL) {
        USB_Log_Printf("[✗] SD card not ready or invalid card UID\r\n");
        return false;
    }
    
    // Try to acquire mutex with timeout
    if (sd_file_mutex == NULL || xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        USB_Log_Printf("[✗] Failed to acquire SD mutex\r\n");
        return false;
    }
    
    FIL file;
    FRESULT result;
    char filename[64];
    
    // Create filename based on card UID (e.g., "CARD_42680B06.log")
    snprintf(filename, sizeof(filename), "0:/CARD_");
    int offset = strlen(filename);
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        snprintf(filename + offset, sizeof(filename) - offset, "%02X", card_uid[i]);
        offset += 2;
    }
    snprintf(filename + offset, sizeof(filename) - offset, ".log");
    
    // Open card log file for reading
    result = f_open(&file, filename, FA_READ);
    if (result != FR_OK) {
        USB_Log_Printf("[✗] Failed to open card log file: %s (error: %d)\r\n", filename, result);
        if (result == FR_NO_FILE) {
            USB_Log_Printf("[→] No log file exists for this card\r\n");
        }
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    USB_Log_Printf("\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("            CARD TRANSACTION LOG: %s\r\n", filename + 3);  // Skip "0:/"
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    
    /* Check file size */
    FSIZE_t file_size = f_size(&file);
    if (file_size > 8192) {
        FSIZE_t seek_pos = file_size - 8192;
        USB_Log_Printf("... (skipping first %lu bytes, showing last 8KB)\r\n", (unsigned long)seek_pos);
        
        f_lseek(&file, seek_pos);
        
        /* Discard the first line as it might be partial/incomplete after random seek */
        char discard_buffer[256];
        f_gets(discard_buffer, sizeof(discard_buffer), &file);
    }

    // Read and print file contents line by line
    char line_buffer[256];
    uint32_t line_count = 0;
    
    while (f_gets(line_buffer, sizeof(line_buffer), &file) != NULL) {
        line_count++;
        USB_Log_Printf("%s", line_buffer);  // f_gets includes newline
        
        // Report task health every 50 lines to prevent watchdog timeout
        if (line_count % 50 == 0) {
            System_ReportTaskStatus(SYSTEM_TASK_ID_USB_COMMAND_HANDLER, true);
        }
    }
    
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("Total: %lu lines\r\n", line_count);
    USB_Log_Printf("\r\n");
    
    // Close file
    f_close(&file);
    xSemaphoreGive(sd_file_mutex);
    
    return true;
}

/**
 * @brief Process pending log messages from the queue (called from SD Logger task context)
 */
static void process_log_queue(void)
{
    SDLogQueueMsg_t log_msg;
    
    // Process up to 4 messages per iteration to avoid hogging CPU
    int processed = 0;
    while (processed < 4 && sd_log_queue != NULL && 
           xQueueReceive(sd_log_queue, &log_msg, 0) == pdTRUE) {
        write_log_to_file(log_msg.message, log_msg.timestamp);
        processed++;
    }
}

/**
 * @brief Write a log message to the system log file (internal, runs in SD Logger task context)
 * @param message Pre-formatted message to write
 * @param timestamp Tick count when message was queued
 * @return true if written successfully
 */
static bool write_log_to_file(const char *message, uint32_t timestamp)
{
    if (!sd_logger_context.filesystem_ready || message == NULL) {
        return false;
    }
    
    // Acquire mutex (we're in SD Logger task, so this should not cause priority inversion)
    if (sd_file_mutex == NULL || xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] write_log_to_file: Failed to acquire mutex\r\n");
        return false;
    }
    
    FIL file;
    FRESULT result;
    UINT bytes_written;
    char log_buffer[160];
    static RTC_DateTime_t last_log_date = {0};
    
    // Open/create system log file
    result = f_open(&file, "0:/system_log.txt", FA_OPEN_APPEND | FA_WRITE);
    if (result != FR_OK) {
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    // Get current RTC time
    RTC_DateTime_t now;
    RTC_GetDateTime(&now);
    uint16_t ms = xTaskGetTickCount() % 1000;
    
    // Check if date changed (or first run)
    if (now.year != last_log_date.year || 
        now.month != last_log_date.month || 
        now.day != last_log_date.day) {
        
        // Write date header
        int header_len = snprintf(log_buffer, sizeof(log_buffer), 
                                "\r\nDate: %02d-%02d-%04d\r\n", 
                                now.day, now.month, now.year);
        f_write(&file, log_buffer, header_len, &bytes_written);
        
        // Update last log date
        last_log_date = now;
    }
    
    // Format with RTC timestamp [HH:MM:SS:ms]
    int len = snprintf(log_buffer, sizeof(log_buffer) - 2, "[%02d:%02d:%02d:%03d] %s", 
                      now.hour, now.minute, now.second, ms, message);
    if (len < 0) len = 0;
    if (len > (int)sizeof(log_buffer) - 3) len = sizeof(log_buffer) - 3;
    log_buffer[len++] = '\r';
    log_buffer[len++] = '\n';
    
    result = f_write(&file, log_buffer, len, &bytes_written);
    f_close(&file);
    xSemaphoreGive(sd_file_mutex);
    
    return (result == FR_OK);
}

/**
 * @brief Log a system event to the system log file (ASYNC - queues message for SD Logger task)
 * @param format Printf-style format string
 * @param ... Variable arguments
 * @return true if queued successfully, false otherwise
 * @note This function is now non-blocking and safe to call from any task priority
 */
bool SD_Logger_LogEvent(const char *format, ...)
{
    if (format == NULL) {
        return false;
    }
    
    char log_buffer[SD_LOG_MSG_MAX_LEN];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    
    // Ensure null termination
    if (len < 0) len = 0;
    if (len >= (int)sizeof(log_buffer)) {
        log_buffer[sizeof(log_buffer) - 1] = '\0';
    }
    
    // Always mirror to USB Log for visibility (especially if RS485 is listening)
    USB_Log_Printf("[SD] %s\r\n", log_buffer);
    
    if (!sd_logger_context.filesystem_ready || sd_log_queue == NULL) {
        return false;
    }
    
    SDLogQueueMsg_t log_msg;
    log_msg.timestamp = xTaskGetTickCount();
    strncpy(log_msg.message, log_buffer, sizeof(log_msg.message) - 1);
    log_msg.message[sizeof(log_msg.message) - 1] = '\0';
    
    // Queue the message (non-blocking)
    if (xQueueSend(sd_log_queue, &log_msg, 0) != pdTRUE) {
        // Queue full - drop the message
        LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Log queue full, message dropped\r\n");
        return false;
    }
    
    return true;
}

/**
 * @brief Log a transaction event (start, progress, complete, abort)
 * @param card_uid Card unique identifier
 * @param uid_length Length of card UID
 * @param event_type Event description string
 * @param balance_before Balance before event (in mL)
 * @param balance_after Balance after event (in mL)
 * @param amount Amount involved in transaction (in mL)
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogTransaction(const uint8_t *card_uid, uint8_t uid_length,
                              const char *event_type,
                              uint32_t balance_before, uint32_t balance_after,
                              uint32_t amount)
{
    const char* unit = MIFARE_GetBalanceUnit();

    // Always log to console for immediate visibility
    USB_Log_Printf("[SD TXN] [%02X%02X%02X%02X] %s | %lu -> %lu %s (Amt: %lu)\r\n",
                   card_uid ? card_uid[0] : 0, card_uid ? card_uid[1] : 0,
                   card_uid ? card_uid[2] : 0, card_uid ? card_uid[3] : 0,
                   event_type ? event_type : "?",
                   balance_before, balance_after, unit, amount);

    /* Queue for upload to RS485 master (best-effort; ignored if no master). */
    if (card_uid != NULL) {
        RS485_Transaction_Record_t rec = {0};
        rec.timestamp = (uint32_t)RTC_GetUnixTime();
        rec.transaction_id = 0; /* Auto-allocated by queue */
        uint8_t copy_len = uid_length;
        if (copy_len > sizeof(rec.card_uid)) copy_len = sizeof(rec.card_uid);
        memcpy(rec.card_uid, card_uid, copy_len);
        rec.card_uid_length = copy_len;
        rec.transaction_type = 0; /* Project-specific code (0 = generic) */
        rec.balance_before = balance_before;
        rec.balance_after = balance_after;
        rec.amount = (int32_t)balance_after - (int32_t)balance_before;
        (void)RS485_SlaveCommon_TxnQueue_Push(&rec);
    }

    if (!sd_logger_context.filesystem_ready || card_uid == NULL || event_type == NULL) {
        return false;
    }
    
    // Try to acquire mutex with timeout
    if (sd_file_mutex == NULL || xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] LogTransaction: Failed to acquire mutex\r\n");
        return false;
    }
    
    FIL file;
    FRESULT result;
    UINT bytes_written;
    char filename[64];
    char log_buffer[160];  // Reduced from 256
    
    // Create filename based on card UID
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
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to open transaction log: %d\r\n", result);
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    // Get RTC time
    RTC_DateTime_t now;
    RTC_GetDateTime(&now);
    uint16_t ms = xTaskGetTickCount() % 1000;
    
    // Format transaction log entry
    // Format transaction log entry with full timestamp for card logs
    int len = snprintf(log_buffer, sizeof(log_buffer),
                      "[%04d-%02d-%02d %02d:%02d:%02d:%03d] TRANSACTION: %s | Before: %lu %s | After: %lu %s | Amount: %lu %s\r\n",
                      now.year, now.month, now.day, now.hour, now.minute, now.second, ms, 
                      event_type, balance_before, unit, balance_after, unit, amount, unit);
    
    // Write to file
    result = f_write(&file, log_buffer, len, &bytes_written);
    
    // Close file
    f_close(&file);
    
    xSemaphoreGive(sd_file_mutex);
    
    // Also log to system log (this will acquire its own mutex)
    SD_Logger_LogEvent("TXN [%02X%02X%02X%02X]: %s | %lu->%lu %s",
                       card_uid[0], card_uid[1], card_uid[2], card_uid[3],
                       event_type, balance_before, balance_after, unit);
    
    LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Transaction logged: %s - %s\r\n", filename, event_type);
    
    return (result == FR_OK);
}

/**
 * @brief Log an error event
 * @param module Module name where error occurred
 * @param error_code Error code
 * @param description Error description
 * @return true if logged successfully, false otherwise
 */
bool SD_Logger_LogError(const char *module, int error_code, const char *description)
{
    // Always log to console for immediate visibility
    USB_Log_Printf("[SD ERR] [%s] %d: %s\r\n", 
                   module ? module : "?", 
                   error_code, 
                   description ? description : "");

    if (!sd_logger_context.filesystem_ready) {
        return false;
    }
    
    // Try to acquire mutex with timeout
    if (sd_file_mutex == NULL || xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] LogError: Failed to acquire mutex\r\n");
        return false;
    }
    
    FIL file;
    FRESULT result;
    UINT bytes_written;
    char log_buffer[160];  // Reduced from 256
    
    // Open/create error log file
    result = f_open(&file, "0:/error_log.txt", FA_OPEN_APPEND | FA_WRITE);
    if (result != FR_OK) {
        LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to open error log: %d\r\n", result);
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    // Get RTC time
    RTC_DateTime_t now;
    RTC_GetDateTime(&now);
    uint16_t ms = xTaskGetTickCount() % 1000;
    
    // Format error log entry with full timestamp
    int len = snprintf(log_buffer, sizeof(log_buffer),
                      "[%04d-%02d-%02d %02d:%02d:%02d:%03d] ERROR [%s] Code: %d - %s\r\n",
                      now.year, now.month, now.day, now.hour, now.minute, now.second, ms, 
                      module ? module : "UNKNOWN",
                      error_code,
                      description ? description : "No description");
    
    // Write to file
    result = f_write(&file, log_buffer, len, &bytes_written);
    
    // Close file
    f_close(&file);
    
    xSemaphoreGive(sd_file_mutex);
    
    // Also log to system log (this will acquire its own mutex)
    SD_Logger_LogEvent("ERR [%s] %d: %s", 
                       module ? module : "?", 
                       error_code, 
                       description ? description : "");
    
    LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Error logged: %s - %d\r\n", module, error_code);
    
    return (result == FR_OK);
}

/**
 * @brief Get last known balance from SD card transaction logs
 * @param card_uid Card unique identifier  
 * @param uid_length Length of card UID (4 or 7 bytes)
 * @param balance_out Pointer to store retrieved balance
 * @return true if balance found, false otherwise
 */
bool SD_Logger_GetLastBalance(const uint8_t *card_uid, uint8_t uid_length, uint32_t *balance_out)
{
    if (!sd_logger_context.filesystem_ready || card_uid == NULL || balance_out == NULL) {
        return false;
    }
    
    // Try to acquire mutex with timeout
    if (sd_file_mutex == NULL || xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    
    FIL file;
    FRESULT result;
    char filename[64];
    char line_buffer[256];
    
    // Create filename based on card UID
    snprintf(filename, sizeof(filename), "0:/CARD_");
    int offset = strlen(filename);
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        snprintf(filename + offset, sizeof(filename) - offset, "%02X", card_uid[i]);
        offset += 2;
    }
    snprintf(filename + offset, sizeof(filename) - offset, ".log");
    
    // Open card log file for reading
    result = f_open(&file, filename, FA_READ);
    if (result != FR_OK) {
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    // Scan file backwards for last transaction entry
    uint32_t last_balance = 0;
    bool found = false;
    
    while (f_gets(line_buffer, sizeof(line_buffer), &file)) {
        // Look for transaction lines: "[timestamp] TRANSACTION: ... | After: XXX mL ..."
        char *after_marker = strstr(line_buffer, "After: ");
        if (after_marker) {
            uint32_t balance = 0;
            if (sscanf(after_marker + 7, "%lu", &balance) == 1) {
                last_balance = balance;
                found = true;
            }
        }
    }
    
    f_close(&file);
    xSemaphoreGive(sd_file_mutex);
    
    if (found) {
        *balance_out = last_balance;
    }
    
    return found;
}
