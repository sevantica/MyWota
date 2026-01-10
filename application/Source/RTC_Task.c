/**
 ******************************************************************************
 * @file    RTC_Task.c
 * @brief   Real-Time Clock Task - Periodic SD card persistence
 * 
 * @attention
 * Copyright (c) Sevantica 2026
 * 
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "RTC_Task.h"
#include "RTC_Manager.h"
#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include "USB_Logging.h"
#include "System.h"
#include "FreeRTOS.h"
#include "task.h"

/* Configuration -------------------------------------------------------------*/
#define RTC_TASK_PERIOD_MS          1000    /* Check every 1 second */

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_RTC_TASK_EN       0       /* Disabled - too verbose */
#define LOG_CRITICAL_RTC_TASK_EN    1
#define LOG_ERROR_RTC_TASK_EN       1

#if LOG_DEBUG_RTC_TASK_EN
    #define LOG_DEBUG_RTC_TASK(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_RTC_TASK(...)
#endif

#if LOG_CRITICAL_RTC_TASK_EN
    #define LOG_CRITICAL_RTC_TASK(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_RTC_TASK(...)
#endif

#if LOG_ERROR_RTC_TASK_EN
    #define LOG_ERROR_RTC_TASK(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_RTC_TASK(...)
#endif

/* Private Variables ---------------------------------------------------------*/
static TaskHandle_t rtc_task_handle = NULL;
static uint32_t last_save_time = 0;

/* Private Function Prototypes -----------------------------------------------*/
static void RTC_Task(void* argument);

/* ========================================================================== */
/*                            PUBLIC API                                      */
/* ========================================================================== */

/**
 * @brief Start RTC task
 */
void Task_Start_RTC_Task(void)
{
    if (rtc_task_handle != NULL) {
        LOG_ERROR_RTC_TASK("[RTC_TASK] Task already started\r\n");
        return;
    }
    
    BaseType_t result = xTaskCreate(
        RTC_Task,
        "RTC_Task",
        RTC_TASK_STACK_WORDS,
        NULL,
        RTC_TASK_PRIORITY,
        &rtc_task_handle
    );
    
    if (result != pdPASS) {
        LOG_ERROR_RTC_TASK("[RTC_TASK] Failed to create task\r\n");
        rtc_task_handle = NULL;
    }
}

/**
 * @brief Get RTC task handle
 */
TaskHandle_t RTC_Task_GetHandle(void)
{
    return rtc_task_handle;
}

/* ========================================================================== */
/*                          MAIN TASK FUNCTION                                */
/* ========================================================================== */

static void RTC_Task(void* argument)
{
    (void)argument;
    
    LOG_CRITICAL_RTC_TASK("[RTC_TASK] Task started\r\n");
    
    // System task guarantees SD Logger has completed initialization (success or failure)
    // before creating this task, so we can immediately attempt to load saved time
    
    // Initialize RTC manager (will use SD if available, fall back to compile time)
    RTC_Status_t status = RTC_Init();
    if (status != RTC_OK) {
        LOG_ERROR_RTC_TASK("[RTC_TASK] ✗ RTC_Init failed: %s\r\n", 
                           RTC_GetStatusString(status));
    }
    
    // Get initial time for reference
    last_save_time = xTaskGetTickCount();
    
    for(;;)
    {
        // Feed watchdog and report task status
        TASK_HEARTBEAT_EVERY_SECOND("RTC");
        System_ReportTaskStatus(SYSTEM_TASK_ID_RTC, true);
        
        // Check if it's time to save to SD card (every 1 minute)
        uint32_t current_time = xTaskGetTickCount();
        uint32_t elapsed_ms = pdTICKS_TO_MS(current_time - last_save_time);
        
        if (elapsed_ms >= RTC_SAVE_INTERVAL_MS) {
            // Save current time to SD card
            status = RTC_SaveToSD();
            if (status == RTC_OK) {
                LOG_DEBUG_RTC_TASK("[RTC_TASK] ✓ Time saved to SD\r\n");
            } else {
                LOG_DEBUG_RTC_TASK("[RTC_TASK] ✗ Failed to save time: %s\r\n", 
                                   RTC_GetStatusString(status));
            }
            
            last_save_time = current_time;
        }
        
        // Sleep for 1 second
        vTaskDelay(pdMS_TO_TICKS(RTC_TASK_PERIOD_MS));
    }
}
