/**
 * ******************************************************************************
 * @file    RTC_Task.c
 * @brief   Real-Time Clock Task - Periodic SD card persistence - MyWota (Dispenser)
 * 
 * @attention
 * Copyright (c) Sevantica 2026
 * 
 * ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "RTC_Task.h"
#include "RTC_Manager.h"
#include "RTC_Persistence_Interface.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "USB_Logging.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "FreeRTOS.h"
#include "task.h"
#include "Event_Broker.h"
#include "Active_Object.h"
#include "timers.h"

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
static ActiveObject_t s_ao_rtc;
static StaticTask_t s_ao_rtc_tcb;
static StackType_t s_ao_rtc_stack[RTC_TASK_STACK_WORDS];

static uint32_t last_save_time = 0;

/* Software Timer for periodic save events */
static TimerHandle_t s_rtc_timer = NULL;
static StaticTimer_t s_rtc_timer_buf;

/* Private Function Prototypes -----------------------------------------------*/
static void state_startup(ActiveObject_t *me, Event_t const *e);
static void state_running(ActiveObject_t *me, Event_t const *e);
static void rtc_timer_cb(TimerHandle_t xTimer);

/* ========================================================================== */
/*                            PUBLIC API                                      */
/* ========================================================================== */

/**
 * @brief Start RTC task
 */
void Task_Start_RTC_Task(void)
{
    if (s_ao_rtc.task != NULL) {
        LOG_ERROR_RTC_TASK("[RTC_TASK] Task already started\r\n");
        return;
    }

    ActiveObject_Init(&s_ao_rtc, "RTC_Task", NULL);

    /* Disables WDT monitoring since this task blocks on event queue */
    System_SetTaskMonitoringEnabled(SYS_TASK_ID_RTC, false);

    bool success = ActiveObject_Start(
        &s_ao_rtc,
        state_startup,
        &s_ao_rtc_tcb,
        s_ao_rtc_stack,
        RTC_TASK_STACK_WORDS,
        RTC_TASK_PRIORITY
    );

    if (!success) {
        LOG_ERROR_RTC_TASK("[RTC_TASK] Failed to create task\r\n");
    }
}

/**
 * @brief Get RTC task handle
 */
TaskHandle_t RTC_Task_GetHandle(void)
{
    return s_ao_rtc.task;
}

/* ========================================================================== */
/*                          STATE MACHINE FUNCTIONS                            */
/* ========================================================================== */

static volatile bool s_rtc_save_pending = false;

static void rtc_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_rtc_save_pending) {
        Event_t* save_evt = EventPool_Alloc(EVT_RTC_SAVE, sizeof(Event_t));
        if (save_evt != NULL) {
            s_rtc_save_pending = true;
            if (!ActiveObject_Post(&s_ao_rtc, save_evt)) {
                s_rtc_save_pending = false;
            }
        }
    }
}

static void state_startup(ActiveObject_t *me, Event_t const *e)
{
    if (e->header.id == AO_EVT_INIT) {
        LOG_CRITICAL_RTC_TASK("[RTC_TASK] State: STARTUP\r\n");
        
        // Initialize RTC manager (will use SD if available, fall back to compile time)
        RTC_Status_t status = RTC_Init();
        if (status != RTC_OK) {
            LOG_ERROR_RTC_TASK("[RTC_TASK] ✗ RTC_Init failed: %s\r\n", 
                               RTC_GetStatusString(status));
        }
        
        // Get initial time for reference
        last_save_time = xTaskGetTickCount();
        
        // Create static timer to trigger save check every 1 second
        s_rtc_timer = xTimerCreateStatic(
            "RTC_Timer",
            pdMS_TO_TICKS(1000), // check every 1 second
            pdTRUE,
            NULL,
            rtc_timer_cb,
            &s_rtc_timer_buf
        );
        
        if (s_rtc_timer != NULL) {
            xTimerStart(s_rtc_timer, 0);
        } else {
            LOG_ERROR_RTC_TASK("[RTC_TASK] Failed to create static timer\r\n");
        }
        
        ActiveObject_Transition(me, state_running);
    }
}

static void state_running(ActiveObject_t *me, Event_t const *e)
{
    (void)me;
    if (e->header.id == AO_EVT_ENTRY) {
        LOG_CRITICAL_RTC_TASK("[RTC_TASK] State: RUNNING\r\n");
        ActiveObject_Subscribe(me, EVT_RTC_SYNC);
    }
    else if (e->header.id == EVT_RTC_SAVE) {
        s_rtc_save_pending = false;
        // Feed watchdog and report task status
        TASK_HEARTBEAT_EVERY_SECOND("RTC");
        System_ReportTaskStatus(SYS_TASK_ID_RTC, true);
        
        // Get save interval from persistence adaptor (project-specific)
        uint32_t save_interval_ms = RTC_PersistGetInterval();
        
        // Check if it's time to save to persistent storage
        uint32_t current_time = xTaskGetTickCount();
        uint32_t elapsed_ms = pdTICKS_TO_MS(current_time - last_save_time);
        
        if (elapsed_ms >= save_interval_ms) {
            // Use persistence interface for saving (adaptor handles storage type)
            if (RTC_PersistIsReady()) {
                time_t unix_time = RTC_GetUnixTime();
                RTC_Persist_Result_t persist_result = RTC_PersistSave(unix_time);
                
                if (persist_result == RTC_PERSIST_OK) {
                    LOG_DEBUG_RTC_TASK("[RTC_TASK] ✓ Time saved via persistence adaptor\r\n");
                } else {
                    LOG_DEBUG_RTC_TASK("[RTC_TASK] ✗ Failed to save time: %s\r\n", 
                                       RTC_Persist_GetResultString(persist_result));
                }
            }
            last_save_time = current_time;
        }
    }
    else if (e->header.id == EVT_RTC_SYNC) {
        LOG_CRITICAL_RTC_TASK("[RTC_TASK] Event: RTC_SYNC - saving synchronized time immediately\r\n");
        
        if (RTC_PersistIsReady()) {
            time_t unix_time = RTC_GetUnixTime();
            RTC_Persist_Result_t persist_result = RTC_PersistSave(unix_time);
            
            if (persist_result == RTC_PERSIST_OK) {
                LOG_CRITICAL_RTC_TASK("[RTC_TASK] ✓ Synchronized time saved\r\n");
            } else {
                LOG_ERROR_RTC_TASK("[RTC_TASK] ✗ Failed to save sync time: %s\r\n", 
                                   RTC_Persist_GetResultString(persist_result));
            }
        }
        last_save_time = xTaskGetTickCount();
    }
}
