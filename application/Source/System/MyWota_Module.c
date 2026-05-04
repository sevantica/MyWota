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
#include "Firmware_Version.h"
#include "SD_Logger_Task.h"

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

#include "MyWota_System.h"

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
    LOG_CRITICAL_SYSTEM("\r\n=== MyWota Application Startup ===\r\n");
    
    /* 1. Print Firmware Information */
    FW_PrintVersionInfo();

    /* 2. Configure Local Adapters */
    /* MyWota_Hardware_Adapter_Init() now called in main.c for earlier availability */
    MyWota_Config_Adapter_Init();
    
    /* 3. Intelligent Configuration Loading */
    /* Step A: Initialize defaults in case of failure */
    Config_InitDefaults();
    
    /* Step B: Try SD Card first if mounted */
    Config_Result_t cfg_res = CONFIG_FILE_NOT_FOUND;
    if (SD_Logger_IsReady()) {
        LOG_CRITICAL_SYSTEM("[CONFIG] SD ready - loading from card...\r\n");
        cfg_res = Config_LoadFromMountedFS();
        
        /* Register log formatters only if SD is available */
        SD_Logger_Format_Adapter_Init();
        if (Log_Strings_Init()) {
            LOG_CRITICAL_SYSTEM("[✓] Log strings loaded from SD\r\n");
        }
    }
    
    /* Step C: Fallback to Flash if SD failed/missing */
    if (cfg_res != CONFIG_OK) {
        LOG_CRITICAL_SYSTEM("[CONFIG] SD failed/missing - loading from Flash...\r\n");
        cfg_res = Config_LoadFromFlash();
    }
    
    if (cfg_res == CONFIG_OK) {
        const SystemConfig_t* active_cfg = Config_Get();
        LOG_CRITICAL_SYSTEM("[✓] Config Active: Device=%s Site=%s\r\n", 
                           active_cfg->system.device_id, active_cfg->system.site_id);
    } else {
        LOG_CRITICAL_SYSTEM("[!] Using hardcoded defaults (Final Result: %d)\r\n", (int)cfg_res);
    }

    const SystemConfig_t* cfg = Config_Get();

    /* 4. Peripheral Adapters */
    RTC_Persistence_Adapter_Init();
    
    if (cfg->modules.io_expander_enabled) {
        if (MyWota_IO_Expander_Adapter_Init()) {
            s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_RUNNING;
        } else {
            s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_ERROR;
        }
    }
    
    /* 5. Start Application Tasks */
    
    /* Feedback / Buzzer */
    if (cfg->modules.buzzer_enabled) {
        Feedback_Task_Start();
        s_module_states[MODULE_BUZZER] = MODULE_STATE_RUNNING;
        System_RegisterTask(SYSTEM_TASK_ID_BUZZER_POLLING, "Feedback");
    }
    
    /* Display */
    if (cfg->modules.lcd_display_enabled) {
        Task_Start_LCD_Display_Task();
        s_module_states[MODULE_LCD_DISPLAY] = MODULE_STATE_RUNNING;
        System_RegisterTask(SYSTEM_TASK_ID_LCD_DISPLAY, "LCD_Display");
    }
    
    /* MIFARE Subsystem */
    pn532_handle = System_GetPN532Handle();
    if (pn532_handle != NULL) {
        MIFARE_Volume_Adapter_Init();
        MIFARE_Dispenser_Init();
        
        if (cfg->modules.mifare_polling_enabled) {
            MIFARE_StartPollingTask();
            s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_RUNNING;
            System_RegisterTask(SYSTEM_TASK_ID_MIFARE_POLLING, "MIFARE");
        }
    }
    
    /* Dispenser Control */
    if (cfg->modules.dispenser_enabled) {
        Task_Start_Dispenser_Task();
        s_module_states[MODULE_DISPENSER] = MODULE_STATE_RUNNING;
        System_RegisterTask(SYSTEM_TASK_ID_DISPENSER, "Dispenser");
    }
    
    /* RS485 */
    if (cfg->modules.rs485_enabled) {
        Task_Start_RS485_Task();
        RS485_Command_Adapter_Init();
        s_module_states[MODULE_RS485] = MODULE_STATE_RUNNING;
        System_RegisterTask(SYSTEM_TASK_ID_RS485, "RS485");
    }
    
    LOG_CRITICAL_SYSTEM("[MODULE] MyWota Initialization Complete\r\n");
}


/* Module Control API Implementation */

bool System_StartModule(System_Module_t module)
{
    if (module >= MODULE_COUNT) {
        LOG_ERROR_SYSTEM("[MODULE] Invalid module ID: %d\r\n", (int)module);
        return false;
    }
    
    if (s_module_states[module] == MODULE_STATE_RUNNING) {
        LOG_ERROR_SYSTEM("[MODULE] %s already running\r\n", s_module_names[module]);
        return false;
    }
    
    LOG_CRITICAL_SYSTEM("[MODULE] Starting %s...\r\n", s_module_names[module]);
    
    switch (module) {
        case MODULE_LCD_DISPLAY:
            Task_Start_LCD_Display_Task();
            s_module_states[MODULE_LCD_DISPLAY] = MODULE_STATE_RUNNING;
            System_RegisterTask(SYSTEM_TASK_ID_LCD_DISPLAY, "LCD_Display");
            return true;
            
        case MODULE_MIFARE_POLLING:
            MIFARE_StartPollingTask();
            s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_RUNNING;
            System_RegisterTask(SYSTEM_TASK_ID_MIFARE_POLLING, "MIFARE");
            return true;
            
        case MODULE_DISPENSER:
            Task_Start_Dispenser_Task();
            s_module_states[MODULE_DISPENSER] = MODULE_STATE_RUNNING;
            System_RegisterTask(SYSTEM_TASK_ID_DISPENSER, "Dispenser");
            return true;
            
        case MODULE_BUZZER:
            Feedback_Task_Start();
            s_module_states[MODULE_BUZZER] = MODULE_STATE_RUNNING;
            System_RegisterTask(SYSTEM_TASK_ID_BUZZER_POLLING, "Feedback");
            return true;
            
        case MODULE_IO_EXPANDER:
            if (MyWota_IO_Expander_Adapter_Init()) {
                s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_RUNNING;
                System_RegisterTask(SYSTEM_TASK_ID_IO_EXPANDER, "IO_Expander");
                return true;
            }
            return false;
            
        case MODULE_RS485:
            Task_Start_RS485_Task();
            RS485_Command_Adapter_Init();
            s_module_states[MODULE_RS485] = MODULE_STATE_RUNNING;
            System_RegisterTask(SYSTEM_TASK_ID_RS485, "RS485");
            return true;
            
        default:
            LOG_ERROR_SYSTEM("[MODULE] Unknown module: %d\r\n", (int)module);
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
    const SystemConfig_t* cfg = Config_Get();
    LOG_CRITICAL_SYSTEM("\r\n");
    LOG_CRITICAL_SYSTEM("═══════════════════════════════════════════════════════════════\r\n");
    LOG_CRITICAL_SYSTEM("                    MODULE STATUS                                \r\n");
    LOG_CRITICAL_SYSTEM("═══════════════════════════════════════════════════════════════\r\n");
    LOG_CRITICAL_SYSTEM("Module               Boot Config  Runtime State\r\n");
    LOG_CRITICAL_SYSTEM("───────────────────────────────────────────────────────────────\r\n");
    
    const char* boot_enabled[MODULE_COUNT] = {
        cfg->modules.lcd_display_enabled ? "Enabled " : "Disabled",
        cfg->modules.mifare_polling_enabled ? "Enabled " : "Disabled",
        cfg->modules.dispenser_enabled ? "Enabled " : "Disabled",
        cfg->modules.buzzer_enabled ? "Enabled " : "Disabled",
        cfg->modules.io_expander_enabled ? "Enabled " : "Disabled",
        cfg->modules.rs485_enabled ? "Enabled " : "Disabled"
    };
    
    const char* state_strings[] = {"STOPPED", "RUNNING", "ERROR"};
    
    for (uint8_t i = 0; i < MODULE_COUNT; i++) {
        LOG_CRITICAL_SYSTEM("%-20s %-12s %-12s\r\n",
                       s_module_names[i],
                       boot_enabled[i],
                       state_strings[s_module_states[i]]);
    }
    
    LOG_CRITICAL_SYSTEM("═══════════════════════════════════════════════════════════════\r\n");
    LOG_CRITICAL_SYSTEM("\r\nCommands: start <module>, stop <module>\r\n");
    LOG_CRITICAL_SYSTEM("Modules: lcd, mifare, dispenser, buzzer, ioexp, rs485\r\n\r\n");
}
