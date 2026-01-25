/**
 * @file Feedback_Task.c
 * @brief Application Task for Feedback (Sound) Logic Implementation
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 */

#include "Feedback_Task.h"
#include "System_Config.h"
#include "System.h"
#include "System Services/Feedback_Service.h"
#include "MIFARE_Transaction_Core.h"
#include "Application_Interface.h"
#include "Dispenser_Controller.h"
#include "USB_Logging.h"
#include "Heartbeat_Task.h"

/* Defines */
#define FEEDBACK_POLL_INTERVAL_MS     (50)
#define FEEDBACK_TASK_STACK_SIZE      (256)
#define FEEDBACK_TASK_PRIORITY        (1)

/* Private Variables */
static TaskHandle_t s_feedback_task_handle = NULL;

/* Static Allocation Buffers */
static StaticTask_t feedback_task_tcb;
static StackType_t feedback_task_stack[FEEDBACK_TASK_STACK_SIZE];

/* Implementaiton */
static void Feedback_TaskFunc(void *pvParameters)
{
    (void)pvParameters;
    
    USB_Log_Printf("[FEEDBACK] Task started\r\n");
    
    const SystemConfig_t* cfg = Config_Get();
    const Buzzer_Config_t* bz_cfg = &cfg->buzzer;

    static bool s_last_dispense_state = false;
    static uint32_t s_last_init_beep_tick = 0;
    static uint32_t s_last_removal_pattern_tick = 0;
    static uint32_t s_last_error_pattern_tick = 0;
    static bool s_logged_app_status = false;
    static MIFARE_TransactionState_t s_last_mifare_state = TRANSACTION_STATE_IDLE;
    
    for (;;) {
        /* Heartbeat */
        TASK_HEARTBEAT_EVERY_SECOND("Feedback");
        System_ReportTaskStatus(SYSTEM_TASK_ID_BUZZER_POLLING, true);
        
        uint32_t current_tick = xTaskGetTickCount();
        
        /* 1. Poll MIFARE Transaction State */
        MIFARE_TransactionState_t mifare_state = MIFARE_GetTransactionState();
        
        if (mifare_state == TRANSACTION_STATE_CARD_DETECTED) {
            // Card detected but not validated
            uint32_t time_since_last_beep = pdTICKS_TO_MS(current_tick - s_last_init_beep_tick);
            if (time_since_last_beep >= bz_cfg->card_init_beep_interval_ms) {
                Feedback_Play(FEEDBACK_PATTERN_CARD_DETECTED);
                s_last_init_beep_tick = current_tick;
            }
        } else if (mifare_state == TRANSACTION_STATE_WAITING_REMOVAL) {
            // Waiting for removal
            if (s_last_mifare_state != TRANSACTION_STATE_WAITING_REMOVAL) {
                // Just entered state
                Feedback_Play(FEEDBACK_PATTERN_REMOVAL);
                s_last_removal_pattern_tick = current_tick;
                USB_Log_Printf("[FEEDBACK] Card removal required - playing pattern\r\n");
            } else {
                // Repeat
                uint32_t time_since_last_pattern = pdTICKS_TO_MS(current_tick - s_last_removal_pattern_tick);
                if (time_since_last_pattern >= bz_cfg->removal_pattern_repeat_ms) {
                    Feedback_Play(FEEDBACK_PATTERN_REMOVAL);
                    s_last_removal_pattern_tick = current_tick;
                }
            }
        } else if (mifare_state == TRANSACTION_STATE_ERROR_NO_FLOW) {
            // Error
            if (s_last_mifare_state != TRANSACTION_STATE_ERROR_NO_FLOW) {
                Feedback_Play(FEEDBACK_PATTERN_ERROR);
                s_last_error_pattern_tick = current_tick;
                USB_Log_Printf("[FEEDBACK] No flow error - playing error pattern\r\n");
            } else {
                // Repeat every 3s
                uint32_t time_since_last_pattern = pdTICKS_TO_MS(current_tick - s_last_error_pattern_tick);
                if (time_since_last_pattern >= 3000) {
                     Feedback_Play(FEEDBACK_PATTERN_ERROR);
                     s_last_error_pattern_tick = current_tick;
                }
            }
        }
        
        s_last_mifare_state = mifare_state;

        /* 2. Poll Application/Dispenser State */
        const Application_Instance_t* app = Application_GetActive();
        
        if (!s_logged_app_status) {
            if (app) {
                USB_Log_Printf("[FEEDBACK] App registered: %s\r\n", app->name ? app->name : "NULL");
            }
            s_logged_app_status = true;
        }

        if (app != NULL && app->callbacks != NULL) {
            bool current_dispense_state = false;
            // For MyWota, is_operation_active maps to MIFARE_Dispenser_IsDispenseActive
            if (app->callbacks->is_operation_active != NULL) {
                current_dispense_state = app->callbacks->is_operation_active();
            }
            
            if (current_dispense_state != s_last_dispense_state) {
                 if (current_dispense_state) {
                     // Start
                     USB_Log_Printf("[FEEDBACK] Dispense START\r\n");
                     Feedback_Play(FEEDBACK_PATTERN_START);
                 } else {
                     // Stop
                     // Check if volume dispensed
                     uint32_t vol = Dispenser_GetDispensedAmountML();
                     if (vol > 0) {
                         USB_Log_Printf("[FEEDBACK] Dispense STOP (User Volume: %lu)\r\n", vol);
                         Feedback_Play(FEEDBACK_PATTERN_STOP);
                     } else {
                         USB_Log_Printf("[FEEDBACK] Dispense STOP (No Volume)\r\n");
                     }
                 }
                 s_last_dispense_state = current_dispense_state;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(FEEDBACK_POLL_INTERVAL_MS));
    }
}

void Feedback_Task_Start(void)
{
    if (s_feedback_task_handle != NULL) return;

    s_feedback_task_handle = xTaskCreateStatic(
        Feedback_TaskFunc,
        "FeedbackTask",
        FEEDBACK_TASK_STACK_SIZE,
        NULL,
        FEEDBACK_TASK_PRIORITY,
        feedback_task_stack,
        &feedback_task_tcb
    );
}
