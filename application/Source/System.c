/*
* @attention
*
*Copyright (c) Sevantica 2025.
* All rights reserved.
* This software is licensed under terms that can be found in the LICENSE file
* in the root directory of this software component.
* If no LICENSE file comes with this software, it is provided AS-IS.
*
******************************************************************************
*/

/**
 * @file System.c
 * @brief System initialization module
 * @details Responsible for initializing all hardware drivers and starting tasks.
 *          This module follows the driver initialization pattern where System.c
 *          only calls driver-level Init functions, not low-level hardware init.
 */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "USB_CDC_Task.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "lvgl.h" 
#include "MyWota_ui_driver.h"

/* Private includes ----------------------------------------------------------*/
#include "System.h"
#include "System_Config.h"
#include "Firmware_Version.h"
#include "PN532_Driver.h"
#include "MIFARE_Volume_Adapter.h"  // Token adapter in this project
#include "MIFARE_Transaction_Core.h"  // Core for task management functions
#include "Dispenser_Controller.h"
#include "Hardware_Access.h"
#include "IO_Expander_Control.h"
#include "CAT9555_Driver.h"
#include "Buzzer_Driver.h"
#include "SD_Logger_Task.h"
#include "SD_Logger_Format_Adapter.h"
#include "RTC_Task.h"
#include "RTC_Persistence_Adapter.h"
#include "RS485_Task.h"
#include "RS485_Command_Adapter.h"
#include "MyWota_Hardware_Adapter.h"
#include "MyWota_Config_Adapter.h"
#include "MyWota_IO_Expander_Adapter.h"
#include "Log_Strings.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include <stdio.h>
#include <string.h>

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_SYSTEM_EN      0
#define LOG_CRITICAL_SYSTEM_EN   1
#define LOG_ERROR_SYSTEM_EN      1

#if LOG_DEBUG_SYSTEM_EN
    #define LOG_DEBUG_SYSTEM(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_SYSTEM(...)
#endif

#if LOG_CRITICAL_SYSTEM_EN
    #define LOG_CRITICAL_SYSTEM(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_SYSTEM(...)
#endif

#if LOG_ERROR_SYSTEM_EN
    #define LOG_ERROR_SYSTEM(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_SYSTEM(...)
#endif

/* Private typedefs -----------------------------------------------------------*/

/* Watchdog Task Status Tracking */
typedef enum {
    TASK_STATUS_UNKNOWN = 0,
    TASK_STATUS_RUNNING,
    TASK_STATUS_ERROR
} Task_Status_t;

typedef enum {
    TASK_ID_SD_LOGGER = 0,
    TASK_ID_USB_CDC,
    TASK_ID_USB_COMMAND_HANDLER,
    TASK_ID_LCD_DISPLAY,
    TASK_ID_DISPENSER,
    TASK_ID_BUZZER_POLLING,
    TASK_ID_MIFARE_POLLING,
    TASK_ID_IO_EXPANDER,
    TASK_ID_RTC,
    TASK_ID_RS485,
    TASK_ID_COUNT  /* Must be last */
} Task_ID_t;

typedef struct {
    const char* name;
    Task_Status_t status;
    TickType_t last_report_tick;
} Task_WDT_Status_t;

/* Private variables ----------------------------------------------------------*/
static TaskHandle_t system_task_handle;

/* Static semaphores - accessed via getter functions */
static SemaphoreHandle_t gpio_semaphore;
static SemaphoreHandle_t i2c_semaphore;
static SemaphoreHandle_t i2c_1_Semaphore;
static SemaphoreHandle_t mux_semaphore;
static SemaphoreHandle_t spi_1_Semaphore;

/* CAT9555 I/O Expander handle pointer - points to driver-owned memory */
static CAT9555_Handle_t *cat9555_handle = NULL;

/* Buzzer handle pointer - points to driver-owned memory */
static Buzzer_Handle_t *buzzer_handle = NULL;

/**
 * @brief Get buzzer handle for other modules to use
 * @return Pointer to buzzer handle, or NULL if not initialized
 */
Buzzer_Handle_t* System_GetBuzzerHandle(void)
{
    return buzzer_handle;
}

/* PN532 NFC Driver handle pointer - points to driver-owned memory */
static PN532_Handle_t *pn532_handle = NULL;

/* Watchdog Status Tracking */
static Task_WDT_Status_t s_task_wdt_status[TASK_ID_COUNT] = {
    {"SD_Logger",    TASK_STATUS_UNKNOWN, 0},
    {"USB_CDC",       TASK_STATUS_UNKNOWN, 0},
    {"USB_Command",   TASK_STATUS_UNKNOWN, 0},
    {"LCD_Display",   TASK_STATUS_UNKNOWN, 0},
    {"Dispenser",       TASK_STATUS_UNKNOWN, 0},
    {"Buzzer",        TASK_STATUS_UNKNOWN, 0},
    {"MIFARE",        TASK_STATUS_UNKNOWN, 0},
    {"IO_Expander",   TASK_STATUS_UNKNOWN, 0},
    {"RTC",           TASK_STATUS_UNKNOWN, 0},
    {"RS485",         TASK_STATUS_UNKNOWN, 0}
};
static SemaphoreHandle_t s_wdt_status_mutex = NULL;
static bool s_watchdog_enabled = false;

/* WDT Log Storage - persists across resets */
static uint32_t s_boot_count = 0;  /* Incremented each boot, stored in flash */
static bool s_wdt_log_saved = false;  /* Prevent multiple saves per boot */

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

/* External task starter functions */
extern void Task_Start_SD_Logger_Task(void);
extern TaskHandle_t task_get_handle_MIFARE_Polling_Task(void);

/* Private function prototypes ------------------------------------------------*/
static void System_Task(void* argument);
static void system_init(void);

/* Private functions ----------------------------------------------------------*/

/**
 * @brief Initialize all system hardware and start tasks
 * @details Called once during system startup. Initializes drivers in the
 *          correct order respecting hardware dependencies.
 */
static void system_init(void)
{
    LOG_CRITICAL_SYSTEM("\r\n=== System Initialization ===\r\n");
    
    /* Initialize boot count from WDT log (before any other flash operations) */
    wdt_log_init_boot_count();
    LOG_CRITICAL_SYSTEM("[✓] Boot count: %lu\r\n", s_boot_count);
    
    /* Print firmware version first */
    FW_PrintVersionInfo();
    
    /* Register MyWota hardware configuration (pin mappings) */
    MyWota_Hardware_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] MyWota Hardware Adapter initialized\r\n");
    
    /* Register MyWota configuration schema */
    MyWota_Config_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] MyWota Config Adapter initialized\r\n");

    /* Hardware Layer */
    Init_Hardware_Layer();
    LOG_CRITICAL_SYSTEM("[✓] Hardware Layer initialized\r\n");

    /* SD Logger Task (uses already-mounted SD card if available) */
    Task_Start_SD_Logger_Task();
    LOG_CRITICAL_SYSTEM("[→] SD Logger Task started, waiting for mount result...\r\n");
    
    /* Wait for SD Logger to complete mounting (success or failure) */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    
    /* Load Configuration */
    if (SD_Logger_IsReady()) {
        LOG_CRITICAL_SYSTEM("[✓] SD Logger mounted - config loaded from SD card\r\n");
        
        /* Register BigYellow-specific log formatters */
        SD_Logger_Format_Adapter_Init();
        LOG_CRITICAL_SYSTEM("[✓] SD Logger Format Adapter initialized\r\n");
        
        /* Initialize ID-based log strings from SD card */
        if (Log_Strings_Init()) {
            LOG_CRITICAL_SYSTEM("[✓] Log strings loaded from SD\r\n");
        } else {
            LOG_CRITICAL_SYSTEM("[!] Log strings not available - using ID fallback\r\n");
        }
    } else {
        LOG_CRITICAL_SYSTEM("[!] SD Logger mount failed - loading config from flash...\r\n");
        Config_Result_t config_result = Config_LoadFromFlash();
        
        if (config_result == CONFIG_OK) {
            LOG_CRITICAL_SYSTEM("[✓] Configuration loaded from flash\r\n");
        } else {
            LOG_CRITICAL_SYSTEM("[!] No valid flash config - using defaults\r\n");
            Config_InitDefaults();
        }
    }
    
    /* Log active configuration */
    const SystemConfig_t* cfg = Config_Get();
    LOG_CRITICAL_SYSTEM("[CONFIG] Device: %s | Site: %s | TestMode: %s\r\n",
                        cfg->system.device_id, cfg->system.site_id,
                        cfg->system.test_mode_enabled ? "ON" : "OFF");

    /* RTC Task - Start immediately after SD mount completes (success or failure)
     * SD is CRITICAL for RTC to load saved time. System task blocks above ensures
     * SD initialization is complete before RTC task starts. */
    
    /* Register BigYellow-specific RTC persistence (SD card) */
    RTC_Persistence_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] RTC Persistence Adapter initialized (SD Card)\r\n");
    
    Task_Start_RTC_Task();
    LOG_CRITICAL_SYSTEM("[→] RTC Task started (SD init complete)\r\n");

    /* USB CDC */
    USB_CDC_Task_Start(); 
    LOG_CRITICAL_SYSTEM("[→] USB CDC Task started\r\n");
    
    /* USB Command Handler */
    Task_Start_USB_Command_Handler();
    LOG_CRITICAL_SYSTEM("[→] USB Command Handler Task started\r\n");
    
    /* GPIO Interrupts */
    Hardware_Init_GPIO_Interrupts();
    LOG_CRITICAL_SYSTEM("[✓] GPIO Interrupts initialized\r\n");
    
    /* CAT9555 I/O Expander (initializes I2C0 internally) */
    cat9555_handle = CAT9555_GetHandle(0);
    CAT9555_Status_t cat_status = CAT9555_ERROR;  /* Assume failure until proven otherwise */
    
    if (cat9555_handle != NULL) {
        cat_status = CAT9555_Init(cat9555_handle, CAT9555_I2C_ADDRESS);
    } else {
        LOG_CRITICAL_SYSTEM("[✗] CAT9555 handle is NULL - cannot initialize\r\n");
    }
    
    if (cat_status != CAT9555_OK) {
        LOG_CRITICAL_SYSTEM("[✗] CAT9555 I/O Expander initialization FAILED: %s\r\n",
                            CAT9555_GetStatusString(cat_status));
    } else {
        LOG_CRITICAL_SYSTEM("[✓] CAT9555 I/O Expander initialized\r\n");
    }
    
    /* I/O Expander Control (high-level abstraction layer) */
    IO_Expander_Control_Status_t io_exp_ctrl_status = IO_Expander_Control_Init();
    if (io_exp_ctrl_status != IO_EXP_CTRL_OK) {
        LOG_CRITICAL_SYSTEM("[✗] I/O Expander Control initialization FAILED: %s\r\n",
                            IO_Expander_Control_GetStatusString(io_exp_ctrl_status));
    } else {
        LOG_CRITICAL_SYSTEM("[✓] I/O Expander Control initialized\r\n");
    }
    
    /* Register MyWota IO Expander pin mapping */
    MyWota_IO_Expander_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] IO Expander Adapter initialized\r\n");
    
    /* I/O Expander Control Task (polls all pins) - check hardware and config */
    if (cat_status == CAT9555_OK && cfg->modules.io_expander_enabled) {
        Task_Start_IO_Expander_Control_Task();
        s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_RUNNING;
        LOG_CRITICAL_SYSTEM("[→] I/O Expander Control Task started\r\n");
    } else if (!cfg->modules.io_expander_enabled) {
        LOG_CRITICAL_SYSTEM("[!] I/O Expander Control Task DISABLED by config\r\n");
    } else {
        s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_ERROR;
        LOG_CRITICAL_SYSTEM("[✗] I/O Expander Control Task FAILED - CAT9555 initialization error\r\n");
    }
    
    /* Buzzer Driver (requires CAT9555) */
    buzzer_handle = Buzzer_GetHandle(0);
    Buzzer_Status_t buzzer_status = BUZZER_ERROR;  /* Assume failure until proven otherwise */
    
    if (cat_status == CAT9555_OK && buzzer_handle != NULL) {
        buzzer_status = Buzzer_Init(buzzer_handle, cat9555_handle, &cfg->buzzer);
    } else if (cat_status != CAT9555_OK) {
        LOG_CRITICAL_SYSTEM("[✗] Buzzer initialization SKIPPED - CAT9555 failed\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[✗] Buzzer handle is NULL - cannot initialize\r\n");
    }
    
    if (buzzer_status != BUZZER_OK) {
        LOG_CRITICAL_SYSTEM("[✗] Buzzer initialization FAILED: %s\r\n",
                            Buzzer_GetStatusString(buzzer_status));
    } else {
        LOG_CRITICAL_SYSTEM("[✓] Buzzer initialized\r\n");
    }
    
    /* LCD Display Task - Start early so it runs in parallel
     * with NFC initialization. LCD should not depend on NFC subsystem. */
    if (cfg->modules.lcd_display_enabled) {
        Task_Start_LCD_Display_Driver_Task();
        if (task_get_handle_LCD_Display_Driver_Task() != NULL) {
            s_module_states[MODULE_LCD_DISPLAY] = MODULE_STATE_RUNNING;
            LOG_CRITICAL_SYSTEM("[→] LCD Display Task started\r\n");
        } else {
            s_module_states[MODULE_LCD_DISPLAY] = MODULE_STATE_ERROR;
            LOG_CRITICAL_SYSTEM("[✗] LCD Display Task FAILED to start (OOM?)\r\n");
        }
    } else {
        LOG_CRITICAL_SYSTEM("[!] LCD Display DISABLED by config\r\n");
    }
    
    /* PN532 NFC Driver - Can fail without blocking LCD */
    pn532_handle = PN532_GetHandle(0);
    PN532_Status_t pn532_status = PN532_STATUS_ERROR;  /* Assume failure until proven otherwise */
    
    if (pn532_handle != NULL) {
        pn532_status = PN532_Init(pn532_handle);
    } else {
        LOG_CRITICAL_SYSTEM("[✗] PN532 Driver handle is NULL - cannot initialize\r\n");
    }
    
    if (pn532_status != PN532_STATUS_OK) {
        LOG_CRITICAL_SYSTEM("[✗] PN532 Driver initialization FAILED: %d\r\n", pn532_status);
    } else {
        LOG_CRITICAL_SYSTEM("[✓] PN532 Driver initialized\r\n");
    }

    /* MIFARE Volume Adapter (MyWota - volume-based) */
    MIFARE_Volume_Adapter_Init();
    LOG_CRITICAL_SYSTEM("[✓] MIFARE Volume Adapter initialized\r\n");
    
    /* Dispenser Integration */
    MIFARE_Dispenser_Init();
    
    /* MIFARE Polling Task - check both hardware and config */
    if (pn532_status == PN532_STATUS_OK && cfg->modules.mifare_polling_enabled) {
        MIFARE_StartPollingTask();
        if (task_get_handle_MIFARE_Polling_Task() != NULL) {
            s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_RUNNING;
            LOG_CRITICAL_SYSTEM("[→] MIFARE Polling Task started\r\n");
        } else {
            s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_ERROR;
            LOG_CRITICAL_SYSTEM("[✗] MIFARE Polling Task FAILED to start (OOM?)\r\n");
        }
    } else if (!cfg->modules.mifare_polling_enabled) {
        LOG_CRITICAL_SYSTEM("[!] MIFARE Polling Task DISABLED by config\r\n");
    } else {
        s_module_states[MODULE_MIFARE_POLLING] = MODULE_STATE_ERROR;
        LOG_CRITICAL_SYSTEM("[✗] MIFARE Polling Task FAILED - PN532 initialization error\r\n");
    }
    
    /* Dispenser Integration Task */
    if (cfg->modules.dispenser_enabled) {
        Task_Start_Dispenser_Task();
        s_module_states[MODULE_DISPENSER] = MODULE_STATE_RUNNING;
        LOG_CRITICAL_SYSTEM("[→] Dispenser Task started\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[!] Dispenser Task DISABLED by config\r\n");
    }

    /* Buzzer Polling Task - check both hardware and config */
    if (buzzer_status == BUZZER_OK && cfg->modules.buzzer_enabled) {
        Buzzer_StartPollingTask(buzzer_handle);
        s_module_states[MODULE_BUZZER] = MODULE_STATE_RUNNING;
        LOG_CRITICAL_SYSTEM("[→] Buzzer Polling Task started\r\n");
    } else if (!cfg->modules.buzzer_enabled) {
        LOG_CRITICAL_SYSTEM("[!] Buzzer Polling Task DISABLED by config\r\n");
    } else {
        s_module_states[MODULE_BUZZER] = MODULE_STATE_ERROR;
        LOG_CRITICAL_SYSTEM("[✗] Buzzer Polling Task FAILED - Buzzer initialization error\r\n");
    }
    
    /* RS485 Communication Task - check config */
    if (cfg->modules.rs485_enabled) {
        Task_Start_RS485_Task();
        s_module_states[MODULE_RS485] = MODULE_STATE_RUNNING;
        LOG_CRITICAL_SYSTEM("[→] RS485 Communication Task started\r\n");
        
        /* Register BigYellow-specific RS485 commands */
        RS485_Command_Adapter_Init();
        LOG_CRITICAL_SYSTEM("[✓] RS485 Command Adapter initialized\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[!] RS485 Communication Task DISABLED by config\r\n");
    }
    
    /* Initialize Watchdog Status Tracking */
    s_wdt_status_mutex = xSemaphoreCreateMutex();
    if (s_wdt_status_mutex == NULL) {
        LOG_CRITICAL_SYSTEM("[✗] Failed to create WDT status mutex\r\n");
    } else {
        LOG_CRITICAL_SYSTEM("[✓] WDT status tracking initialized\r\n");
    }
    
    /* Enable Hardware Watchdog - 1000ms timeout, pause during debug */
    watchdog_enable(1000, true);
    s_watchdog_enabled = true;
    LOG_CRITICAL_SYSTEM("[✓] Hardware Watchdog enabled (1000ms timeout)\r\n");
    
    LOG_CRITICAL_SYSTEM("=== Initialization Phase Complete ===\r\n\r\n");
}

/**
 * @brief System FreeRTOS task
 * @param argument Task parameters (unused)
 * @details Performs system initialization and then monitors task health.
 *          Clears watchdog if all tasks report OK within 400ms window.
 *          Logs to SD if any task fails to report.
 */
static void System_Task(void* argument)
{
    (void)argument;
    
    /* Perform system initialization */
    system_init();
    
    /* Give tasks time to complete initialization before starting watchdog monitoring
     * LCD task takes ~2.6s (display init + UI render + backlight fade)
     * PN532 can take ~2s for I2C communication 
     * During this period, feed the watchdog to prevent hardware reset */
    const TickType_t startup_grace_period = pdMS_TO_TICKS(5000);  /* 5 second grace period */
    const TickType_t grace_period_tick = pdMS_TO_TICKS(100);      /* Check every 100ms */
    LOG_CRITICAL_SYSTEM("[WDT] Waiting %lu ms for tasks to complete initialization...\r\n", 
                        (unsigned long)(startup_grace_period * portTICK_PERIOD_MS));
    
    TickType_t grace_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - grace_start) < startup_grace_period) {
        watchdog_update();  /* Keep hardware watchdog happy during grace period */
        vTaskDelay(grace_period_tick);
    }
    
    LOG_CRITICAL_SYSTEM("[WDT] Grace period complete - starting watchdog monitoring\r\n");
    
    /* Main watchdog monitoring loop */
    const TickType_t wdt_check_period = pdMS_TO_TICKS(100);  /* Check every 100ms */
    const TickType_t report_timeout = pdMS_TO_TICKS(800);    /* 800ms reporting window (leaves 200ms margin before 1000ms WDT timeout) */
    
    /* Clear watchdog once before starting monitoring loop */
    watchdog_update();
    TickType_t last_wdt_clear = xTaskGetTickCount();
    
    for (;;)
    {
        vTaskDelay(wdt_check_period);
        
        if (!s_watchdog_enabled || !s_wdt_status_mutex) {
            continue;
        }
        
        /* Check if it's time to evaluate task statuses */
        TickType_t check_time = xTaskGetTickCount();
        TickType_t elapsed_since_clear = check_time - last_wdt_clear;
        
        if (elapsed_since_clear >= report_timeout) {
            bool all_tasks_ok = true;
            bool any_task_reported = false;
            TickType_t now;  /* Will be read inside mutex to avoid race condition */
            
            /* Local copy of task statuses to avoid holding mutex during logging */
            typedef struct {
                Task_Status_t status;
                TickType_t last_report_tick;
                const char* name;
            } Task_Status_Snapshot_t;
            Task_Status_Snapshot_t status_snapshot[TASK_ID_COUNT];
            
            /* Take mutex BRIEFLY to copy all task statuses */
            if (xSemaphoreTake(s_wdt_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                /* Use critical section to make snapshot + timestamp atomic */
                taskENTER_CRITICAL();
                
                /* Read time with interrupts disabled to prevent race */
                now = xTaskGetTickCount();
                
                /* Copy task status data - FAST operation, no logging */
                for (uint8_t i = 0; i < TASK_ID_COUNT; i++) {
                    status_snapshot[i].status = s_task_wdt_status[i].status;
                    status_snapshot[i].last_report_tick = s_task_wdt_status[i].last_report_tick;
                    status_snapshot[i].name = s_task_wdt_status[i].name;
                }
                
                taskEXIT_CRITICAL();
                
                /* Release mutex IMMEDIATELY - total hold time ~1ms */
                xSemaphoreGive(s_wdt_status_mutex);
                
                /* Now analyze the snapshot WITHOUT holding the mutex */
                const TickType_t max_silence_time = report_timeout * 3;
                
                for (uint8_t i = 0; i < TASK_ID_COUNT; i++) {
                    /* Check if task is expected to be running based on module state */
                    bool expected_running = true;
                    
                    /* Map Task ID to Module ID where applicable */
                    switch (i) {
                        case TASK_ID_LCD_DISPLAY:
                            if (s_module_states[MODULE_LCD_DISPLAY] != MODULE_STATE_RUNNING) expected_running = false;
                            break;
                        case TASK_ID_MIFARE_POLLING:
                            if (s_module_states[MODULE_MIFARE_POLLING] != MODULE_STATE_RUNNING) expected_running = false;
                            break;
                        case TASK_ID_DISPENSER:
                            if (s_module_states[MODULE_DISPENSER] != MODULE_STATE_RUNNING) expected_running = false;
                            break;
                        case TASK_ID_BUZZER_POLLING:
                            if (s_module_states[MODULE_BUZZER] != MODULE_STATE_RUNNING) expected_running = false;
                            break;
                        case TASK_ID_IO_EXPANDER:
                            if (s_module_states[MODULE_IO_EXPANDER] != MODULE_STATE_RUNNING) expected_running = false;
                            break;
                        case TASK_ID_RS485:
                            if (s_module_states[MODULE_RS485] != MODULE_STATE_RUNNING) expected_running = false;
                            break;
                        default:
                            /* Core system tasks (USB, SD, RTC) are always expected to run */
                            expected_running = true;
                            break;
                    }
                    
                    if (!expected_running) {
                        continue;
                    }

                    TickType_t time_since_report = now - status_snapshot[i].last_report_tick;
                    
                    /* Only check time-based timeout, not status enum.
                     * Task is OK if it reported within 2400ms, regardless of status enum value. */
                    if (time_since_report > max_silence_time) {
                        /* Task hasn't reported in 3x timeout - truly hung */
                        all_tasks_ok = false;
                        any_task_reported = true;
                    } else if (status_snapshot[i].status == TASK_STATUS_ERROR) {
                        /* Task explicitly reported an error */
                        all_tasks_ok = false;
                        any_task_reported = true;
                    } else if (status_snapshot[i].status == TASK_STATUS_RUNNING) {
                        /* Task reported OK recently */
                        any_task_reported = true;
                    }
                }
                
                /* If all tasks reported OK, clear watchdog */
                if (all_tasks_ok && any_task_reported) {
                    watchdog_update();  /* Clear hardware watchdog */
                    last_wdt_clear = now;
                    
                    /* Reset task statuses for next cycle - take mutex briefly */
                    if (xSemaphoreTake(s_wdt_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        for (uint8_t i = 0; i < TASK_ID_COUNT; i++) {
                            s_task_wdt_status[i].status = TASK_STATUS_UNKNOWN;
                        }
                        xSemaphoreGive(s_wdt_status_mutex);
                    }
                } else {
                    /* Save WDT status to flash BEFORE logging (in case WDT triggers during log) */
                    System_SaveWDTLogToFlash();
                    
                    /* Log missing/failed tasks - can take 100s of ms with USB + SD */
                    LOG_CRITICAL_SYSTEM("[WDT] Not all tasks reported within 2400ms:\r\n");
                    
                    for (uint8_t i = 0; i < TASK_ID_COUNT; i++) {
                        const char* status_str;
                        TickType_t time_since_report = now - status_snapshot[i].last_report_tick;
                        
                        /* Debug logging for USB_Command task */
                        if (i == TASK_ID_USB_COMMAND_HANDLER) {
                            USB_Log_Printf("[WDT_DBG] USB_Command snapshot: status=%d, last_tick=%lu, now=%lu, diff=%lu\r\n",
                                         status_snapshot[i].status,
                                         status_snapshot[i].last_report_tick,
                                         now,
                                         time_since_report);
                        }
                        
                        switch (status_snapshot[i].status) {
                            case TASK_STATUS_UNKNOWN:
                                status_str = "NO_REPORT";
                                break;
                            case TASK_STATUS_RUNNING:
                                if (time_since_report > report_timeout) {
                                    status_str = "TIMEOUT";
                                } else {
                                    status_str = "OK";
                                }
                                break;
                            case TASK_STATUS_ERROR:
                                status_str = "ERROR";
                                break;
                            default:
                                status_str = "UNKNOWN";
                                break;
                        }
                        
                        LOG_CRITICAL_SYSTEM("  %s: %s (last: %lu ms ago)\r\n",
                                          status_snapshot[i].name,
                                          status_str,
                                          (unsigned long)time_since_report);
                        
                        /* Also log to SD if available */
                        if (SD_Logger_IsReady()) {
                            SD_Logger_LogEvent("WDT: %s - %s (last: %lu ms)",
                                    status_snapshot[i].name,
                                    status_str,
                                    (unsigned long)time_since_report);
                        }
                    }
                    
                    /* Reset timer and task statuses for next cycle to avoid spam */
                    last_wdt_clear = now;
                    if (xSemaphoreTake(s_wdt_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        for (uint8_t i = 0; i < TASK_ID_COUNT; i++) {
                            s_task_wdt_status[i].status = TASK_STATUS_UNKNOWN;
                        }
                        xSemaphoreGive(s_wdt_status_mutex);
                    }
                }
            }
        }
    }
}

/* Public functions -----------------------------------------------------------*/

void Task_Start_System_Task(void)
{
    xTaskCreate(System_Task, "System_Task", SYSTEM_TASK_STACK_WORDS, NULL, SYSTEM_TASK_PRIORITY, &system_task_handle);
}

TaskHandle_t task_get_handle_System_Task(void)
{
    return system_task_handle;
}

/* ========================================================================== */
/*                         SEMAPHORE GETTER FUNCTIONS                        */
/* ========================================================================== */

SemaphoreHandle_t System_GetGpioSemaphore(void)
{
    return gpio_semaphore;
}

SemaphoreHandle_t System_GetI2C0Semaphore(void)
{
    return i2c_semaphore;
}

SemaphoreHandle_t System_GetI2C1Semaphore(void)
{
    return i2c_1_Semaphore;
}

SemaphoreHandle_t System_GetMuxSemaphore(void)
{
    return mux_semaphore;
}

SemaphoreHandle_t System_GetSPI1Semaphore(void)
{
    return spi_1_Semaphore;
}

/* ========================================================================== */
/*                    WATCHDOG TASK HEALTH REPORTING API                     */
/* ========================================================================== */

/**
 * @brief Tasks call this to report their running status
 * @param task_id Unique task identifier from System_Task_ID_t enum
 * @param is_running_ok true if task is healthy, false if error detected
 */
void System_ReportTaskStatus(System_Task_ID_t task_id, bool is_running_ok)
{
    /* Map external SYSTEM_TASK_ID_* to internal TASK_ID_* */
    Task_ID_t internal_id;
    
    /* Core tasks (0-9) map directly */
    if (task_id <= SYS_TASK_ID_RS485) {
        switch (task_id) {
            case SYS_TASK_ID_SD_LOGGER:    internal_id = TASK_ID_SD_LOGGER; break;
            case SYS_TASK_ID_USB_CDC:      internal_id = TASK_ID_USB_CDC; break;
            case SYS_TASK_ID_USB_COMMAND:  internal_id = TASK_ID_USB_COMMAND_HANDLER; break;
            case SYS_TASK_ID_RTC:          internal_id = TASK_ID_RTC; break;
            case SYS_TASK_ID_RS485:        internal_id = TASK_ID_RS485; break;
            default:
                LOG_ERROR_SYSTEM("[WDT] Unknown core task ID: %d\r\n", task_id);
                return;
        }
    }
    /* App tasks start at SYS_TASK_ID_APP_START (10) */
    else if (task_id >= SYS_TASK_ID_APP_START) {
        uint8_t app_offset = task_id - SYS_TASK_ID_APP_START;
        switch (app_offset) {
            case 0: internal_id = TASK_ID_LCD_DISPLAY; break;     /* SYSTEM_TASK_ID_LCD_DISPLAY */
            case 1: internal_id = TASK_ID_DISPENSER; break;       /* SYSTEM_TASK_ID_DISPENSER */
            case 2: internal_id = TASK_ID_BUZZER_POLLING; break;  /* SYSTEM_TASK_ID_BUZZER_POLLING */
            case 3: internal_id = TASK_ID_MIFARE_POLLING; break;  /* SYSTEM_TASK_ID_MIFARE_POLLING */
            case 4: internal_id = TASK_ID_IO_EXPANDER; break;     /* SYSTEM_TASK_ID_IO_EXPANDER */
            default:
                LOG_ERROR_SYSTEM("[WDT] Unknown app task ID: %d (offset=%d)\r\n", task_id, app_offset);
                return;
        }
    }
    else {
        LOG_ERROR_SYSTEM("[WDT] Invalid task ID: %d\r\n", task_id);
        return;
    }
    
    static bool usb_mutex_not_ready_logged = false;
    static bool usb_first_success_logged = false;
    
    if (!s_wdt_status_mutex) {
        /* Silently ignore during early boot before mutex created */
        if (internal_id == TASK_ID_USB_COMMAND_HANDLER && !usb_mutex_not_ready_logged) {
            USB_Log_Printf("[WDT_DBG] USB_Command waiting for mutex initialization...\r\n");
            usb_mutex_not_ready_logged = true;
        }
        return;
    }
    
    if (xSemaphoreTake(s_wdt_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        TickType_t tick = xTaskGetTickCount();
        s_task_wdt_status[internal_id].status = is_running_ok ? TASK_STATUS_RUNNING : TASK_STATUS_ERROR;
        s_task_wdt_status[internal_id].last_report_tick = tick;
        
        if (internal_id == TASK_ID_USB_COMMAND_HANDLER && !usb_first_success_logged) {
            USB_Log_Printf("[WDT_DBG] USB_Command now reporting successfully (tick=%lu)\r\n", tick);
            usb_first_success_logged = true;
        }
        
        xSemaphoreGive(s_wdt_status_mutex);
    } else {
        /* Failed to take mutex - likely System_Task holding it for logging */
        if (internal_id == TASK_ID_USB_COMMAND_HANDLER) {
            USB_Log_Printf("[WDT_DBG] USB_Command report failed - mutex timeout\r\n");
        }
    }
}

/* ========================================================================== */
/*                      MODULE RUNTIME CONTROL API                           */
/* ========================================================================== */

/**
 * @brief Start a module at runtime
 * @param module Module to start
 * @return true if module started successfully, false otherwise
 */
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
            if (Buzzer_IsInitialized(buzzer_handle)) {
                Buzzer_StartPollingTask(buzzer_handle);
                s_module_states[MODULE_BUZZER] = MODULE_STATE_RUNNING;
                return true;
            } else {
                LOG_ERROR_SYSTEM("[MODULE] Buzzer hardware not initialized\r\n");
                s_module_states[MODULE_BUZZER] = MODULE_STATE_ERROR;
                return false;
            }
            
        case MODULE_IO_EXPANDER:
            Task_Start_IO_Expander_Control_Task();
            s_module_states[MODULE_IO_EXPANDER] = MODULE_STATE_RUNNING;
            return true;
            
        case MODULE_RS485:
            Task_Start_RS485_Task();
            s_module_states[MODULE_RS485] = MODULE_STATE_RUNNING;
            return true;
            
        default:
            LOG_ERROR_SYSTEM("[MODULE] Unknown module: %d\r\n", module);
            return false;
    }
}

/**
 * @brief Stop a module at runtime
 * @param module Module to stop
 * @return true if module stopped successfully, false otherwise
 * @note Currently not fully implemented - task deletion requires careful resource cleanup
 */
bool System_StopModule(System_Module_t module)
{
    if (module >= MODULE_COUNT) {
        LOG_ERROR_SYSTEM("[MODULE] Invalid module ID: %d\r\n", module);
        return false;
    }
    
    if (s_module_states[module] != MODULE_STATE_RUNNING) {
        LOG_ERROR_SYSTEM("[MODULE] %s not running\r\n", s_module_names[module]);
        return false;
    }
    
    LOG_CRITICAL_SYSTEM("[MODULE] Stopping %s not fully implemented\r\n", s_module_names[module]);
    /* TODO: Implement task suspension/deletion with proper cleanup */
    return false;
}

/**
 * @brief Get current state of a module
 * @param module Module to query
 * @return Current module state
 */
Module_State_t System_GetModuleState(System_Module_t module)
{
    if (module >= MODULE_COUNT) {
        return MODULE_STATE_ERROR;
    }
    return s_module_states[module];
}

/**
 * @brief Get module name string
 * @param module Module ID
 * @return Module name string, or "Unknown" if invalid
 */
const char* System_GetModuleName(System_Module_t module)
{
    if (module >= MODULE_COUNT) {
        return "Unknown";
    }
    return s_module_names[module];
}

/**
 * @brief Print status of all modules to USB log
 */
void System_PrintModuleStatus(void)
{
    const SystemConfig_t *cfg = Config_Get();
    
    USB_Log_Printf("\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("                    MODULE STATUS                                \r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("Module               Boot Config  Runtime State\r\n");
    USB_Log_Printf("───────────────────────────────────────────────────────────────\r\n");
    
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
        USB_Log_Printf("%-20s %-12s %-12s\r\n",
                       s_module_names[i],
                       boot_enabled[i],
                       state_strings[s_module_states[i]]);
    }
    
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("\r\nCommands: start <module>, stop <module>\r\n");
    USB_Log_Printf("Modules: lcd, mifare, dispenser, buzzer, ioexp, rs485\r\n");
    USB_Log_Printf("\r\n");
}

/* WDT Log Flash Storage Functions ------------------------------------------- */

/**
 * @brief Simple CRC32 calculation for WDT log validation
 */
static uint32_t wdt_log_crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

/**
 * @brief Load WDT log from flash and extract boot count
 * @note Called at startup to increment boot counter
 */
static void wdt_log_init_boot_count(void)
{
    const WDT_Log_t* flash_log = (const WDT_Log_t*)(XIP_BASE + WDT_LOG_FLASH_OFFSET);
    
    /* Check if valid log exists */
    if (flash_log->magic == WDT_LOG_MAGIC_NUMBER && 
        flash_log->version == WDT_LOG_VERSION) {
        
        /* Verify CRC (exclude CRC field itself) */
        uint32_t calc_crc = wdt_log_crc32((const uint8_t*)flash_log, 
                                          sizeof(WDT_Log_t) - sizeof(uint32_t));
        if (calc_crc == flash_log->crc32) {
            s_boot_count = flash_log->boot_count + 1;
            LOG_DEBUG_SYSTEM("[WDT_LOG] Previous log found, boot count: %lu\r\n", s_boot_count);
            return;
        }
    }
    
    /* No valid log - start at 1 */
    s_boot_count = 1;
    LOG_DEBUG_SYSTEM("[WDT_LOG] No previous log found, boot count: 1\r\n");
}

/**
 * @brief Save WDT task status to flash before watchdog reset
 */
void System_SaveWDTLogToFlash(void)
{
    /* Prevent multiple saves per boot */
    if (s_wdt_log_saved) {
        return;
    }
    s_wdt_log_saved = true;
    
    /* Build the WDT log structure */
    static WDT_Log_t wdt_log;  /* Static to avoid stack overflow */
    memset(&wdt_log, 0, sizeof(WDT_Log_t));
    
    wdt_log.magic = WDT_LOG_MAGIC_NUMBER;
    wdt_log.version = WDT_LOG_VERSION;
    wdt_log.timestamp_tick = xTaskGetTickCount();
    wdt_log.boot_count = s_boot_count;
    wdt_log.task_count = (uint8_t)TASK_ID_COUNT;
    
    /* Copy task status - no mutex, we're in critical state */
    TickType_t now = xTaskGetTickCount();
    for (uint8_t i = 0; i < TASK_ID_COUNT && i < WDT_LOG_MAX_TASKS; i++) {
        strncpy(wdt_log.tasks[i].name, s_task_wdt_status[i].name, WDT_LOG_TASK_NAME_LEN - 1);
        wdt_log.tasks[i].name[WDT_LOG_TASK_NAME_LEN - 1] = '\0';
        wdt_log.tasks[i].status = (uint8_t)s_task_wdt_status[i].status;
        wdt_log.tasks[i].last_report_tick = s_task_wdt_status[i].last_report_tick;
        wdt_log.tasks[i].time_since_report_ms = (now - s_task_wdt_status[i].last_report_tick) * portTICK_PERIOD_MS;
    }
    
    /* Calculate CRC (exclude CRC field itself) */
    wdt_log.crc32 = wdt_log_crc32((const uint8_t*)&wdt_log, sizeof(WDT_Log_t) - sizeof(uint32_t));
    
    /* Calculate write size - must be multiple of FLASH_PAGE_SIZE (256 bytes) */
    size_t write_size = ((sizeof(WDT_Log_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
    
    /* Feed watchdog before flash operations */
    watchdog_update();
    
    /* Disable interrupts for flash write */
    uint32_t interrupts = save_and_disable_interrupts();
    
    /* Erase the sector first */
    flash_range_erase(WDT_LOG_FLASH_OFFSET, WDT_LOG_FLASH_SECTOR_SIZE);
    
    /* Brief interrupt enable for watchdog */
    restore_interrupts(interrupts);
    watchdog_update();
    interrupts = save_and_disable_interrupts();
    
    /* Write the log */
    flash_range_program(WDT_LOG_FLASH_OFFSET, (const uint8_t*)&wdt_log, write_size);
    
    /* Restore interrupts */
    restore_interrupts(interrupts);
    
    LOG_CRITICAL_SYSTEM("[WDT_LOG] Saved WDT log to flash (boot %lu, CRC: 0x%08lX)\r\n", 
                        s_boot_count, wdt_log.crc32);
}

/**
 * @brief Load WDT log from flash memory
 * @param log Pointer to WDT_Log_t structure to fill
 * @return true if valid log was loaded, false if no valid log exists
 */
bool System_LoadWDTLogFromFlash(WDT_Log_t* log)
{
    if (log == NULL) {
        return false;
    }
    
    const WDT_Log_t* flash_log = (const WDT_Log_t*)(XIP_BASE + WDT_LOG_FLASH_OFFSET);
    
    /* Check magic number */
    if (flash_log->magic != WDT_LOG_MAGIC_NUMBER) {
        return false;
    }
    
    /* Check version */
    if (flash_log->version != WDT_LOG_VERSION) {
        return false;
    }
    
    /* Verify CRC */
    uint32_t calc_crc = wdt_log_crc32((const uint8_t*)flash_log, 
                                      sizeof(WDT_Log_t) - sizeof(uint32_t));
    if (calc_crc != flash_log->crc32) {
        return false;
    }
    
    /* Copy to output */
    memcpy(log, flash_log, sizeof(WDT_Log_t));
    return true;
}

/**
 * @brief Print stored WDT log via USB (for diagnostics)
 */
void System_PrintWDTLog(void)
{
    static WDT_Log_t log;  /* Static to avoid stack overflow */
    
    if (!System_LoadWDTLogFromFlash(&log)) {
        USB_Log_Printf("[WDT_LOG] No valid WDT log found in flash\r\n");
        return;
    }
    
    USB_Log_Printf("\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("                   WDT LOG FROM FLASH                           \r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("Boot Count:        %lu\r\n", log.boot_count);
    USB_Log_Printf("Saved at Tick:     %lu (%lu ms)\r\n", 
                   log.timestamp_tick, log.timestamp_tick * portTICK_PERIOD_MS);
    USB_Log_Printf("Task Count:        %u\r\n", log.task_count);
    USB_Log_Printf("CRC32:             0x%08lX\r\n", log.crc32);
    USB_Log_Printf("───────────────────────────────────────────────────────────────\r\n");
    USB_Log_Printf("Task             Status      Last Report    Time Since Report\r\n");
    USB_Log_Printf("───────────────────────────────────────────────────────────────\r\n");
    
    const char* status_strings[] = {"UNKNOWN", "RUNNING", "ERROR"};
    
    for (uint8_t i = 0; i < log.task_count && i < WDT_LOG_MAX_TASKS; i++) {
        const char* status_str = (log.tasks[i].status < 3) ? 
                                 status_strings[log.tasks[i].status] : "???";
        
        USB_Log_Printf("%-16s %-10s  %10lu     %lu ms\r\n",
                       log.tasks[i].name,
                       status_str,
                       log.tasks[i].last_report_tick,
                       log.tasks[i].time_since_report_ms);
    }
    
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("\r\n");
}

/**
 * @brief Get boot count from stored WDT log
 * @return Current boot count
 */
uint32_t System_GetBootCount(void)
{
    return s_boot_count;
}
