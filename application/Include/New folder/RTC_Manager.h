/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * Real-Time Clock Manager
 * 
 * This module provides real-time clock functionality with:
 * - Compile-time fallback (never goes before build date)
 * - SD card persistence (saves every 1 minute)
 * - RS485 time synchronization
 * - USB time display and update
 *
 ******************************************************************************
 */

#ifndef RTC_MANAGER_H
#define RTC_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define RTC_SAVE_INTERVAL_MS        (60000)     /* Save to SD every 1 minute */
#define RTC_FILE_PATH               "0:/rtc_time.dat"

/* ============================================================================
 * Status Enum
 * ============================================================================ */

typedef enum {
    RTC_OK = 0,
    RTC_ERROR,
    RTC_ERROR_INVALID_PARAM,
    RTC_ERROR_NOT_INITIALIZED,
    RTC_ERROR_TIME_BEFORE_COMPILE,
    RTC_ERROR_SD_WRITE,
    RTC_ERROR_SD_READ,
} RTC_Status_t;

/* ============================================================================
 * Time Structure
 * ============================================================================ */

typedef struct {
    uint16_t year;      /* Full year (e.g., 2026) */
    uint8_t month;      /* 1-12 */
    uint8_t day;        /* 1-31 */
    uint8_t hour;       /* 0-23 */
    uint8_t minute;     /* 0-59 */
    uint8_t second;     /* 0-59 */
    uint8_t weekday;    /* 0=Sunday, 6=Saturday */
} RTC_DateTime_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize RTC manager
 * @details Loads time from SD card if available, otherwise uses compile time
 * @return RTC_OK on success
 */
RTC_Status_t RTC_Init(void);

/**
 * @brief Get current date/time
 * @param datetime Pointer to RTC_DateTime_t structure to fill
 * @return RTC_OK on success
 */
RTC_Status_t RTC_GetDateTime(RTC_DateTime_t *datetime);

/**
 * @brief Get current time as Unix timestamp
 * @return Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
 */
time_t RTC_GetUnixTime(void);

/**
 * @brief Set current date/time
 * @param datetime Pointer to RTC_DateTime_t structure with new time
 * @return RTC_OK on success, RTC_ERROR_TIME_BEFORE_COMPILE if time is before compile time
 */
RTC_Status_t RTC_SetDateTime(const RTC_DateTime_t *datetime);

/**
 * @brief Set time from Unix timestamp
 * @param unix_time Unix timestamp (seconds since 1970-01-01)
 * @return RTC_OK on success, RTC_ERROR_TIME_BEFORE_COMPILE if time is before compile time
 */
RTC_Status_t RTC_SetUnixTime(time_t unix_time);

/**
 * @brief Save current time to SD card
 * @return RTC_OK on success
 */
RTC_Status_t RTC_SaveToSD(void);

/**
 * @brief Load time from SD card
 * @return RTC_OK on success
 */
RTC_Status_t RTC_LoadFromSD(void);

/**
 * @brief Format date/time as string
 * @param datetime Pointer to RTC_DateTime_t structure
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of characters written (excluding null terminator)
 */
int RTC_FormatDateTime(const RTC_DateTime_t *datetime, char *buffer, size_t buffer_size);

/**
 * @brief Format Unix timestamp as string
 * @param unix_time Unix timestamp
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of characters written (excluding null terminator)
 */
int RTC_FormatUnixTime(time_t unix_time, char *buffer, size_t buffer_size);

/**
 * @brief Get compile time as Unix timestamp
 * @return Compile time Unix timestamp
 */
time_t RTC_GetCompileTime(void);

/**
 * @brief Get status string for RTC_Status_t
 * @param status Status code
 * @return Human-readable status string
 */
const char* RTC_GetStatusString(RTC_Status_t status);

/**
 * @brief Check if time has been synchronized since boot
 * @return true if time has been set by external source (RS485 or USB)
 */
bool RTC_IsSynchronized(void);

#endif /* RTC_MANAGER_H */
