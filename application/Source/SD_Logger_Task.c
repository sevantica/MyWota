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
#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include "USB_Logging.h"
#include "SD_Logger_Task.h"
#include "SD_SPI_Driver.h"
#include "MIFARE_Transaction_Manager.h"
#include "System_Config.h"
#include "System.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

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
static SemaphoreHandle_t sd_file_mutex = NULL;  // Mutex for SD file operations
static QueueHandle_t sd_log_queue = NULL;       // Queue for async log messages

/* State Machine Context */
typedef struct {
    SDLoggerState_t current_state;
    SDLoggerState_t previous_state;
    uint32_t state_entry_time;
    uint32_t time_in_state;
    uint32_t retry_count;
    bool filesystem_ready;
    bool suspended;                         // Suspended for USB MSC mode
    FATFS fatfs;                            // FAT filesystem object
} SDLoggerContext_t;

static SDLoggerContext_t sd_logger_context = {
    .current_state = SD_LOGGER_STATE_STARTUP,
    .previous_state = SD_LOGGER_STATE_STARTUP,
    .state_entry_time = 0,
    .time_in_state = 0,
    .retry_count = 0,
    .filesystem_ready = false,
    .suspended = false
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
        sd_file_mutex = xSemaphoreCreateMutex();
        if (sd_file_mutex == NULL) {
            LOG_ERROR_SD_LOGGER("[SD_LOGGER] Failed to create file mutex!\r\n");
        }
    }
    
    // Create queue for async log messages
    if (sd_log_queue == NULL) {
        sd_log_queue = xQueueCreate(SD_LOG_QUEUE_LENGTH, sizeof(SDLogQueueMsg_t));
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
        DWORD fre_clust, fre_sect, tot_sect;
        
        result = f_getfree("0:", &fre_clust, &fs);
        if (result == FR_OK) {
            tot_sect = (fs->n_fatent - 2) * fs->csize;
            fre_sect = fre_clust * fs->csize;
            
            LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Total: %lu KB, Free: %lu KB\r\n",
                           tot_sect / 2, fre_sect / 2);
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
    static bool error_notified = false;
    
    // Mark filesystem as not ready
    sd_logger_context.filesystem_ready = false;
    
    // Notify System task once when entering error state
    if (!error_notified) {
        extern TaskHandle_t task_get_handle_System_Task(void);
        TaskHandle_t system_handle = task_get_handle_System_Task();
        if (system_handle != NULL) {
            xTaskNotifyGive(system_handle);
            LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Notified System task of mount failure\r\n");
        }
        error_notified = true;
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
    
    // Get current tick count as timestamp
    uint32_t timestamp = xTaskGetTickCount();
    
    // Write boot header
    snprintf(log_buffer, sizeof(log_buffer), 
             "\r\n======== SYSTEM BOOT [%lu] ========\r\n", timestamp);
    f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    
    // Log SD Logger init
    snprintf(log_buffer, sizeof(log_buffer), 
             "[%lu] SD_Logger: Initialized\r\n", timestamp);
    f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    
    // Log config info from loaded configuration
    extern SystemConfig_t g_system_config;
    snprintf(log_buffer, sizeof(log_buffer), 
             "[%lu] Config: Device=%s Site=%s\r\n", 
             timestamp, g_system_config.system.device_id, g_system_config.system.site_id);
    f_write(&file, log_buffer, strlen(log_buffer), &bytes_written);
    
    snprintf(log_buffer, sizeof(log_buffer), 
             "[%lu] Mode: %s\r\n", 
             timestamp, g_system_config.system.test_mode_enabled ? "TEST" : "PRODUCTION");
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
    xTaskCreate(SD_Logger_Task, 
                "SD_Logger", 
                SD_LOGGER_TASK_STACK_WORDS, 
                NULL, 
                SD_LOGGER_TASK_PRIORITY, 
                &sd_logger_task_handle);
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
                  "  Balance: %lu ml\r\n"
                  "  Last Topup: %lu ml\r\n"
                  "  Transaction Counter: %u\r\n"
                  "  Status Flags: 0x%02X\r\n"
                  "  Transaction State: 0x%02X\r\n",
                  MIFARE_BLOCK_USER_PRIMARY,
                  mifare_data->user_primary.balance_ml,
                  mifare_data->user_primary.last_topup_ml,
                  mifare_data->user_primary.transaction_counter,
                  mifare_data->user_primary.status_flags,
                  mifare_data->user_primary.transaction_state);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log User Backup Data (Block 6)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (USER BACKUP):\r\n"
                  "  Balance: %lu ml\r\n"
                  "  Last Topup: %lu ml\r\n"
                  "  Transaction Counter: %u\r\n"
                  "  Status Flags: 0x%02X\r\n"
                  "  Transaction State: 0x%02X\r\n",
                  MIFARE_BLOCK_USER_BACKUP,
                  mifare_data->user_backup.balance_ml,
                  mifare_data->user_backup.last_topup_ml,
                  mifare_data->user_backup.transaction_counter,
                  mifare_data->user_backup.status_flags,
                  mifare_data->user_backup.transaction_state);
    f_write(&file, log_buffer, len, &bytes_written);
    
    // Log Usage Data (Block 8)
    len = snprintf(log_buffer, sizeof(log_buffer),
                  "BLOCK %d (USAGE DATA):\r\n"
                  "  Total Volume Purchased: %lu ml\r\n"
                  "  Total Dispenses Completed: %lu\r\n"
                  "  Total Volume Dispensed: %lu ml\r\n",
                  MIFARE_BLOCK_USAGE_DATA,
                  mifare_data->usage_data.total_volume_purchased_ml,
                  mifare_data->usage_data.total_dispenses_completed,
                  mifare_data->usage_data.total_volume_dispensed_ml);
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
    
    // Read and print file contents line by line
    char line_buffer[256];
    uint32_t line_count = 0;
    
    while (f_gets(line_buffer, sizeof(line_buffer), &file) != NULL) {
        line_count++;
        USB_Log_Printf("%s", line_buffer);  // f_gets includes newline
        
        // Report task health every 50 lines to prevent watchdog timeout
        if (line_count % 50 == 0) {
            System_ReportTaskStatus(SYSTEM_TASK_ID_SD_LOGGER, true);
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
 * @brief Recover card balance from SD card log file
 * @param card_uid Card unique identifier (4 or 7 bytes)
 * @param uid_length Length of card UID
 * @param balance_ml Pointer to store recovered balance (in milliliters)
 * @return true if balance was recovered successfully, false otherwise
 * @note Parses the card log file to find the latest balance entry (searches for "Balance:")
 */
bool SD_Logger_RecoverCardBalance(const uint8_t *card_uid, uint8_t uid_length, uint32_t *balance_ml)
{
    if (!sd_logger_context.filesystem_ready || card_uid == NULL || balance_ml == NULL) {
        USB_Log_Printf("[✗] SD card not ready or invalid parameters\r\n");
        return false;
    }
    
    // Try to acquire mutex with timeout
    if (sd_file_mutex == NULL || xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
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
        USB_Log_Printf("[✗] No log file found for card: %s\r\n", filename + 3);
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    USB_Log_Printf("[→] Searching log file: %s\r\n", filename + 3);
    
    // Read file and find the LAST occurrence of "Balance:" in USER PRIMARY section
    // We want the most recent balance logged
    char line_buffer[128];
    uint32_t last_balance = 0;
    bool found_balance = false;
    uint32_t line_count = 0;
    bool in_primary_section = false;
    
    while (f_gets(line_buffer, sizeof(line_buffer), &file) != NULL) {
        line_count++;
        
        // Track if we're in USER PRIMARY section (we want primary balance, not backup)
        if (strstr(line_buffer, "USER PRIMARY") != NULL) {
            in_primary_section = true;
        } else if (strstr(line_buffer, "USER BACKUP") != NULL || 
                   strstr(line_buffer, "USAGE DATA") != NULL ||
                   strstr(line_buffer, "ACCOUNT DATA") != NULL) {
            in_primary_section = false;
        }
        
        // Look for "Balance:" line within USER PRIMARY section
        if (in_primary_section) {
            const char *balance_ptr = strstr(line_buffer, "Balance:");
            if (balance_ptr != NULL) {
                // Parse the balance value - format is "  Balance: 12345 ml"
                balance_ptr += 8;  // Skip "Balance:"
                while (*balance_ptr == ' ') balance_ptr++;  // Skip whitespace
                
                uint32_t parsed_balance = (uint32_t)strtoul(balance_ptr, NULL, 10);
                if (parsed_balance > 0 || *balance_ptr == '0') {
                    last_balance = parsed_balance;
                    found_balance = true;
                }
            }
        }
        
        // Report task health every 100 lines
        if (line_count % 100 == 0) {
            System_ReportTaskStatus(SYSTEM_TASK_ID_SD_LOGGER, true);
        }
    }
    
    f_close(&file);
    xSemaphoreGive(sd_file_mutex);
    
    if (found_balance) {
        *balance_ml = last_balance;
        USB_Log_Printf("[✓] Found balance in log: %lu ml\r\n", last_balance);
        return true;
    } else {
        USB_Log_Printf("[✗] No balance entry found in log file\r\n");
        return false;
    }
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
    char log_buffer[140];
    
    // Open/create system log file
    result = f_open(&file, "0:/system_log.txt", FA_OPEN_APPEND | FA_WRITE);
    if (result != FR_OK) {
        xSemaphoreGive(sd_file_mutex);
        return false;
    }
    
    // Format with timestamp
    int len = snprintf(log_buffer, sizeof(log_buffer) - 2, "[%lu] %s", timestamp, message);
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
    if (!sd_logger_context.filesystem_ready || format == NULL || sd_log_queue == NULL) {
        return false;
    }
    
    SDLogQueueMsg_t log_msg;
    log_msg.timestamp = xTaskGetTickCount();
    
    // Format the message
    va_list args;
    va_start(args, format);
    int len = vsnprintf(log_msg.message, sizeof(log_msg.message), format, args);
    va_end(args);
    
    // Ensure null termination
    if (len < 0) len = 0;
    if (len >= (int)sizeof(log_msg.message)) {
        log_msg.message[sizeof(log_msg.message) - 1] = '\0';
    }
    
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
    
    // Get current tick count as timestamp
    uint32_t timestamp = xTaskGetTickCount();
    
    // Format transaction log entry
    int len = snprintf(log_buffer, sizeof(log_buffer),
                      "[%lu] TRANSACTION: %s | Before: %lu mL | After: %lu mL | Amount: %lu mL\r\n",
                      timestamp, event_type, balance_before, balance_after, amount);
    
    // Write to file
    result = f_write(&file, log_buffer, len, &bytes_written);
    
    // Close file
    f_close(&file);
    
    xSemaphoreGive(sd_file_mutex);
    
    // Also log to system log (this will acquire its own mutex)
    SD_Logger_LogEvent("TXN [%02X%02X%02X%02X]: %s | %lu->%lu mL",
                       card_uid[0], card_uid[1], card_uid[2], card_uid[3],
                       event_type, balance_before, balance_after);
    
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
    
    // Get current tick count as timestamp
    uint32_t timestamp = xTaskGetTickCount();
    
    // Format error log entry
    int len = snprintf(log_buffer, sizeof(log_buffer),
                      "[%lu] ERROR [%s] Code: %d - %s\r\n",
                      timestamp, 
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
    
    LOG_DEBUG_SD_LOGGER("[SD_LOGGER] Error logged: [%s] %d\r\n", module, error_code);
    
    return (result == FR_OK);
}

/* ========================================================================== */
/*                       USB MSC MODE SUPPORT FUNCTIONS                       */
/* ========================================================================== */

/**
 * @brief Suspend SD logging for USB MSC mode
 * @note Stops the logging task from writing to SD card
 */
void SD_Logger_SuspendLogging(void)
{
    LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Suspending logging for USB MSC mode\r\n");
    
    // Mark as suspended first
    sd_logger_context.suspended = true;
    sd_logger_context.filesystem_ready = false;
    
    // Wait for any pending file operations to complete
    if (sd_file_mutex != NULL) {
        if (xSemaphoreTake(sd_file_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            // Got mutex, release it - ensures no file ops in progress
            xSemaphoreGive(sd_file_mutex);
        }
    }
    
    LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Logging suspended\r\n");
}

/**
 * @brief Resume SD logging after USB MSC mode
 * @note Resumes the logging task after FatFs is remounted
 */
void SD_Logger_ResumeLogging(void)
{
    LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Resuming logging after USB MSC mode\r\n");
    
    // Clear suspended flag and reset state to remount
    sd_logger_context.suspended = false;
    sd_logger_context.retry_count = 0;
    
    // Force remount by going to MOUNT_FS state
    // The FatFs should already be mounted by USB_MSC_Disable()
    sd_logger_context.filesystem_ready = true;
    sd_logger_context.current_state = SD_LOGGER_STATE_READY;
    
    LOG_CRITICAL_SD_LOGGER("[SD_LOGGER] Logging resumed\r\n");
}

/**
 * @brief Get the FatFs object used by SD Logger
 * @return Pointer to FATFS object, or NULL if not mounted
 */
FATFS* SD_Logger_GetFatFs(void)
{
    if (sd_logger_context.filesystem_ready && !sd_logger_context.suspended) {
        return &sd_logger_context.fatfs;
    }
    return NULL;
}
