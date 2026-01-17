/**
 ******************************************************************************
 * @file    RTC_Manager.c
 * @brief   Real-Time Clock Manager Implementation
 * 
 * @attention
 * Copyright (c) Sevantica 2026
 * 
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "RTC_Manager.h"
#include "Firmware_Version.h"
#include "USB_Logging.h"
#include "SD_Logger_Task.h"
#include "ff.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_RTC_EN        0
#define LOG_CRITICAL_RTC_EN     1
#define LOG_ERROR_RTC_EN        1

#if LOG_DEBUG_RTC_EN
    #define LOG_DEBUG_RTC(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_RTC(...)
#endif

#if LOG_CRITICAL_RTC_EN
    #define LOG_CRITICAL_RTC(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_RTC(...)
#endif

#if LOG_ERROR_RTC_EN
    #define LOG_ERROR_RTC(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_RTC(...)
#endif

/* Private Variables ---------------------------------------------------------*/
static bool rtc_initialized = false;
static bool rtc_synchronized = false;  /* Set true when updated via RS485/USB */
static time_t compile_time_unix = 0;
static time_t current_time_unix = 0;   /* Current software RTC time */
static TickType_t last_tick = 0;        /* Last FreeRTOS tick count */

/* Private Function Prototypes -----------------------------------------------*/
static time_t parse_compile_time(void);
static time_t datetime_to_unix(const RTC_DateTime_t *dt);
static void unix_to_datetime(time_t unix_time, RTC_DateTime_t *dt);
static bool is_time_valid(time_t unix_time);

/* ============================================================================
 * Initialization
 * ============================================================================ */

RTC_Status_t RTC_Init(void)
{
    if (rtc_initialized) {
        return RTC_OK;
    }
    
    LOG_CRITICAL_RTC("[RTC] Initializing RTC manager...\r\n");
    
    // Calculate compile time as fallback
    compile_time_unix = parse_compile_time();
    
    RTC_DateTime_t compile_dt;
    unix_to_datetime(compile_time_unix, &compile_dt);
    LOG_CRITICAL_RTC("[RTC] Compile time: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                     compile_dt.year, compile_dt.month, compile_dt.day,
                     compile_dt.hour, compile_dt.minute, compile_dt.second);
    
    // Try to load time from SD card
    RTC_Status_t status = RTC_LoadFromSD();
    if (status == RTC_OK) {
        LOG_CRITICAL_RTC("[RTC] ✓ Time loaded from SD card\r\n");
    } else {
        // Fall back to compile time
        LOG_CRITICAL_RTC("[RTC] → Using compile time as fallback\r\n");
        current_time_unix = compile_time_unix;
        last_tick = xTaskGetTickCount();
    }
    
    rtc_initialized = true;
    LOG_CRITICAL_RTC("[RTC] ✓ RTC manager initialized\r\n");
    
    return RTC_OK;
}

/* ============================================================================
 * Public API - Getters
 * ============================================================================ */

RTC_Status_t RTC_GetDateTime(RTC_DateTime_t *datetime)
{
    if (!rtc_initialized) {
        return RTC_ERROR_NOT_INITIALIZED;
    }
    
    if (datetime == NULL) {
        return RTC_ERROR_INVALID_PARAM;
    }
    
    // Update current time based on elapsed ticks
    TickType_t current_tick = xTaskGetTickCount();
    TickType_t elapsed_ticks = current_tick - last_tick;
    time_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;
    
    time_t current_time = current_time_unix + elapsed_seconds;
    
    // Convert to datetime structure
    unix_to_datetime(current_time, datetime);
    
    return RTC_OK;
}

time_t RTC_GetUnixTime(void)
{
    if (!rtc_initialized) {
        return 0;
    }
    
    // Update current time based on elapsed ticks
    TickType_t current_tick = xTaskGetTickCount();
    TickType_t elapsed_ticks = current_tick - last_tick;
    time_t elapsed_seconds = pdTICKS_TO_MS(elapsed_ticks) / 1000;
    
    return current_time_unix + elapsed_seconds;
}

bool RTC_IsSynchronized(void)
{
    return rtc_synchronized;
}

time_t RTC_GetCompileTime(void)
{
    return compile_time_unix;
}

/* ============================================================================
 * Public API - Setters
 * ============================================================================ */

RTC_Status_t RTC_SetDateTime(const RTC_DateTime_t *datetime)
{
    if (!rtc_initialized) {
        return RTC_ERROR_NOT_INITIALIZED;
    }
    
    if (datetime == NULL) {
        return RTC_ERROR_INVALID_PARAM;
    }
    
    // Convert to Unix time for validation
    time_t unix_time = datetime_to_unix(datetime);
    
    // Validate: must not be before compile time
    if (!is_time_valid(unix_time)) {
        LOG_ERROR_RTC("[RTC] ✗ Rejected time before compile time\r\n");
        return RTC_ERROR_TIME_BEFORE_COMPILE;
    }
    
    // Set software RTC time
    current_time_unix = unix_time;
    last_tick = xTaskGetTickCount();
    
    rtc_synchronized = true;
    LOG_DEBUG_RTC("[RTC] ✓ Time updated: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                  datetime->year, datetime->month, datetime->day,
                  datetime->hour, datetime->minute, datetime->second);
    
    return RTC_OK;
}

RTC_Status_t RTC_SetUnixTime(time_t unix_time)
{
    if (!is_time_valid(unix_time)) {
        return RTC_ERROR_TIME_BEFORE_COMPILE;
    }
    
    RTC_DateTime_t dt;
    unix_to_datetime(unix_time, &dt);
    
    return RTC_SetDateTime(&dt);
}

/* ============================================================================
 * SD Card Persistence
 * ============================================================================ */

RTC_Status_t RTC_SaveToSD(void)
{
    if (!rtc_initialized) {
        return RTC_ERROR_NOT_INITIALIZED;
    }
    
    // Check if SD card is mounted
    if (!SD_Logger_IsReady()) {
        return RTC_ERROR_SD_WRITE;
    }
    
    // Get current time
    time_t current_time = RTC_GetUnixTime();
    if (current_time == 0) {
        return RTC_ERROR;
    }
    
    // Open/create RTC file
    static FIL file;
    FRESULT result = f_open(&file, RTC_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) {
        LOG_ERROR_RTC("[RTC] ✗ Failed to open RTC file for write: %d\r\n", result);
        return RTC_ERROR_SD_WRITE;
    }
    
    // Write Unix timestamp (8 bytes for time_t as 64-bit)
    UINT bytes_written;
    result = f_write(&file, &current_time, sizeof(time_t), &bytes_written);
    f_close(&file);
    
    if (result != FR_OK || bytes_written != sizeof(time_t)) {
        LOG_ERROR_RTC("[RTC] ✗ Failed to write RTC file: %d\r\n", result);
        return RTC_ERROR_SD_WRITE;
    }
    
    LOG_DEBUG_RTC("[RTC] ✓ Time saved to SD card\r\n");
    return RTC_OK;
}

RTC_Status_t RTC_LoadFromSD(void)
{
    // Check if SD card is mounted
    if (!SD_Logger_IsReady()) {
        return RTC_ERROR_SD_READ;
    }
    
    // Open RTC file
    static FIL file;
    FRESULT result = f_open(&file, RTC_FILE_PATH, FA_READ);
    if (result != FR_OK) {
        LOG_DEBUG_RTC("[RTC] No saved time found on SD card\r\n");
        return RTC_ERROR_SD_READ;
    }
    
    // Read Unix timestamp
    time_t saved_time;
    UINT bytes_read;
    result = f_read(&file, &saved_time, sizeof(time_t), &bytes_read);
    f_close(&file);
    
    if (result != FR_OK || bytes_read != sizeof(time_t)) {
        LOG_ERROR_RTC("[RTC] ✗ Failed to read RTC file: %d\r\n", result);
        return RTC_ERROR_SD_READ;
    }
    
    // Validate time
    if (!is_time_valid(saved_time)) {
        LOG_ERROR_RTC("[RTC] ✗ Invalid time in SD file (before compile time)\r\n");
        return RTC_ERROR_TIME_BEFORE_COMPILE;
    }
    
    // Set software RTC time
    current_time_unix = saved_time;
    last_tick = xTaskGetTickCount();
    
    return RTC_OK;
}

/* ============================================================================
 * Formatting
 * ============================================================================ */

int RTC_FormatDateTime(const RTC_DateTime_t *datetime, char *buffer, size_t buffer_size)
{
    if (datetime == NULL || buffer == NULL || buffer_size == 0) {
        return 0;
    }
    
    return snprintf(buffer, buffer_size, "%04d-%02d-%02d %02d:%02d:%02d",
                    datetime->year, datetime->month, datetime->day,
                    datetime->hour, datetime->minute, datetime->second);
}

int RTC_FormatUnixTime(time_t unix_time, char *buffer, size_t buffer_size)
{
    RTC_DateTime_t dt;
    unix_to_datetime(unix_time, &dt);
    return RTC_FormatDateTime(&dt, buffer, buffer_size);
}

/* ============================================================================
 * Status Strings
 * ============================================================================ */

const char* RTC_GetStatusString(RTC_Status_t status)
{
    switch (status) {
        case RTC_OK:                        return "OK";
        case RTC_ERROR:                     return "Error";
        case RTC_ERROR_INVALID_PARAM:       return "Invalid Parameter";
        case RTC_ERROR_NOT_INITIALIZED:     return "Not Initialized";
        case RTC_ERROR_TIME_BEFORE_COMPILE: return "Time Before Compile Time";
        case RTC_ERROR_SD_WRITE:            return "SD Write Error";
        case RTC_ERROR_SD_READ:             return "SD Read Error";
        default:                            return "Unknown Error";
    }
}

/* ============================================================================
 * Private Helper Functions
 * ============================================================================ */

/**
 * @brief Parse compile date/time into Unix timestamp
 */
static time_t parse_compile_time(void)
{
    struct tm tm_compile = {0};
    
    // Parse __DATE__ (format: "Jan  9 2026")
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    
    char month_str[4];
    int day, year;
    sscanf(FW_BUILD_DATE, "%s %d %d", month_str, &day, &year);
    
    // Find month index
    for (int i = 0; i < 12; i++) {
        if (strcmp(month_str, months[i]) == 0) {
            tm_compile.tm_mon = i;
            break;
        }
    }
    
    tm_compile.tm_mday = day;
    tm_compile.tm_year = year - 1900;  // tm_year is years since 1900
    
    // Parse __TIME__ (format: "HH:MM:SS")
    int hour, min, sec;
    sscanf(FW_BUILD_TIME, "%d:%d:%d", &hour, &min, &sec);
    tm_compile.tm_hour = hour;
    tm_compile.tm_min = min;
    tm_compile.tm_sec = sec;
    
    // Convert to Unix timestamp
    return mktime(&tm_compile);
}

/**
 * @brief Convert RTC_DateTime_t to Unix timestamp
 */
static time_t datetime_to_unix(const RTC_DateTime_t *dt)
{
    struct tm tm_time = {0};
    tm_time.tm_year = dt->year - 1900;
    tm_time.tm_mon = dt->month - 1;
    tm_time.tm_mday = dt->day;
    tm_time.tm_hour = dt->hour;
    tm_time.tm_min = dt->minute;
    tm_time.tm_sec = dt->second;
    
    return mktime(&tm_time);
}

/**
 * @brief Convert Unix timestamp to RTC_DateTime_t
 */
static void unix_to_datetime(time_t unix_time, RTC_DateTime_t *dt)
{
    struct tm tm_time;
    localtime_r(&unix_time, &tm_time);  // Thread-safe version
    
    dt->year = tm_time.tm_year + 1900;
    dt->month = tm_time.tm_mon + 1;
    dt->day = tm_time.tm_mday;
    dt->hour = tm_time.tm_hour;
    dt->minute = tm_time.tm_min;
    dt->second = tm_time.tm_sec;
    dt->weekday = tm_time.tm_wday;
}

/**
 * @brief Validate time is not before compile time
 */
static bool is_time_valid(time_t unix_time)
{
    return unix_time >= compile_time_unix;
}
