/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file RTC_Persistence_Adapter.c
 * @brief MyWota RTC Persistence Adapter - SD Card Implementation
 * @details Implements time persistence using SD card for MyWota project
 */

/* Includes ------------------------------------------------------------------*/
#include "RTC_Persistence_Adapter.h"
#include "SD_Logger_Task.h"
#include "USB_Logging.h"
#include "ff.h"
#include <string.h>

/* Configuration -------------------------------------------------------------*/
#define RTC_SAVE_FILE_PATH          "/time.dat"
#define RTC_SAVE_INTERVAL_MS        60000       /* Save every 1 minute */

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_RTC_PERSIST_EN    0

#if LOG_DEBUG_RTC_PERSIST_EN
    #define LOG_DEBUG_PERSIST(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_PERSIST(...)
#endif

/* Private Function Prototypes -----------------------------------------------*/
static RTC_Persist_Result_t rtc_save_to_sd(time_t current_time);
static RTC_Persist_Result_t rtc_load_from_sd(time_t* loaded_time);
static uint32_t rtc_get_save_interval(void);
static bool rtc_storage_is_ready(void);

/* Persistence Interface Definition ------------------------------------------*/
static const RTC_Persistence_Interface_t sd_persistence_interface = {
    .save = rtc_save_to_sd,
    .load = rtc_load_from_sd,
    .get_interval = rtc_get_save_interval,
    .is_ready = rtc_storage_is_ready,
    .storage_name = "SD Card"
};

/* Public Functions ----------------------------------------------------------*/

/**
 * @brief Initialize RTC persistence adapter
 */
RTC_Persist_Result_t RTC_Persistence_Adapter_Init(void)
{
    LOG_DEBUG_PERSIST("[RTC_PERSIST] Registering SD card persistence...\\r\\n");
    
    RTC_Persist_Result_t result = RTC_RegisterPersistence(&sd_persistence_interface);
    
    if (result == RTC_PERSIST_OK) {
        LOG_DEBUG_PERSIST("[RTC_PERSIST] ✓ SD card persistence registered\\r\\n");
    } else {
        USB_Log_Printf("[RTC_PERSIST] ✗ Failed to register persistence\\r\\n");
    }
    
    return result;
}

/* Private Functions ---------------------------------------------------------*/

/**
 * @brief Save current time to SD card
 * @param current_time Unix timestamp to save
 * @return RTC_PERSIST_OK on success
 */
static RTC_Persist_Result_t rtc_save_to_sd(time_t current_time)
{
    if (!SD_Logger_IsReady()) {
        return RTC_PERSIST_NOT_AVAILABLE;
    }
    
    /* Validate time (basic sanity check - must be after year 2020) */
    if (current_time < 1577836800) {  /* 2020-01-01 00:00:00 UTC */
        return RTC_PERSIST_INVALID_TIME;
    }
    
    /* Lock the file system safely */
    if (!SD_Logger_LockFS()) {
        return RTC_PERSIST_NOT_AVAILABLE;
    }
    
    /* Open file for writing */
    FIL file;
    FRESULT result = f_open(&file, RTC_SAVE_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) {
        SD_Logger_UnlockFS();
        LOG_DEBUG_PERSIST("[RTC_PERSIST] ✗ Failed to open file for write: %d\\r\\n", result);
        return RTC_PERSIST_ERROR;
    }
    
    /* Write Unix timestamp (8 bytes for 64-bit time_t) */
    UINT bytes_written;
    result = f_write(&file, &current_time, sizeof(time_t), &bytes_written);
    f_close(&file);
    
    SD_Logger_UnlockFS();
    
    if (result != FR_OK || bytes_written != sizeof(time_t)) {
        LOG_DEBUG_PERSIST("[RTC_PERSIST] ✗ Failed to write time: %d\\r\\n", result);
        return RTC_PERSIST_ERROR;
    }
    
    LOG_DEBUG_PERSIST("[RTC_PERSIST] ✓ Time saved: %lld\\r\\n", (long long)current_time);
    return RTC_PERSIST_OK;
}

/**
 * @brief Load time from SD card
 * @param loaded_time Pointer to store loaded Unix timestamp
 * @return RTC_PERSIST_OK on success
 */
static RTC_Persist_Result_t rtc_load_from_sd(time_t* loaded_time)
{
    if (!SD_Logger_IsReady()) {
        return RTC_PERSIST_NOT_AVAILABLE;
    }
    
    if (loaded_time == NULL) {
        return RTC_PERSIST_ERROR;
    }
    
    /* Lock the file system safely */
    if (!SD_Logger_LockFS()) {
        return RTC_PERSIST_NOT_AVAILABLE;
    }
    
    /* Open file for reading */
    FIL file;
    FRESULT result = f_open(&file, RTC_SAVE_FILE_PATH, FA_READ);
    if (result != FR_OK) {
        SD_Logger_UnlockFS();
        LOG_DEBUG_PERSIST("[RTC_PERSIST] No saved time file found\\r\\n");
        return RTC_PERSIST_NOT_AVAILABLE;
    }
    
    /* Read Unix timestamp */
    time_t saved_time;
    UINT bytes_read;
    result = f_read(&file, &saved_time, sizeof(time_t), &bytes_read);
    f_close(&file);
    
    SD_Logger_UnlockFS();
    
    if (result != FR_OK || bytes_read != sizeof(time_t)) {
        LOG_DEBUG_PERSIST("[RTC_PERSIST] ✗ Failed to read time: %d\\r\\n", result);
        return RTC_PERSIST_ERROR;
    }
    
    /* Validate loaded time */
    if (saved_time < 1577836800) {  /* Before 2020-01-01 */
        LOG_DEBUG_PERSIST("[RTC_PERSIST] ✗ Invalid time in file: %lld\\r\\n", (long long)saved_time);
        return RTC_PERSIST_INVALID_TIME;
    }
    
    *loaded_time = saved_time;
    LOG_DEBUG_PERSIST("[RTC_PERSIST] ✓ Time loaded: %lld\\r\\n", (long long)saved_time);
    return RTC_PERSIST_OK;
}

/**
 * @brief Get save interval (how often to persist time)
 * @return Interval in milliseconds
 */
static uint32_t rtc_get_save_interval(void)
{
    return RTC_SAVE_INTERVAL_MS;
}

/**
 * @brief Check if SD card storage is available
 * @return true if SD card is mounted and ready
 */
static bool rtc_storage_is_ready(void)
{
    return SD_Logger_IsReady();
}
