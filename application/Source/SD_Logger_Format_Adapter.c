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
 * @file SD_Logger_Format_Adapter.c
 * @brief BigYellow SD Logger Format Adapter
 * @details Implements log formatting for MIFARE transactions and car wash events
 */

/* Includes ------------------------------------------------------------------*/
#include "SD_Logger_Format_Adapter.h"
#include "MIFARE_Transaction_Core.h"
#include "RTC_Manager.h"
#include "USB_Logging.h"
#include <stdio.h>
#include <string.h>

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_FORMAT_ADAPTER_EN    0

#if LOG_DEBUG_FORMAT_ADAPTER_EN
    #define LOG_DEBUG_FORMAT(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_FORMAT(...)
#endif

/*===========================================================================*/
/*                          Transaction Log Data                              */
/*===========================================================================*/

/**
 * @brief Transaction log data structure (passed to formatter)
 */
typedef struct {
    uint8_t card_uid[7];
    uint8_t uid_length;
    uint32_t balance_before;
    uint32_t balance_after;
    int32_t amount;             /* Negative = deduction, Positive = topup */
    const char* operation;      /* "WASH", "TOPUP", "INIT" */
} Transaction_Log_Data_t;

/**
 * @brief Card scan log data structure
 */
typedef struct {
    uint8_t card_uid[7];
    uint8_t uid_length;
    uint32_t balance;
    const void* card_data;      /* Pointer to MIFARE_CardData_t if available */
} Card_Scan_Log_Data_t;

/*===========================================================================*/
/*                          Format Callbacks                                  */
/*===========================================================================*/

/**
 * @brief Format a transaction record
 */
static int format_transaction(const void* data, char* buffer, size_t buffer_size, 
                               SD_Log_Format_Type_t entry_type)
{
    (void)entry_type;
    
    if (data == NULL || buffer == NULL) {
        return -1;
    }
    
    const Transaction_Log_Data_t* txn = (const Transaction_Log_Data_t*)data;
    
    /* Get current time */
    char time_str[24];
    RTC_DateTime_t dt;
    if (RTC_GetDateTime(&dt) == RTC_OK) {
        snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    } else {
        snprintf(time_str, sizeof(time_str), "0000-00-00 00:00:00");
    }
    
    /* Format UID as hex string */
    char uid_hex[16];
    int uid_offset = 0;
    for (int i = 0; i < txn->uid_length && uid_offset < 14; i++) {
        uid_offset += snprintf(uid_hex + uid_offset, sizeof(uid_hex) - uid_offset, 
                               "%02X", txn->card_uid[i]);
    }
    
    /* Format transaction line */
    return snprintf(buffer, buffer_size, 
                    "[%s] %s | UID: %s | Before: %lu | After: %lu | Amount: %ld\r\n",
                    time_str,
                    txn->operation ? txn->operation : "TXN",
                    uid_hex,
                    (unsigned long)txn->balance_before,
                    (unsigned long)txn->balance_after,
                    (long)txn->amount);
}

/**
 * @brief Format a card scan record
 */
static int format_card_scan(const void* data, char* buffer, size_t buffer_size,
                             SD_Log_Format_Type_t entry_type)
{
    (void)entry_type;
    
    if (data == NULL || buffer == NULL) {
        return -1;
    }
    
    const Card_Scan_Log_Data_t* scan = (const Card_Scan_Log_Data_t*)data;
    
    /* Get current time */
    char time_str[24];
    RTC_DateTime_t dt;
    if (RTC_GetDateTime(&dt) == RTC_OK) {
        snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    } else {
        snprintf(time_str, sizeof(time_str), "0000-00-00 00:00:00");
    }
    
    /* Format UID as hex string */
    char uid_hex[16];
    int uid_offset = 0;
    for (int i = 0; i < scan->uid_length && uid_offset < 14; i++) {
        uid_offset += snprintf(uid_hex + uid_offset, sizeof(uid_hex) - uid_offset, 
                               "%02X", scan->card_uid[i]);
    }
    
    /* Format scan line */
    return snprintf(buffer, buffer_size, 
                    "[%s] SCAN | UID: %s | Balance: %lu tokens\r\n",
                    time_str,
                    uid_hex,
                    (unsigned long)scan->balance);
}

/**
 * @brief Format a system event
 */
static int format_system_event(const void* data, char* buffer, size_t buffer_size,
                                SD_Log_Format_Type_t entry_type)
{
    if (buffer == NULL) {
        return -1;
    }
    
    /* Get current time */
    char time_str[24];
    RTC_DateTime_t dt;
    if (RTC_GetDateTime(&dt) == RTC_OK) {
        snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
                 dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    } else {
        snprintf(time_str, sizeof(time_str), "0000-00-00 00:00:00");
    }
    
    /* Event type prefix */
    const char* type_str;
    switch (entry_type) {
        case SDLOG_FMT_TYPE_ERROR:
            type_str = "ERROR";
            break;
        case SDLOG_FMT_TYPE_DEBUG:
            type_str = "DEBUG";
            break;
        default:
            type_str = "SYSTEM";
            break;
    }
    
    /* Format with optional message */
    if (data != NULL) {
        return snprintf(buffer, buffer_size, "[%s] %s | %s\r\n",
                        time_str, type_str, (const char*)data);
    } else {
        return snprintf(buffer, buffer_size, "[%s] %s | (no message)\r\n",
                        time_str, type_str);
    }
}

/**
 * @brief Get card-specific log filename
 */
static int get_card_log_filename(const void* data, char* buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 20) {
        return -1;
    }
    
    if (data == NULL) {
        /* Return system log filename */
        return snprintf(buffer, buffer_size, "/logs/system.log");
    }
    
    /* data is a pointer to card UID (uint8_t*) */
    const uint8_t* uid = (const uint8_t*)data;
    
    /* Create filename from first 4 bytes of UID */
    return snprintf(buffer, buffer_size, "/logs/card_%02X%02X%02X%02X.log",
                    uid[0], uid[1], uid[2], uid[3]);
}

/*===========================================================================*/
/*                          Interface Definition                              */
/*===========================================================================*/

static const SD_Log_Format_Interface_t bigyellow_format_interface = {
    .format_transaction = format_transaction,
    .format_card_scan = format_card_scan,
    .format_system_event = format_system_event,
    .get_card_log_filename = get_card_log_filename,
    .project_name = "BigYellow"
};

/*===========================================================================*/
/*                          Public Functions                                  */
/*===========================================================================*/

/**
 * @brief Initialize SD Logger format adapter
 */
SD_Log_Result_t SD_Logger_Format_Adapter_Init(void)
{
    LOG_DEBUG_FORMAT("[SD_FORMAT] Registering BigYellow log formatters...\r\n");
    
    SD_Log_Result_t result = SD_Log_RegisterFormat(&bigyellow_format_interface);
    
    if (result == SD_LOG_OK) {
        LOG_DEBUG_FORMAT("[SD_FORMAT] BigYellow log formatters registered\r\n");
    } else {
        USB_Log_Printf("[SD_FORMAT] Failed to register log formatters\r\n");
    }
    
    return result;
}
