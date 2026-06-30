/**
 * @file Feedback_Task.c
 * @brief Application Task for Feedback (Sound) Logic - MyWota (Dispenser)
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 */

#include "Feedback_Task.h"
#include "System_Config.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "Feedback_Service.h"
#include "MIFARE_Transaction_Core.h"
#include "Application_Interface.h"
#include "Dispenser_Controller.h"
#include "USB_Logging.h"
#include "Heartbeat_Task.h"
#include "Event_Broker.h"
#include "Active_Object.h"
#include "timers.h"

/* Private Variables */
static ActiveObject_t s_ao_feedback;

/* Static Allocation Buffers */
#define FEEDBACK_TASK_STACK_SIZE      (256)
#define FEEDBACK_TASK_PRIORITY        (1)

static StaticTask_t s_ao_feedback_tcb;
static StackType_t s_ao_feedback_stack[FEEDBACK_TASK_STACK_SIZE];

/* Software Timer for repeating patterns */
static TimerHandle_t s_beep_timer = NULL;
static StaticTimer_t s_beep_timer_buf;

/* Private Function Prototypes */
static void state_startup(ActiveObject_t *me, Event_t const *e);
static void state_ready(ActiveObject_t *me, Event_t const *e);

static void beep_timer_cb(TimerHandle_t xTimer)
{
    Feedback_Pattern_t pattern = (Feedback_Pattern_t)pvTimerGetTimerID(xTimer);
    Feedback_Play(pattern);
}

static void stop_beep_timer(void)
{
    if (s_beep_timer != NULL && xTimerIsTimerActive(s_beep_timer)) {
        xTimerStop(s_beep_timer, 0);
    }
}

static void start_beep_timer(Feedback_Pattern_t pattern, uint32_t interval_ms)
{
    stop_beep_timer();
    if (s_beep_timer != NULL && interval_ms > 0) {
        vTimerSetTimerID(s_beep_timer, (void*)(uintptr_t)pattern);
        xTimerChangePeriod(s_beep_timer, pdMS_TO_TICKS(interval_ms), 0);
        xTimerStart(s_beep_timer, 0);
    }
}

static void state_startup(ActiveObject_t *me, Event_t const *e)
{
    if (e->header.id == AO_EVT_INIT) {
        USB_Log_Printf("[AO Feedback] State: STARTUP\r\n");
        
        s_beep_timer = xTimerCreateStatic("Feedback_Timer",
                                          pdMS_TO_TICKS(1000),
                                          pdTRUE,
                                          NULL,
                                          beep_timer_cb,
                                          &s_beep_timer_buf);
        
        ActiveObject_Transition(me, state_ready);
    }
}

static void state_ready(ActiveObject_t *me, Event_t const *e)
{
    const SystemConfig_t* cfg = Config_Get();
    const Buzzer_Config_t* bz_cfg = &cfg->buzzer;

    if (e->header.id == AO_EVT_ENTRY) {
        USB_Log_Printf("[AO Feedback] State: READY\r\n");
        ActiveObject_Subscribe(me, EVT_RFID_CARD_DETECTED);
        ActiveObject_Subscribe(me, EVT_RFID_CARD_REMOVED);
        ActiveObject_Subscribe(me, EVT_RFID_TRANSACTION_SUCCESS);
        ActiveObject_Subscribe(me, EVT_RFID_TRANSACTION_FAILED);
        ActiveObject_Subscribe(me, EVT_OPERATION_START);
        ActiveObject_Subscribe(me, EVT_OPERATION_STOP);
        ActiveObject_Subscribe(me, EVT_BALANCE_UPDATED);
    }
    else if (e->header.id == EVT_RFID_CARD_DETECTED) {
        USB_Log_Printf("[AO Feedback] Event: CARD_DETECTED\r\n");
        Feedback_Play(FEEDBACK_PATTERN_CARD_DETECTED);
        start_beep_timer(FEEDBACK_PATTERN_CARD_DETECTED, bz_cfg->card_init_beep_interval_ms);
    }
    else if (e->header.id == EVT_RFID_TRANSACTION_SUCCESS) {
        USB_Log_Printf("[AO Feedback] Event: TRANSACTION_SUCCESS (Waiting Card Removal)\r\n");
        Feedback_Play(FEEDBACK_PATTERN_REMOVAL);
        start_beep_timer(FEEDBACK_PATTERN_REMOVAL, bz_cfg->removal_pattern_repeat_ms);
    }
    else if (e->header.id == EVT_RFID_TRANSACTION_FAILED) {
        USB_Log_Printf("[AO Feedback] Event: TRANSACTION_FAILED (Error state)\r\n");
        Feedback_Play(FEEDBACK_PATTERN_ERROR);
        start_beep_timer(FEEDBACK_PATTERN_ERROR, 3000);
    }
    else if (e->header.id == EVT_RFID_CARD_REMOVED) {
        USB_Log_Printf("[AO Feedback] Event: CARD_REMOVED\r\n");
        stop_beep_timer();
    }
    else if (e->header.id == EVT_OPERATION_START) {
        USB_Log_Printf("[AO Feedback] Event: DISPENSE_START\r\n");
        stop_beep_timer();
        Feedback_Play(FEEDBACK_PATTERN_START);
    }
    else if (e->header.id == EVT_OPERATION_STOP) {
        USB_Log_Printf("[AO Feedback] Event: DISPENSE_STOP\r\n");
        stop_beep_timer();
        Feedback_Play(FEEDBACK_PATTERN_STOP);
    }
    else if (e->header.id == EVT_BALANCE_UPDATED) {
        USB_Log_Printf("[AO Feedback] Event: BALANCE_UPDATED\r\n");
        Feedback_Play(FEEDBACK_PATTERN_DOUBLE_BEEP);
    }
}

void Feedback_Task_Start(void)
{
    if (s_ao_feedback.task != NULL) return;

    ActiveObject_Init(&s_ao_feedback, "FeedbackTask", NULL);
    
    /* Disables monitoring since this task blocks on event queue */
    System_SetTaskMonitoringEnabled(SYSTEM_TASK_ID_BUZZER_POLLING, false);

    ActiveObject_Start(&s_ao_feedback,
                       state_startup,
                       &s_ao_feedback_tcb,
                       s_ao_feedback_stack,
                       FEEDBACK_TASK_STACK_SIZE,
                       FEEDBACK_TASK_PRIORITY);
}

TaskHandle_t Feedback_Task_GetHandle(void)
{
    return s_ao_feedback.task;
}
