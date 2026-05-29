/**
 * @file    SD_Logger_Task.c
 * @brief   SD Card Logger Task Adapter - Event-driven asynchronous logging adapter
 * @attention
 * Copyright (c) Sevantica 2026
 */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "Task_Stack_Config.h"
#include "USB_Logging.h"
#include "SD_Logger_Task.h"
#include "SD_SPI_Driver.h"
#include "MIFARE_Transaction_Core.h"
#include "System_Config.h"
#include "Module_Interface.h"
#include "Event_Broker.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "RTC_Manager.h"
#include "SD_Logging_Service.h"
#include "RS485_Slave_Common_Handlers.h"
#include "System_Core.h"

/* Private Prototypes */
extern bool AO_SD_Logger_IsReady(void);

/**
 * @brief Start SD Logger Task (No-op in Active Object architecture as the core AO is started by System_Core.c)
 */
void Task_Start_SD_Logger_Task(void)
{
    /* Handled by AO_SD_Logger_Start() early in System_Core.c */
    USB_Log_Printf("[SD_ADAPTER] Active Object AO_SD_Logger is managed by System Core\r\n");
}

TaskHandle_t SD_Logger_Task_GetHandle(void)
{
    extern TaskHandle_t task_get_handle_System_Task(void);
    return task_get_handle_System_Task(); // Dummy fallback
}

/**
 * @brief Check if SD logger is ready for logging operations
 * @return true if ready, false otherwise
 */
bool SD_Logger_IsReady(void)
{
    return AO_SD_Logger_IsReady();
}

/**
 * @brief Log MIFARE card scan with all block data to SD card (Asynchronous)
 */
bool SD_Logger_LogMIFARECardScan(const uint8_t *card_uid, uint8_t uid_length, 
                                 const void *card_data, SDLogType_t log_type)
{
    if (card_uid == NULL || card_data == NULL) {
        return false;
    }
    
    const MIFARE_CardData_t *mifare_data = (const MIFARE_CardData_t *)card_data;
    char filename[64];
    
    // Create filename based on card UID
    snprintf(filename, sizeof(filename), "0:/CARD_");
    int offset = strlen(filename);
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        snprintf(filename + offset, sizeof(filename) - offset, "%02X", card_uid[i]);
        offset += 2;
    }
    snprintf(filename + offset, sizeof(filename) - offset, ".log");
    
    /* Allocate and publish event asynchronously */
    Event_Log_t* log_evt = (Event_Log_t*)EventPool_Alloc(EVT_LOG_TRANSACTION, sizeof(Event_Log_t));
    if (log_evt != NULL) {
        strncpy(log_evt->filename, filename, sizeof(log_evt->filename) - 1);
        log_evt->filename[sizeof(log_evt->filename) - 1] = '\0';
        
        snprintf(log_evt->text, sizeof(log_evt->text),
                 "MIFARE CARD SCAN - Log Type: %d\r\n"
                 "Magic: 0x%08lX | Token Count: %lu | Last Topup: %lu\r\n"
                 "Total Washes: %lu | Volume: %lu ml\r\n",
                 log_type,
                 mifare_data->header.magic_bytes,
                 mifare_data->user_primary.balance,
                 mifare_data->user_primary.last_topup,
                 mifare_data->usage_data.total_dispenses_completed,
                 mifare_data->usage_data.total_volume_dispensed);
        
        return EventBroker_Publish((Event_t*)log_evt);
    }
    
    return false;
}

/**
 * @brief Print card transaction log to USB terminal (Synchronous Reader)
 */
bool SD_Logger_PrintCardLog(const uint8_t *card_uid, uint8_t uid_length)
{
    if (card_uid == NULL) {
        USB_Log_Printf("[✗] Invalid card UID for print\r\n");
        return false;
    }
    
    if (!SD_Log_LockFS(1000)) {
        USB_Log_Printf("[✗] Failed to acquire SD mutex\r\n");
        return false;
    }
    
    FIL file;
    FRESULT result;
    char filename[64];
    
    snprintf(filename, sizeof(filename), "0:/CARD_");
    int offset = strlen(filename);
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        snprintf(filename + offset, sizeof(filename) - offset, "%02X", card_uid[i]);
        offset += 2;
    }
    snprintf(filename + offset, sizeof(filename) - offset, ".log");
    
    result = f_open(&file, filename, FA_READ);
    if (result != FR_OK) {
        USB_Log_Printf("[✗] Failed to open card log file: %s (error: %d)\r\n", filename, result);
        SD_Log_UnlockFS();
        return false;
    }
    
    USB_Log_Printf("\r\n═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("            CARD TRANSACTION LOG: %s\r\n", filename + 3);
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    
    char line_buffer[256];
    uint32_t line_count = 0;
    
    while (f_gets(line_buffer, sizeof(line_buffer), &file) != NULL) {
        line_count++;
        USB_Log_Printf("%s", line_buffer);
        
        if (line_count % 50 == 0) {
            extern void System_ReportTaskStatus(System_Task_ID_t task_id, bool is_running_ok);
            System_ReportTaskStatus(SYS_TASK_ID_USB_COMMAND, true);
        }
    }
    
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    f_close(&file);
    SD_Log_UnlockFS();
    
    return true;
}

/**
 * @brief Log a system event to the system log file (Asynchronous)
 */
bool SD_Logger_LogEvent(const char *format, ...)
{
    if (format == NULL) {
        return false;
    }
    
    char log_buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    
    /* Prepend RTC timestamp */
    char time_str[32];
    RTC_DateTime_t now;
    if (RTC_GetDateTime(&now) == RTC_OK) {
        snprintf(time_str, sizeof(time_str), "[%02d:%02d:%02d] ", now.hour, now.minute, now.second);
    } else {
        snprintf(time_str, sizeof(time_str), "[%lu] ", xTaskGetTickCount());
    }
    
    /* Print to console */
    USB_Log_Printf("[SD] %s\r\n", log_buffer);
    
    /* Allocate and publish event */
    Event_Log_t* log_evt = (Event_Log_t*)EventPool_Alloc(EVT_LOG_SYSTEM, sizeof(Event_Log_t));
    if (log_evt != NULL) {
        log_evt->filename[0] = '\0'; // default path
        snprintf(log_evt->text, sizeof(log_evt->text), "%s%s", time_str, log_buffer);
        return EventBroker_Publish((Event_t*)log_evt);
    }
    
    return false;
}

/**
 * @brief Log a transaction event (Asynchronous)
 */
bool SD_Logger_LogTransaction(const uint8_t *card_uid, uint8_t uid_length,
                              const char *event_type,
                              uint32_t balance_before, uint32_t balance_after,
                              uint32_t amount)
{
    const char* unit = MIFARE_GetBalanceUnit();

    // Mirror to USB Log for visibility
    USB_Log_Printf("[SD TXN] [%02X%02X%02X%02X] %s | %lu -> %lu %s (Amt: %lu)\r\n",
                   card_uid ? card_uid[0] : 0, card_uid ? card_uid[1] : 0,
                   card_uid ? card_uid[2] : 0, card_uid ? card_uid[3] : 0,
                   event_type ? event_type : "?",
                   balance_before, balance_after, unit, amount);

    /* Push for upload to RS485 master queue */
    if (card_uid != NULL) {
        RS485_Transaction_Record_t rec = {0};
        rec.timestamp = (uint32_t)RTC_GetUnixTime();
        rec.transaction_id = 0;
        uint8_t copy_len = uid_length;
        if (copy_len > sizeof(rec.card_uid)) copy_len = sizeof(rec.card_uid);
        memcpy(rec.card_uid, card_uid, copy_len);
        rec.card_uid_length = copy_len;
        rec.transaction_type = 0;
        rec.balance_before = balance_before;
        rec.balance_after = balance_after;
        rec.amount = (int32_t)balance_after - (int32_t)balance_before;
        (void)RS485_SlaveCommon_TxnQueue_Push(&rec);
    }

    if (card_uid == NULL || event_type == NULL) {
        return false;
    }
    
    char filename[64];
    snprintf(filename, sizeof(filename), "0:/CARD_");
    int offset = strlen(filename);
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        snprintf(filename + offset, sizeof(filename) - offset, "%02X", card_uid[i]);
        offset += 2;
    }
    snprintf(filename + offset, sizeof(filename) - offset, ".log");
    
    /* Allocate and publish event asynchronously */
    Event_Log_t* log_evt = (Event_Log_t*)EventPool_Alloc(EVT_LOG_TRANSACTION, sizeof(Event_Log_t));
    if (log_evt != NULL) {
        strncpy(log_evt->filename, filename, sizeof(log_evt->filename) - 1);
        log_evt->filename[sizeof(log_evt->filename) - 1] = '\0';
        
        RTC_DateTime_t now;
        RTC_GetDateTime(&now);
        
        snprintf(log_evt->text, sizeof(log_evt->text),
                 "[%02d:%02d:%02d] TRANSACTION: %s | Before: %lu %s | After: %lu %s | Amount: %lu %s\r\n",
                 now.hour, now.minute, now.second, event_type, balance_before, unit, balance_after, unit, amount, unit);
        
        EventBroker_Publish((Event_t*)log_evt);
    }
    
    // Also mirror to system log
    SD_Logger_LogEvent("TXN [%02X%02X%02X%02X]: %s | %lu->%lu %s",
                       card_uid[0], card_uid[1], card_uid[2], card_uid[3],
                       event_type, balance_before, balance_after, unit);
    
    return true;
}

/**
 * @brief Log an error event (Asynchronous)
 */
bool SD_Logger_LogError(const char *module, int error_code, const char *description)
{
    USB_Log_Printf("[SD ERR] [%s] %d: %s\r\n", 
                   module ? module : "?", 
                   error_code, 
                   description ? description : "");

    /* Allocate and publish event asynchronously */
    Event_Log_t* log_evt = (Event_Log_t*)EventPool_Alloc(EVT_LOG_ERROR, sizeof(Event_Log_t));
    if (log_evt != NULL) {
        log_evt->filename[0] = '\0'; // default path (error.log)
        
        RTC_DateTime_t now;
        RTC_GetDateTime(&now);
        
        snprintf(log_evt->text, sizeof(log_evt->text),
                 "[%02d:%02d:%02d] ERROR [%s] Code: %d - %s\r\n",
                 now.hour, now.minute, now.second, module ? module : "UNKNOWN", error_code, description ? description : "");
        
        EventBroker_Publish((Event_t*)log_evt);
    }
    
    // Also mirror to system log
    SD_Logger_LogEvent("ERR [%s] %d: %s", 
                       module ? module : "?", 
                       error_code, 
                       description ? description : "");
    
    return true;
}

/**
 * @brief Get last known balance from SD card transaction logs (Synchronous Reader)
 */
bool SD_Logger_GetLastBalance(const uint8_t *card_uid, uint8_t uid_length, uint32_t *balance_out)
{
    if (card_uid == NULL || balance_out == NULL) {
        return false;
    }
    
    if (!SD_Log_LockFS(1000)) {
        return false;
    }
    
    FIL file;
    FRESULT result;
    char filename[64];
    char line_buffer[256];
    
    snprintf(filename, sizeof(filename), "0:/CARD_");
    int offset = strlen(filename);
    for (uint8_t i = 0; i < uid_length && i < 7; i++) {
        snprintf(filename + offset, sizeof(filename) - offset, "%02X", card_uid[i]);
        offset += 2;
    }
    snprintf(filename + offset, sizeof(filename) - offset, ".log");
    
    result = f_open(&file, filename, FA_READ);
    if (result != FR_OK) {
        SD_Log_UnlockFS();
        return false;
    }
    
    uint32_t last_balance = 0;
    bool found = false;
    
    while (f_gets(line_buffer, sizeof(line_buffer), &file)) {
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
    SD_Log_UnlockFS();
    
    if (found) {
        *balance_out = last_balance;
    }
    
    return found;
}
