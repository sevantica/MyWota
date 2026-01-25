/**
 * @file MyWota_Module.c
 * @brief Application Module Implementation for MyWota Water Dispenser
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 */

#include "Module_Interface.h"
#include "System_Config.h"
#include "System_Core.h"
#include "USB_Logging.h"

/* Adapters */
#include "MyWota_Hardware_Adapter.h"
#include "MyWota_Config_Adapter.h"
#include "SD_Logger_Format_Adapter.h"
#include "RTC_Persistence_Adapter.h"
#include "MyWota_IO_Expander_Adapter.h"
#include "RS485_Command_Adapter.h"
#include "MIFARE_Volume_Adapter.h"

/* Application Drivers/Controllers */
#include "PN532_Driver.h"
#include "Dispenser_Controller.h"
#include "MyWota_ui_driver.h"
#include "Log_Strings.h"
#include "Feedback_Task.h"

/* MIFARE */
#include "MIFARE_Transaction_Core.h"

/* Task Headers */
#include "RS485_Task.h"

/* Defines */
#define LOG_CRITICAL_SYSTEM(...) USB_Log_Printf(__VA_ARGS__)
#define LOG_ERROR_SYSTEM(...)    USB_Log_Printf(__VA_ARGS__)

#include "System.h" // Needed for System_Module_t and task IDs

/* Module Runtime Control State Tracking */
static Module_State_t s_module_states[MODULE_COUNT] = {
    MODULE_STATE_STOPPED,  /* LCD_DISPLAY */
    MODULE_STATE_STOPPED,  /* MIFARE_POLLING */
    MODULE_STATE_STOPPED,  /* DISPENSER */
    MODULE_STATE_STOPPED,  /* BUZZER */
    MODULE_STATE_STOPPED,  /* IO_EXPANDER */
    MODULE_STATE_STOPPED   /* RS485 */
};

static const char* s_module_names[MODULE_COUNT] = {
    "LCD_Display",
    "MIFARE_Polling",
    "Dispenser",
    "Buzzer",
    "IO_Expander",
    "RS485"
};

/* Private Variables */
static PN532_Handle_t *pn532_handle = NULL;

/* Implementation -----------------------------------------------------------*/

void Module_Init(void)
{
    LOG_CRITICAL_SYSTEM("[MODULE] Starting MyWota Application Initialization...\r\n");
    
    const SystemConfig_t* cfg = Config_Get();

    /* 1. Register Adapters */
    MyWota_Hardware_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] MyWota Hardware Adapter initialized\r\n");
    
    MyWota_Config_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] MyWota Config Adapter initialized\r\n");
    
    SD_Logger_Format_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] SD Logger Format Adapter initialized\r\n");
    
    if (Log_Strings_Init()) {
        LOG_CRITICAL_SYSTEM("[✓] Log strings loaded from SD\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[!] Log strings not available - using ID fallback\r\n");
    }
    
    RTC_Persistence_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] RTC Persistence Adapter initialized\r\n");
    
    /* Initialize IO Expander (Service) */
    if (cfg->modules.io_expander_enabled) {
        if (MyWota_IO_Expander_Adapter_Init()) {
            s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_RUNNING;
            /* Note: Registers task internally */
        } else {
            s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_ERROR;
        }
    }
    
    /* 2. Get Common Hardware Handles */
    pn532_handle = System_GetPN532Handle();
    PN532_Status_t pn532_status = (pn532_handle != NULL) ? PN532_STATUS_OK : PN532_STATUS_ERROR;
    
    /* 3. Initialize High-Level Drivers */
    
    /* Feedback / Buzzer Task */
    if (cfg->modules.buzzer_enabled) {
        Feedback_Task_Start();
        s_module_states[MODULE_BUZZER] = MODULE_STATE_RUNNING;
        LOG_CRITICAL_SYSTEM("[→] Feedback Task started\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[!] Feedback Task DISABLED by config\r\n");
    }
    
    /* LCD Display */
    if (cfg->modules.lcd_display_enabled) {
        Task_Start_LCD_Display_Driver_Task();
        s_module_states[MODULE_LCD_DISPLAY] = MODULE_STATE_RUNNING;
        System_RegisterTask(SYSTEM_TASK_ID_LCD_DISPLAY, "LCD_Display");
        LOG_CRITICAL_SYSTEM("[→] LCD Display Task started\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[!] LCD Display DISABLED by config\r\n");
    }
    
    /* MIFARE Volume Adapter and Tasks */
    if (pn532_status == PN532_STATUS_OK) {
        /* MIFARE Volume Adapter (MyWota - volume-based) */
        MIFARE_Volume_Adapter_Init();
        LOG_CRITICAL_SYSTEM("[✓] MIFARE Volume Adapter initialized\r\n");
        
        /* Dispenser Integration */
        MIFARE_Dispenser_Init();
        LOG_CRITICAL_SYSTEM("[✓] MIFARE Dispenser Integration initialized\r\n");
        
        if (cfg->modules.mifare_polling_enabled) {
            MIFARE_StartPollingTask();
            s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_RUNNING;
            System_RegisterTask(SYSTEM_TASK_ID_MIFARE_POLLING, "MIFARE");
            LOG_CRITICAL_SYSTEM("[→] MIFARE Polling Task started\r\n");
        }
    } else {
        s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_ERROR;
        LOG_CRITICAL_SYSTEM("[!] MIFARE tasks skipped - PN532 not initialized\r\n");
    }

    /* Dispenser Controller */
    if (cfg->modules.dispenser_enabled) {
        Task_Start_Dispenser_Task();
        s_module_states[MODULE_DISPENSER] = MODULE_STATE_RUNNING;
        System_RegisterTask(SYSTEM_TASK_ID_DISPENSER, "Dispenser");
        LOG_CRITICAL_SYSTEM("[→] Dispenser Task started\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[!] Dispenser Task DISABLED by config\r\n");
    }
    
    /* RS485 */
    if (cfg->modules.rs485_enabled) {
        Task_Start_RS485_Task();
        RS485_Command_Adapter_Init();
        s_module_states[MODULE_RS485] = MODULE_STATE_RUNNING;
        System_RegisterTask(SYSTEM_TASK_ID_RS485, "RS485");
        LOG_CRITICAL_SYSTEM("[→] RS485 Communication Task started\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[!] RS485 Communication Task DISABLED by config\r\n");
    }
    
    LOG_CRITICAL_SYSTEM("[MODULE] MyWota Initialization Complete\r\n");
}


/* Module Control API Implementation */

bool System_StartModule(System_Module_t module)
{
    if (module >= MODULE_COUNT) {
        LOG_ERROR_SYSTEM("[MODULE] Invalid module ID: %d\r\n", module);
        return false;
    }
    
    if (s_module_states[module] == MODULE_STATE_RUNNING) {
        LOG_ERROR_SYSTEM("[MODULE] %s already running\r\n", s_module_names[module]);
        return false;
    }
    
    LOG_CRITICAL_SYSTEM("[MODULE] Starting %s...\r\n", s_module_names[module]);
    
    switch (module) {
        case MODULE_LCD_DISPLAY:
            Task_Start_LCD_Display_Driver_Task();
            s_module_states[MODULE_LCD_DISPLAY] = MODULE_STATE_RUNNING;
            return true;
            
        case MODULE_MIFARE_POLLING:
            MIFARE_StartPollingTask();
            s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_RUNNING;
            return true;
            
        case MODULE_DISPENSER:
            Task_Start_Dispenser_Task();
            s_module_states[MODULE_DISPENSER] = MODULE_STATE_RUNNING;
            return true;
            
        case MODULE_BUZZER:
            Feedback_Task_Start();
            s_module_states[MODULE_BUZZER] = MODULE_STATE_RUNNING;
            return true;
            
        case MODULE_IO_EXPANDER:
            // IO Expander Service is self-managing once started
            // But we can re-init if needed? Usually not needed.
            // Just return true if it was enabled.
            if (MyWota_IO_Expander_Adapter_Init()) {
                s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_RUNNING;
                return true;
            }
            return false;
            
        case MODULE_RS485:
            Task_Start_RS485_Task();
            s_module_states[MODULE_RS485] = MODULE_STATE_RUNNING;
            return true;
            
        default:
            LOG_ERROR_SYSTEM("[MODULE] Unknown module: %d\r\n", module);
            return false;
    }
}

bool System_StopModule(System_Module_t module)
{
    if (module >= MODULE_COUNT) return false;
    LOG_CRITICAL_SYSTEM("[MODULE] Stopping %s not implemented\r\n", s_module_names[module]);
    return false;
}

Module_State_t System_GetModuleState(System_Module_t module)
{
    if (module >= MODULE_COUNT) return MODULE_STATE_ERROR;
    return s_module_states[module];
}

const char* System_GetModuleName(System_Module_t module)
{
    if (module >= MODULE_COUNT) return "Unknown";
    return s_module_names[module];
}

void System_PrintModuleStatus(void)
{
    LOG_CRITICAL_SYSTEM("=== Module Status ===\r\n");
    for (int i = 0; i < MODULE_COUNT; i++) {
        const char* state_str = "UNKNOWN";
        switch (s_module_states[i]) {
            case MODULE_STATE_STOPPED: state_str = "STOPPED"; break;
            case MODULE_STATE_RUNNING: state_str = "RUNNING"; break;
            case MODULE_STATE_ERROR:   state_str = "ERROR";   break;
        }
        LOG_CRITICAL_SYSTEM("  %s: %s\r\n", s_module_names[i], state_str);
    }
}
