/*
 * @attention
 * Copyright (c) Sevantica 2025.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file USB_Command_Handler.c
 * @brief USB command line interface for debugging and configuration
 * @details Provides a simple command-line interface over USB CDC for
 *          system diagnostics, configuration viewing, and control
 */

/* Includes ------------------------------------------------------------------*/
#include "USB_Command_Handler.h"
#include "USB_CDC_Task.h"
#include "USB_Logging.h"
#include "USB_MSC_SD.h"
#include "Status_String_Utils.h"
#include "System_Config.h"
#include "System.h"
#include "Firmware_Version.h"
#include "SD_Logger_Task.h"
#include "SD_SPI_Driver.h"  /* For SD card info */
#include "ff.h"  /* FatFs for SD card file operations */
#include "MIFARE_Transaction_Manager.h"
#include "Dispenser_Controller.h"
#include "PN532_Driver.h"
#include "FreeRTOS.h"
#include "task.h"
#include "task_stack_config.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"  /* For reset_usb_boot() */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>


/* Private defines -----------------------------------------------------------*/
#define LOG_DEBUG_CMD_EN      0  // Disabled for production (logs every keystroke)
#define LOG_CRITICAL_CMD_EN   1
#define LOG_ERROR_CMD_EN      1

#if LOG_DEBUG_CMD_EN
    #define LOG_DEBUG_CMD(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_CMD(...)
#endif

#if LOG_CRITICAL_CMD_EN
    #define LOG_CRITICAL_CMD(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_CMD(...)
#endif

#if LOG_ERROR_CMD_EN
    #define LOG_ERROR_CMD(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_CMD(...)
#endif

/* Convenience logging macro */
#define CMD_DEBUG_PRINT(fmt, ...) LOG_DEBUG_CMD("USB_CMD: " fmt "\r\n", ##__VA_ARGS__)

/* Helper function for case-insensitive substring search */
static const char* strcasestr_local(const char* haystack, const char* needle)
{
    if (!needle[0]) return haystack;
    
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        
        while (*h && *n && (toupper((unsigned char)*h) == toupper((unsigned char)*n))) {
            h++;
            n++;
        }
        
        if (!*n) return haystack;  /* Found match */
    }
    return NULL;  /* Not found */
}

/* Private typedefs ----------------------------------------------------------*/
typedef USB_Command_Status_t (*command_handler_t)(int argc, char** argv);

typedef struct {
    const char* name;
    command_handler_t handler;
    const char* description;
    const char* usage;
} command_entry_t;

/* Private variables ---------------------------------------------------------*/
static TaskHandle_t usb_command_task_handle = NULL;

/* Pending card command state */
static USB_PendingCommandState_t s_pending_command = {
    .command = USB_PENDING_CMD_NONE,
    .param_value = 0,
    .expire_tick = 0,
    .active = false
};

/**
 * @brief Get USB Command Handler task handle
 * @return Task handle or NULL if not created
 */
TaskHandle_t task_get_handle_USB_Command_Handler_Task(void)
{
    return usb_command_task_handle;
}
static char cmd_line_buffer[USB_CMD_MAX_LINE_LENGTH];
static size_t cmd_line_index = 0;
static char* cmd_args[USB_CMD_MAX_ARGS];
static TickType_t last_rx_tick = 0;
#define CMD_AUTO_EXECUTE_TIMEOUT_MS  500  /* Auto-execute after 500ms of no input */

/* File list cache for numbered file access */
#define SD_FILE_LIST_MAX  20
static char sd_file_list[SD_FILE_LIST_MAX][32];  /* Cache of listed filenames */
static uint32_t sd_file_list_count = 0;          /* Number of files in cache */

/* Private function prototypes -----------------------------------------------*/
static void USB_Command_Task(void* argument);
static USB_Command_Status_t parse_command_line(char* line, int* argc, char** argv);
static USB_Command_Status_t execute_command(int argc, char** argv);
static void print_prompt(void);
static void print_welcome(void);

/* Command handlers */
static USB_Command_Status_t cmd_help(int argc, char** argv);
static USB_Command_Status_t cmd_config(int argc, char** argv);
static USB_Command_Status_t cmd_status(int argc, char** argv);
static USB_Command_Status_t cmd_clear(int argc, char** argv);
static USB_Command_Status_t cmd_set(int argc, char** argv);
static USB_Command_Status_t cmd_cardlog(int argc, char** argv);
static USB_Command_Status_t cmd_card_init(int argc, char** argv);
static USB_Command_Status_t cmd_topup(int argc, char** argv);
static USB_Command_Status_t cmd_reset(int argc, char** argv);
static USB_Command_Status_t cmd_bootsel(int argc, char** argv);
static USB_Command_Status_t cmd_dispensestart(int argc, char** argv);
static USB_Command_Status_t cmd_dispensewait(int argc, char** argv);
static USB_Command_Status_t cmd_dispensestop(int argc, char** argv);
static USB_Command_Status_t cmd_sdwipe(int argc, char** argv);
static USB_Command_Status_t cmd_sdlist(int argc, char** argv);
static USB_Command_Status_t cmd_sdfind(int argc, char** argv);
static USB_Command_Status_t cmd_sdread(int argc, char** argv);
static USB_Command_Status_t cmd_modules(int argc, char** argv);
static USB_Command_Status_t cmd_start(int argc, char** argv);
static USB_Command_Status_t cmd_stop(int argc, char** argv);
static USB_Command_Status_t cmd_recover(int argc, char** argv);
static USB_Command_Status_t cmd_msc(int argc, char** argv);
static USB_Command_Status_t cmd_version(int argc, char** argv);
static USB_Command_Status_t cmd_factorykeys(int argc, char** argv);
static USB_Command_Status_t cmd_decryptcard(int argc, char** argv);
static USB_Command_Status_t cmd_secmode(int argc, char** argv);

/* Card command execution helpers (used for both immediate and pending execution) */
static USB_Command_Status_t USB_Command_ExecuteCardInit(void);
static USB_Command_Status_t USB_Command_ExecuteTopup(uint32_t topup_ml);
static USB_Command_Status_t USB_Command_ExecuteCardRecover(uint32_t balance_ml, const uint8_t *target_uid, uint8_t uid_length);
static USB_Command_Status_t USB_Command_ExecuteDecryptCard(void);

/* Command table -------------------------------------------------------------*/
static const command_entry_t command_table[] = {
    {"help",     cmd_help,      "Show available commands",               "help"},
    {"?",        cmd_help,      "Show all available commands",               "?"},
    {"version",  cmd_version,   "Show firmware version info",            "version"},
    {"config",   cmd_config,    "Print current configuration",           "config"},
    {"set",      cmd_set,       "Set config value (set <param> <val>)",  "set <parameter> <value>"},
    {"status",   cmd_status,    "Show system status",                    "status"},
    {"modules",  cmd_modules,   "Show module status",                    "modules"},
    {"start",    cmd_start,     "Start module (start <module>)",         "start <lcd|mifare|dispenser|buzzer|ioexp>"},
    {"stop",     cmd_stop,      "Stop module (stop <module>)",           "stop <mifare|buzzer>"},
    {"clear",    cmd_clear,     "Clear screen",                          "clear"},
    {"reset",    cmd_reset,     "Reset the Pico (software reboot)",      "reset"},
    {"bootsel",  cmd_bootsel,   "Reboot into BOOTSEL mode for flashing", "bootsel"},
    {"cardlog",  cmd_cardlog,   "Print card log (cardlog <UID_HEX>)",    "cardlog <UID_HEX>"},
    {"cardinit", cmd_card_init, "Initialize/format card (works on corrupted cards)", "cardinit"},
    {"recover",  cmd_recover,   "Recover card from SD log (recover <UID>)", "recover <UID_HEX>"},
    {"topup",    cmd_topup,     "Add volume to card (topup <ml>)",      "topup <milliliters>"},
    {"dispensestart", cmd_dispensestart, "Start dispense manually (no card)",  "dispensestart <liters>L"},
    {"dispensewait", cmd_dispensewait, "Dispense with card up to limit",  "dispensewait <liters>L"},
    {"dispensestop",  cmd_dispensestop,  "Stop dispense manually",              "dispensestop"},
    {"msc",       cmd_msc,       "USB Mass Storage mode (msc enable|disable|status)", "msc <enable|disable|status>"},
    {"sdwipe",    cmd_sdwipe,    "Delete all files on SD card",          "sdwipe"},
    {"sdlist",    cmd_sdlist,    "List files (sdlist [config|card|system])", "sdlist [type]"},
    {"sdfind",    cmd_sdfind,    "Find file by UID (sdfind <UID_HEX>)",  "sdfind <UID_HEX>"},
    {"sdread",    cmd_sdread,    "Read file (sdread <name|#>)",          "sdread <filename|#number>"},
    {"factorykeys", cmd_factorykeys, "Reset system keys to factory (config only)", "factorykeys"},
    {"decryptcard", cmd_decryptcard, "Read card (encrypted) and write back (unencrypted)", "decryptcard"},
    {"secmode",    cmd_secmode,    "Set security mode (secmode <on|off>)", "secmode <on|off>"},
};
static const size_t command_table_size = sizeof(command_table) / sizeof(command_table[0]);

/* ========================================================================== */
/*                            PUBLIC FUNCTIONS                                */
/* ========================================================================== */

void Task_Start_USB_Command_Handler(void)
{
    if (usb_command_task_handle != NULL) {
        LOG_ERROR_CMD("USB_CMD: Task already started\r\n");
        return;
    }

    BaseType_t result = xTaskCreate(
        USB_Command_Task,
        "USB_Command_Task",
        USB_COMMAND_HANDLER_TASK_STACK_WORDS,
        NULL,
        USB_COMMAND_HANDLER_TASK_PRIORITY,
        &usb_command_task_handle
    );

    if (result == pdPASS) {
        LOG_CRITICAL_CMD("[✓] USB_CMD: Task started successfully\r\n");
    } else {
        LOG_ERROR_CMD("[✗] USB_CMD: Failed to start task\r\n");
    }
}

DEFINE_STATUS_STRING_FUNCTION_SWITCH(USB_Command, USB_Command_Status_t,
    CASE_STATUS(USB_CMD_OK, "OK")
    CASE_STATUS(USB_CMD_ERROR, "Error")
    CASE_STATUS(USB_CMD_ERROR_UNKNOWN_COMMAND, "Unknown Command")
    CASE_STATUS(USB_CMD_ERROR_INVALID_PARAM, "Invalid Param")
    CASE_STATUS(USB_CMD_ERROR_NOT_INITIALIZED, "Not Initialized")
)

/* ========================================================================== */
/*                            PRIVATE FUNCTIONS                               */
/* ========================================================================== */

static void USB_Command_Task(void* argument)
{
    (void)argument;

    /* Delay 5 seconds to let all other tasks start and log their init messages first */
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    print_welcome();
    print_prompt();

    for (;;)
    {
        /* Report task health to watchdog every loop iteration */
        System_ReportTaskStatus(SYSTEM_TASK_ID_USB_COMMAND_HANDLER, true);
        
        /* Read available bytes from USB CDC */
        uint8_t rx_byte;
        size_t bytes_read = USB_CDC_Read(&rx_byte, 1);

        if (bytes_read > 0)
        {
            /* Update last receive timestamp */
            last_rx_tick = xTaskGetTickCount();
            
            /* Debug: Log received byte */
            CMD_DEBUG_PRINT("RX byte: 0x%02X ('%c')", rx_byte, (rx_byte >= 32 && rx_byte < 127) ? rx_byte : '.');
            
            /* Echo character back to terminal */
            USB_CDC_Write(&rx_byte, 1);

            /* Handle special characters */
            if (rx_byte == '\r' || rx_byte == '\n')
            {
                /* End of command line */
                USB_Log_Printf("\r\n");
                
                if (cmd_line_index > 0)
                {
                    cmd_line_buffer[cmd_line_index] = '\0';
                    
                    /* Parse and execute command */
                    int argc = 0;
                    USB_Command_Status_t parse_status = parse_command_line(cmd_line_buffer, &argc, cmd_args);
                    
                    if (parse_status == USB_CMD_OK && argc > 0)
                    {
                        execute_command(argc, cmd_args);
                    }
                    
                    /* Reset buffer */
                    cmd_line_index = 0;
                    last_rx_tick = 0;
                }
                
                print_prompt();
            }
            else if (rx_byte == '\b' || rx_byte == 127)  /* Backspace or DEL */
            {
                if (cmd_line_index > 0)
                {
                    cmd_line_index--;
                    /* Erase character on terminal: backspace, space, backspace */
                    USB_Log_Printf(" \b");
                }
            }
            else if (rx_byte >= 32 && rx_byte < 127)  /* Printable ASCII */
            {
                if (cmd_line_index < (USB_CMD_MAX_LINE_LENGTH - 1))
                {
                    cmd_line_buffer[cmd_line_index++] = (char)rx_byte;
                }
            }
        }
        else
        {
            /* No data received - check for timeout auto-execute */
            if (cmd_line_index > 0 && last_rx_tick != 0)
            {
                TickType_t elapsed = xTaskGetTickCount() - last_rx_tick;
                if (elapsed >= pdMS_TO_TICKS(CMD_AUTO_EXECUTE_TIMEOUT_MS))
                {
                    /* Timeout - auto-execute command */
                    USB_Log_Printf("\r\n");
                    cmd_line_buffer[cmd_line_index] = '\0';
                    
                    /* Parse and execute command */
                    int argc = 0;
                    USB_Command_Status_t parse_status = parse_command_line(cmd_line_buffer, &argc, cmd_args);
                    
                    if (parse_status == USB_CMD_OK && argc > 0)
                    {
                        execute_command(argc, cmd_args);
                    }
                    
                    /* Reset buffer */
                    cmd_line_index = 0;
                    last_rx_tick = 0;
                    
                    print_prompt();
                }
            }
        }

        /* Small delay to prevent tight loop */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static USB_Command_Status_t parse_command_line(char* line, int* argc, char** argv)
{
    *argc = 0;
    char* token = strtok(line, " \t");
    
    while (token != NULL && *argc < USB_CMD_MAX_ARGS)
    {
        argv[(*argc)++] = token;
        token = strtok(NULL, " \t");
    }
    
    return USB_CMD_OK;
}

static USB_Command_Status_t execute_command(int argc, char** argv)
{
    if (argc == 0 || argv[0] == NULL) {
        return USB_CMD_ERROR;
    }

    /* Search for command in table */
    for (size_t i = 0; i < command_table_size; i++)
    {
        if (strcmp(argv[0], command_table[i].name) == 0)
        {
            /* Execute command handler */
            USB_Command_Status_t status = command_table[i].handler(argc, argv);
            
            if (status != USB_CMD_OK) {
                USB_Log_Printf("[✗] Command failed: %s\r\n", USB_Command_GetStatusString(status));
            }
            
            return status;
        }
    }

    /* Command not found */
    USB_Log_Printf("[✗] Unknown command: '%s'. Type 'help' for available commands.\r\n", argv[0]);
    return USB_CMD_ERROR_UNKNOWN_COMMAND;
}

static void print_prompt(void)
{
    USB_Log_Printf("MyWota> ");
}

static void print_welcome(void)
{
    USB_Log_Printf("\r\n");
    USB_Log_Printf("╔════════════════════════════════════════════════════════════════╗\r\n");
    USB_Log_Printf("║                MyWota USB Command Interface                    ║\r\n");
    USB_Log_Printf("╚════════════════════════════════════════════════════════════════╝\r\n");
    USB_Log_Printf("Type 'help' for available commands.\r\n");
    USB_Log_Printf("\r\n");
}

/* ========================================================================== */
/*                            COMMAND HANDLERS                                */
/* ========================================================================== */

static USB_Command_Status_t cmd_help(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    USB_Log_Printf("\r\nAvailable Commands:\r\n");
    USB_Log_Printf("══════════════════════════════════════════════════════════════\r\n");
    
    for (size_t i = 0; i < command_table_size; i++)
    {
        USB_Log_Printf("  %-10s - %s\r\n", 
                      command_table[i].name, 
                      command_table[i].description);
    }
    
    USB_Log_Printf("\r\n");
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_config(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    USB_Log_Printf("\r\n");
    Config_PrintToUSB();
    USB_Log_Printf("\r\n");
    
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_status(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    USB_Log_Printf("\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("                     SYSTEM STATUS                              \r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    
    /* Get system info */
    const SystemConfig_t* config = Config_Get();
    
    USB_Log_Printf("Device ID:         %s\r\n", config->system.device_id);
    USB_Log_Printf("Site ID:           %s\r\n", config->system.site_id);
    USB_Log_Printf("Config Version:    %d\r\n", config->version);
    USB_Log_Printf("Test Mode:         %s\r\n", config->system.test_mode_enabled ? "ENABLED" : "DISABLED");
    
    /* MIFARE Card Status */
    USB_Log_Printf("\r\n--- MIFARE Card Status ---\r\n");
    MIFARE_CardState_t card_state = MIFARE_GetCardState();
    MIFARE_TransactionState_t txn_state = MIFARE_GetTransactionState();
    USB_Log_Printf("Card State:        %s (%d)\r\n", MIFARE_GetCardStateString(card_state), card_state);
    USB_Log_Printf("Transaction State: %s (%d)\r\n", MIFARE_GetTransactionStateString(txn_state), txn_state);
    USB_Log_Printf("Card Ready:        %s\r\n", MIFARE_IsCardReady() ? "YES" : "NO");
    if (MIFARE_IsCardReady()) {
        uint32_t balance_ml = MIFARE_GetBalanceMl();
        USB_Log_Printf("Volume Balance:    %lu ml\r\n", balance_ml);
    }
    
    /* FreeRTOS info */
    USB_Log_Printf("\r\n--- FreeRTOS Info ---\r\n");
    USB_Log_Printf("Free Heap:         %u bytes\r\n", (unsigned int)xPortGetFreeHeapSize());
    USB_Log_Printf("Min Free Heap:     %u bytes\r\n", (unsigned int)xPortGetMinimumEverFreeHeapSize());
    
    USB_Log_Printf("\r\n");
    
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_clear(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    /* Send ANSI escape codes to clear screen and move cursor to home */
    USB_Log_Printf("\033[2J\033[H");
    
    print_welcome();
    
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_reset(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    USB_Log_Printf("[→] Resetting Pico...\r\n");
    
    /* Small delay to allow message to be sent */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    /* Perform software reset using watchdog */
    watchdog_reboot(0, 0, 0);
    
    /* Should never reach here */
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_bootsel(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    USB_Log_Printf("[→] Rebooting into BOOTSEL mode...\r\n");
    USB_Log_Printf("[!] Device will appear as USB drive for flashing\r\n");
    
    /* Small delay to allow message to be sent */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    /* Reboot into USB boot mode (BOOTSEL) */
    /* First parameter: gpio_activity_pin_mask (0 = no activity LED) */
    /* Second parameter: disable_interface_mask (0 = enable all interfaces) */
    reset_usb_boot(0, 0);
    
    /* Should never reach here */
    return USB_CMD_OK;
}

/**
 * @brief Start dispense manually without card
 * @param argc Argument count
 * @param argv Arguments: <liters>L (e.g. "1.5L", "20L")
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_dispensestart(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("[✗] Usage: dispensestart <liters>L (e.g. 1.5L, 20L)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    /* Parse argument - must end with 'L' or 'l' */
    char *arg = argv[1];
    size_t len = strlen(arg);
    
    if (len < 2 || (arg[len-1] != 'L' && arg[len-1] != 'l')) {
        USB_Log_Printf("[✗] Invalid format. Use <liters>L (e.g. 1.5L, 20L)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    /* Parse float value (liters) */
    char liters_str[16];
    strncpy(liters_str, arg, len - 1);  /* Copy without the 'L' */
    liters_str[len - 1] = '\0';
    
    float liters = (float)atof(liters_str);
    if (liters <= 0.0f || liters > 100.0f) {
        USB_Log_Printf("[✗] Invalid volume. Range: 0.1L to 100.0L\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    /* Convert liters to ml (internal unit) */
    uint32_t target_ml = (uint32_t)(liters * 1000.0f + 0.5f);  /* Round to nearest ml */
    
    DispenserResult_t result = MIFARE_Dispenser_ManualStart(target_ml);
    
    if (result == DISPENSER_RESULT_OK) {
        USB_Log_Printf("[✓] Manual dispense started for %.1fL (%lu ml)\r\n", liters, target_ml);
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Failed to start dispense (already running?)\r\n");
        return USB_CMD_ERROR;
    }
}

/**
 * @brief Set dispense limit and wait for card validation
 * @param argc Argument count
 * @param argv Arguments (liters with 'L' suffix)
 * @return USB_Command_Status_t Command execution status
 * @note Sets volume limit, then waits for card to be scanned. Once card validates,
 *       dispense starts normally but stops at the specified volume limit.
 */
static USB_Command_Status_t cmd_dispensewait(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("[✗] Usage: dispensewait <liters>L (e.g. 1.5L, 20L)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    /* Parse argument - must end with 'L' or 'l' */
    char *arg = argv[1];
    size_t len = strlen(arg);
    
    if (len < 2 || (arg[len-1] != 'L' && arg[len-1] != 'l')) {
        USB_Log_Printf("[✗] Invalid format. Use <liters>L (e.g. 1.5L, 20L)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    /* Parse float value (liters) */
    char liters_str[16];
    strncpy(liters_str, arg, len - 1);  /* Copy without the 'L' */
    liters_str[len - 1] = '\0';
    
    float liters = (float)atof(liters_str);
    if (liters <= 0.0f || liters > 100.0f) {
        USB_Log_Printf("[✗] Invalid volume. Range: 0.1L to 100.0L\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    /* Convert liters to ml (internal unit) */
    uint32_t max_ml = (uint32_t)(liters * 1000.0f + 0.5f);  /* Round to nearest ml */
    
    DispenserResult_t result = MIFARE_Dispenser_WaitAndDispense(max_ml);
    
    if (result == DISPENSER_RESULT_OK) {
        USB_Log_Printf("[✓] Ready to dispense max %.1fL (%lu ml)\r\n", liters, max_ml);
        USB_Log_Printf("[→] Scan card to start dispense (will stop at limit)\r\n");
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Failed to start wait-and-dispense\r\n");
        return USB_CMD_ERROR;
    }
}

/**
 * @brief Stop dispense manually
 * @param argc Argument count (unused)
 * @param argv Arguments (unused)
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_dispensestop(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    DispenserResult_t result = MIFARE_Dispenser_ManualStop();
    
    if (result == DISPENSER_RESULT_OK) {
        USB_Log_Printf("[✓] Dispense stopped\r\n");
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Failed to stop dispense\r\n");
        return USB_CMD_ERROR;
    }
}

/**
 * @brief Enable/disable USB Mass Storage mode for SD card access
 * @param argc Argument count
 * @param argv Arguments: "enable" or "disable"
 * @return USB_CMD_OK on success
 * 
 * Usage:
 *   msc enable  - Expose SD card as USB drive (stops logging)
 *   msc disable - Return to normal logging mode
 *   msc         - Show current MSC status
 */
static USB_Command_Status_t cmd_msc(int argc, char** argv)
{
    if (argc < 2 || strcasecmp(argv[1], "status") == 0) {
        // No argument or "status" - show status
        USB_Log_Printf("\r\n=== USB Mass Storage Status ===\r\n");
        USB_Log_Printf("  Mode: %s\r\n", USB_MSC_GetModeString(USB_MSC_GetMode()));
        USB_Log_Printf("  SD Logger: %s\r\n", SD_Logger_IsReady() ? "Ready" : "Not Ready");
        USB_Log_Printf("  SD Card: %s\r\n", SD_IsReady() ? "Ready" : "Not Ready");
        
        // Show SD card info if available
        SD_CardInfo_t info;
        if (SD_GetCardInfo(&info) == SD_OK) {
            USB_Log_Printf("  Blocks: %lu\r\n", info.block_count);
            USB_Log_Printf("  Block Size: %u bytes\r\n", info.block_size);
            USB_Log_Printf("  Capacity: %lu MB\r\n", (info.block_count / 2048));
        } else {
            USB_Log_Printf("  SD Card Info: Not available\r\n");
        }
        
        USB_Log_Printf("\r\nUsage:\r\n");
        USB_Log_Printf("  msc enable  - Expose SD card to PC (stops logging)\r\n");
        USB_Log_Printf("  msc disable - Return to logging mode\r\n");
        USB_Log_Printf("  msc status  - Show current MSC status\r\n");
        USB_Log_Printf("\r\n");
        return USB_CMD_OK;
    }
    
    if (strcasecmp(argv[1], "enable") == 0) {
        USB_Log_Printf("\r\n[→] Enabling USB Mass Storage mode...\r\n");
        USB_Log_Printf("[!] WARNING: SD card logging will be suspended!\r\n");
        USB_Log_Printf("[!] Use 'msc disable' or safely eject on PC when done.\r\n\r\n");
        
        USB_MSC_Status_t status = USB_MSC_Enable();
        
        if (status == USB_MSC_OK) {
            USB_Log_Printf("[✓] MSC mode enabled - SD card now visible to PC\r\n");
            USB_Log_Printf("[!] Safely eject the drive before using 'msc disable'\r\n\r\n");
            return USB_CMD_OK;
        } else {
            USB_Log_Printf("[✗] Failed to enable MSC: %s\r\n\r\n", USB_MSC_GetStatusString(status));
            return USB_CMD_ERROR;
        }
    }
    else if (strcasecmp(argv[1], "disable") == 0) {
        USB_Log_Printf("\r\n[→] Disabling USB Mass Storage mode...\r\n");
        
        USB_MSC_Status_t status = USB_MSC_Disable();
        
        if (status == USB_MSC_OK) {
            USB_Log_Printf("[✓] MSC mode disabled - logging resumed\r\n\r\n");
            return USB_CMD_OK;
        } else {
            USB_Log_Printf("[✗] Failed to disable MSC: %s\r\n\r\n", USB_MSC_GetStatusString(status));
            return USB_CMD_ERROR;
        }
    }
    else {
        USB_Log_Printf("[✗] Unknown argument '%s'. Use 'enable', 'disable', or 'status'.\r\n", argv[1]);
        return USB_CMD_ERROR_INVALID_PARAM;
    }
}

/**
 * @brief Delete all files on SD card
 * @param argc Argument count (unused)
 * @param argv Arguments (unused)
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_sdwipe(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    if (!SD_Logger_IsReady()) {
        USB_Log_Printf("[✗] SD card not ready\r\n");
        return USB_CMD_ERROR;
    }
    
    USB_Log_Printf("\r\n[→] Deleting all files on SD card...\r\n");
    
    DIR dir;
    FILINFO fno;
    FRESULT res;
    uint32_t files_deleted = 0;
    uint32_t files_failed = 0;
    char filepath[64];
    
    /* Open root directory */
    res = f_opendir(&dir, "/");
    if (res != FR_OK) {
        USB_Log_Printf("[✗] Failed to open root directory (error %d)\r\n", res);
        return USB_CMD_ERROR;
    }
    
    /* Iterate through all files */
    while (1) {
        /* Feed watchdog during potentially long operation */
        System_ReportTaskStatus(SYSTEM_TASK_ID_USB_COMMAND_HANDLER, true);
        
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break;  /* Error or end of directory */
        }
        
        /* Skip directories (only delete files) */
        if (fno.fattrib & AM_DIR) {
            USB_Log_Printf("  [→] Skipping directory: %s\r\n", fno.fname);
            continue;
        }
        
        /* Build full path */
        snprintf(filepath, sizeof(filepath), "/%s", fno.fname);
        
        /* Delete the file */
        res = f_unlink(filepath);
        if (res == FR_OK) {
            USB_Log_Printf("  [✓] Deleted: %s\r\n", fno.fname);
            files_deleted++;
        } else {
            USB_Log_Printf("  [✗] Failed to delete: %s (error %d)\r\n", fno.fname, res);
            files_failed++;
        }
    }
    
    f_closedir(&dir);
    
    USB_Log_Printf("\r\n[✓] SD wipe complete: %lu files deleted", files_deleted);
    if (files_failed > 0) {
        USB_Log_Printf(", %lu failed", files_failed);
    }
    USB_Log_Printf("\r\n\r\n");
    
    return (files_failed == 0) ? USB_CMD_OK : USB_CMD_ERROR;
}

/**
 * @brief List files on SD card, optionally filtered by type
 * @param argc Argument count
 * @param argv Arguments: [type] = "config", "card", "system", or omit for all
 * @return USB_CMD_OK on success
 * 
 * File types:
 *   config - config.txt (configuration file)
 *   card   - CARD_*.log (card transaction logs)
 *   system - system_log.txt (system event log)
 */
static USB_Command_Status_t cmd_sdlist(int argc, char** argv)
{
    typedef enum {
        FILTER_ALL = 0,
        FILTER_CONFIG,
        FILTER_CARD,
        FILTER_SYSTEM
    } FileFilter_t;
    
    FileFilter_t filter = FILTER_ALL;
    const char* filter_name = "all";
    
    /* Parse filter argument */
    if (argc >= 2) {
        if (strcasecmp(argv[1], "config") == 0) {
            filter = FILTER_CONFIG;
            filter_name = "config";
        } else if (strcasecmp(argv[1], "card") == 0) {
            filter = FILTER_CARD;
            filter_name = "card logs";
        } else if (strcasecmp(argv[1], "system") == 0) {
            filter = FILTER_SYSTEM;
            filter_name = "system log";
        } else {
            USB_Log_Printf("[✗] Unknown filter type: %s\r\n", argv[1]);
            USB_Log_Printf("    Valid types: config, card, system\r\n");
            return USB_CMD_ERROR_INVALID_PARAM;
        }
    }
    
    if (!SD_Logger_IsReady()) {
        USB_Log_Printf("[✗] SD card not ready\r\n");
        return USB_CMD_ERROR;
    }
    
    USB_Log_Printf("\r\n═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("  SD Card Files (filter: %s)\r\n", filter_name);
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    
    DIR dir;
    FILINFO fno;
    FRESULT res;
    uint32_t file_count = 0;
    uint32_t total_size = 0;
    
    res = f_opendir(&dir, "/");
    if (res != FR_OK) {
        USB_Log_Printf("[✗] Failed to open root directory (error %d)\r\n", res);
        return USB_CMD_ERROR;
    }
    
    while (1) {
        System_ReportTaskStatus(SYSTEM_TASK_ID_USB_COMMAND_HANDLER, true);
        
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break;
        }
        
        /* Skip directories */
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        
        /* Apply filter */
        bool match = false;
        switch (filter) {
            case FILTER_ALL:
                match = true;
                break;
            case FILTER_CONFIG:
                /* Match config.txt */
                match = (strcasecmp(fno.fname, "config.txt") == 0);
                break;
            case FILTER_CARD:
                /* Match CARD_*.log files */
                match = (strncasecmp(fno.fname, "CARD_", 5) == 0 && 
                         strcasestr_local(fno.fname, ".log") != NULL);
                break;
            case FILTER_SYSTEM:
                /* Match system_log.txt */
                match = (strcasecmp(fno.fname, "system_log.txt") == 0);
                break;
        }
        
        if (!match) {
            continue;
        }
        
        /* Cache filename for numbered access (up to SD_FILE_LIST_MAX) */
        if (file_count < SD_FILE_LIST_MAX) {
            strncpy(sd_file_list[file_count], fno.fname, sizeof(sd_file_list[0]) - 1);
            sd_file_list[file_count][sizeof(sd_file_list[0]) - 1] = '\0';
            
            /* Print file info with number (only for cached files) */
            USB_Log_Printf("  [%2lu] %8lu  %s\r\n", file_count + 1, (unsigned long)fno.fsize, fno.fname);
        }
        
        total_size += fno.fsize;
        file_count++;
    }
    
    /* Store count for sdread command */
    sd_file_list_count = (file_count > SD_FILE_LIST_MAX) ? SD_FILE_LIST_MAX : file_count;
    
    f_closedir(&dir);
    
    USB_Log_Printf("───────────────────────────────────────────────────────────────\r\n");
    if (file_count > SD_FILE_LIST_MAX) {
        USB_Log_Printf("  Showing %d of %lu files (use filter to narrow results)\r\n", 
                      SD_FILE_LIST_MAX, file_count);
    }
    USB_Log_Printf("  %lu file(s), %lu bytes total\r\n", file_count, total_size);
    USB_Log_Printf("\r\n");
    
    return USB_CMD_OK;
}

/**
 * @brief Search for files containing a UID in the filename
 * @param argc Argument count
 * @param argv Arguments: <UID_HEX> e.g. "42680B06"
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_sdfind(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("[✗] Usage: sdfind <UID_HEX>\r\n");
        USB_Log_Printf("Example: sdfind 42680B06\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    const char* search_uid = argv[1];
    
    if (!SD_Logger_IsReady()) {
        USB_Log_Printf("[✗] SD card not ready\r\n");
        return USB_CMD_ERROR;
    }
    
    USB_Log_Printf("\r\n[→] Searching for files containing '%s'...\r\n\r\n", search_uid);
    
    DIR dir;
    FILINFO fno;
    FRESULT res;
    uint32_t match_count = 0;
    
    res = f_opendir(&dir, "/");
    if (res != FR_OK) {
        USB_Log_Printf("[✗] Failed to open root directory (error %d)\r\n", res);
        return USB_CMD_ERROR;
    }
    
    while (1) {
        System_ReportTaskStatus(SYSTEM_TASK_ID_USB_COMMAND_HANDLER, true);
        
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) {
            break;
        }
        
        /* Skip directories */
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        
        /* Case-insensitive search for UID in filename */
        if (strcasestr_local(fno.fname, search_uid) != NULL) {
            USB_Log_Printf("  [✓] %8lu  %s\r\n", (unsigned long)fno.fsize, fno.fname);
            match_count++;
        }
    }
    
    f_closedir(&dir);
    
    if (match_count == 0) {
        USB_Log_Printf("  No files found matching '%s'\r\n", search_uid);
    } else {
        USB_Log_Printf("\r\n[✓] Found %lu file(s)\r\n", match_count);
    }
    USB_Log_Printf("\r\n");
    
    return USB_CMD_OK;
}

/**
 * @brief Read and display contents of a file from SD card
 * @param argc Argument count
 * @param argv Arguments: <filename> or <#number> from sdlist
 * @return USB_CMD_OK on success
 */
static USB_Command_Status_t cmd_sdread(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("[✗] Usage: sdread <filename> or sdread <#number>\r\n");
        USB_Log_Printf("Example: sdread config.txt\r\n");
        USB_Log_Printf("Example: sdread 1  (first file from sdlist)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    if (!SD_Logger_IsReady()) {
        USB_Log_Printf("[✗] SD card not ready\r\n");
        return USB_CMD_ERROR;
    }
    
    char filepath[64];
    const char* arg = argv[1];
    
    /* Check if argument is a number (from sdlist) */
    if (arg[0] >= '1' && arg[0] <= '9') {
        uint32_t file_num = (uint32_t)atoi(arg);
        
        if (file_num == 0 || file_num > sd_file_list_count) {
            USB_Log_Printf("[✗] Invalid file number: %lu (use 1-%lu from sdlist)\r\n", 
                          file_num, sd_file_list_count);
            return USB_CMD_ERROR_INVALID_PARAM;
        }
        
        /* Use cached filename */
        snprintf(filepath, sizeof(filepath), "/%s", sd_file_list[file_num - 1]);
        USB_Log_Printf("[→] Opening file #%lu: %s\r\n", file_num, sd_file_list[file_num - 1]);
    } else {
        /* Use filename directly */
        if (arg[0] == '/') {
            snprintf(filepath, sizeof(filepath), "%s", arg);
        } else {
            snprintf(filepath, sizeof(filepath), "/%s", arg);
        }
    }
    
    FIL file;
    FRESULT res = f_open(&file, filepath, FA_READ);
    if (res != FR_OK) {
        USB_Log_Printf("[✗] Failed to open file: %s (error %d)\r\n", filepath, res);
        return USB_CMD_ERROR;
    }
    
    /* Get file size */
    FSIZE_t file_size = f_size(&file);
    
    USB_Log_Printf("\r\n═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("  File: %s (%lu bytes)\r\n", filepath + 1, (unsigned long)file_size);
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    
    /* Read and print file contents in chunks */
    char read_buffer[128];
    UINT bytes_read;
    uint32_t total_read = 0;
    
    while (1) {
        System_ReportTaskStatus(SYSTEM_TASK_ID_USB_COMMAND_HANDLER, true);
        
        res = f_read(&file, read_buffer, sizeof(read_buffer) - 1, &bytes_read);
        if (res != FR_OK || bytes_read == 0) {
            break;
        }
        
        read_buffer[bytes_read] = '\0';
        USB_Log_Printf("%s", read_buffer);
        total_read += bytes_read;
        
        /* Limit output to prevent overwhelming USB buffer */
        if (total_read > 8192) {
            USB_Log_Printf("\r\n... (truncated at 8KB, file is %lu bytes)\r\n", 
                          (unsigned long)file_size);
            break;
        }
    }
    
    f_close(&file);
    
    USB_Log_Printf("\r\n───────────────────────────────────────────────────────────────\r\n");
    USB_Log_Printf("  End of file (%lu bytes read)\r\n", total_read);
    USB_Log_Printf("\r\n");
    
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_set(int argc, char** argv)
{
    if (argc < 3) {
        USB_Log_Printf("[✗] Usage: set <parameter> <value>\r\n");
        USB_Log_Printf("Example: set dispenser.max_dispense_ml 3000\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }

    const char* param = argv[1];
    
    /* For string parameters that may contain spaces, concatenate all remaining args */
    static char value_buffer[64];
    value_buffer[0] = '\0';
    
    /* Concatenate all value arguments (argv[2] onwards) with spaces */
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            strncat(value_buffer, " ", sizeof(value_buffer) - strlen(value_buffer) - 1);
        }
        strncat(value_buffer, argv[i], sizeof(value_buffer) - strlen(value_buffer) - 1);
    }
    
    const char* value = value_buffer;
    
    /* Get mutable config pointer */
    SystemConfig_t* cfg = &g_system_config;
    bool changed = false;
    
    /* MIFARE parameters */
    if (strcmp(param, "mifare.card_timeout_ms") == 0) {
        cfg->mifare.card_timeout_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.max_retries") == 0) {
        cfg->mifare.max_retries = (uint8_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.card_removal_fail_count") == 0) {
        cfg->mifare.card_removal_fail_count = (uint8_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.stability_timeout_ms") == 0) {
        cfg->mifare.stability_timeout_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.removal_stability_ms") == 0) {
        cfg->mifare.removal_stability_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.card_init_default_balance_ml") == 0) {
        cfg->mifare.card_init_default_balance_ml = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.auto_reinit_on_corruption") == 0) {
        cfg->mifare.auto_reinit_on_corruption = (atoi(value) != 0);
        changed = true;
    }
    /* UI parameters */
    else if (strcmp(param, "ui.display_refresh_ms") == 0) {
        cfg->ui.display_refresh_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "ui.screen_switch_delay_ms") == 0) {
        cfg->ui.screen_switch_delay_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "ui.ui_hide_delay_ms") == 0) {
        cfg->ui.ui_hide_delay_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "ui.led_flash_interval_ms") == 0) {
        cfg->ui.led_flash_interval_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "ui.data_poll_interval_ms") == 0) {
        cfg->ui.data_poll_interval_ms = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "ui.init_customer_id") == 0) {
        strncpy(cfg->ui.init_customer_id, value, sizeof(cfg->ui.init_customer_id) - 1);
        cfg->ui.init_customer_id[sizeof(cfg->ui.init_customer_id) - 1] = '\0';
        changed = true;
    } else if (strcmp(param, "ui.no_card_customer_id") == 0) {
        strncpy(cfg->ui.no_card_customer_id, value, sizeof(cfg->ui.no_card_customer_id) - 1);
        cfg->ui.no_card_customer_id[sizeof(cfg->ui.no_card_customer_id) - 1] = '\0';
        changed = true;
    }
    /* Background color settings */
    else if (strcmp(param, "ui.bg_color") == 0) {
        cfg->ui.bg_color = (uint32_t)strtoul(value, NULL, 16);
        changed = true;
    } else if (strcmp(param, "ui.bg_grad_color") == 0) {
        cfg->ui.bg_grad_color = (uint32_t)strtoul(value, NULL, 16);
        changed = true;
    } else if (strcmp(param, "ui.bg_main_stop") == 0) {
        cfg->ui.bg_main_stop = (uint8_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "ui.bg_grad_stop") == 0) {
        cfg->ui.bg_grad_stop = (uint8_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "ui.title_bar_color") == 0) {
        cfg->ui.title_bar_color = (uint32_t)strtoul(value, NULL, 16);
        changed = true;
    } else if (strncmp(param, "ui.", 3) == 0) {
        /* Handle state-based UI parameters: ui.<state>.<param> */
        const char *param_after_ui = param + 3;  /* Skip "ui." */
        const char *second_dot = strchr(param_after_ui, '.');
        
        if (second_dot != NULL) {
            /* Extract state name */
            size_t state_name_len = second_dot - param_after_ui;
            char state_name[16];
            if (state_name_len < sizeof(state_name)) {
                strncpy(state_name, param_after_ui, state_name_len);
                state_name[state_name_len] = '\0';
                
                /* Parse state */
                UI_State_t state = UI_STATE_COUNT;  /* Invalid by default */
                if (strcmp(state_name, "idle") == 0) state = UI_STATE_IDLE;
                else if (strcmp(state_name, "initializing") == 0) state = UI_STATE_CARD_INITIALIZING;
                else if (strcmp(state_name, "ready") == 0) state = UI_STATE_CARD_READY;
                else if (strcmp(state_name, "dispensing") == 0) state = UI_STATE_DISPENSING;
                else if (strcmp(state_name, "error") == 0) state = UI_STATE_ERROR;
                
                if (state < UI_STATE_COUNT) {
                    /* Valid state - parse parameter name */
                    const char *param_name = second_dot + 1;
                    UI_State_Config_t *state_cfg = &cfg->ui.states[state];
                    
                    if (strcmp(param_name, "show_customer_id") == 0) {
                        state_cfg->show_customer_id = (atoi(value) != 0);
                        changed = true;
                    }
                    /* Legacy parameters removed in V8 - silently ignore */
                    else if (strcmp(param_name, "image_brightness_active") == 0 ||
                             strcmp(param_name, "image_brightness_inactive") == 0 ||
                             strcmp(param_name, "ring_opacity_active") == 0 ||
                             strcmp(param_name, "ring_opacity_inactive") == 0 ||
                             strcmp(param_name, "ring_color_vacuum_active") == 0 ||
                             strcmp(param_name, "ring_color_vacuum_inactive") == 0 ||
                             strcmp(param_name, "ring_color_brush_active") == 0 ||
                             strcmp(param_name, "ring_color_brush_inactive") == 0 ||
                             strcmp(param_name, "ring_color_pressure_active") == 0 ||
                             strcmp(param_name, "ring_color_pressure_inactive") == 0) {
                        USB_Log_Printf("Warning: Parameter '%s' removed in V8 (no longer used)\r\n", param_name);
                    }
                }
            }
        }
    }
    /* System parameters */
    else if (strcmp(param, "system.test_mode_enabled") == 0) {
        cfg->system.test_mode_enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "system.log_level") == 0) {
        cfg->system.log_level = (uint8_t)atoi(value);
        changed = true;
    }
    /* MIFARE Security parameters */
    else if (strcmp(param, "mifare.security.encryption_enabled") == 0) {
        cfg->mifare.security.encryption_enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.pbkdf2_iterations") == 0) {
        cfg->mifare.security.pbkdf2_iterations = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.security.use_custom_sector_keys") == 0) {
        cfg->mifare.security.use_custom_sector_keys = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.enable_hmac_auth") == 0) {
        cfg->mifare.security.enable_hmac_auth = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.enable_replay_protection") == 0) {
        cfg->mifare.security.enable_replay_protection = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.enable_challenge_response") == 0) {
        cfg->mifare.security.enable_challenge_response = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.max_timestamp_drift_sec") == 0) {
        cfg->mifare.security.max_timestamp_drift_sec = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.security.failed_challenge_lockout") == 0) {
        cfg->mifare.security.failed_challenge_lockout = (uint8_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "mifare.security.encrypt_user_data") == 0) {
        cfg->mifare.security.encrypt_user_data = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.encrypt_transactions") == 0) {
        cfg->mifare.security.encrypt_transactions = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.encrypt_token_cache") == 0) {
        cfg->mifare.security.encrypt_token_cache = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "mifare.security.encrypt_account_data") == 0) {
        cfg->mifare.security.encrypt_account_data = (atoi(value) != 0);
        changed = true;
    }
    /* Dispenser settings */
    else if (strcmp(param, "dispenser.dispense_duration_seconds") == 0) {
        cfg->dispenser.dispense_duration_seconds = (uint32_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "dispenser.card_removal_delay_ms") == 0) {
        cfg->dispenser.card_removal_delay_ms = (uint32_t)atoi(value);
        changed = true;
    }
    /* Module enable settings */
    else if (strcmp(param, "modules.lcd_display_enabled") == 0) {
        cfg->modules.lcd_display_enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "modules.mifare_polling_enabled") == 0) {
        cfg->modules.mifare_polling_enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "modules.dispenser_enabled") == 0) {
        cfg->modules.dispenser_enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "modules.buzzer_enabled") == 0) {
        cfg->modules.buzzer_enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "modules.io_expander_enabled") == 0) {
        cfg->modules.io_expander_enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "modules.rs485_enabled") == 0) {
        cfg->modules.rs485_enabled = (atoi(value) != 0);
        changed = true;
    }
    /* Buzzer settings */
    else if (strcmp(param, "buzzer.enabled") == 0) {
        cfg->buzzer.enabled = (atoi(value) != 0);
        changed = true;
    } else if (strcmp(param, "buzzer.default_duration_ms") == 0) {
        cfg->buzzer.default_duration_ms = (uint16_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "buzzer.double_beep_on_ms") == 0) {
        cfg->buzzer.double_beep_on_ms = (uint16_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "buzzer.double_beep_off_ms") == 0) {
        cfg->buzzer.double_beep_off_ms = (uint16_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "buzzer.card_init_beep_interval_ms") == 0) {
        cfg->buzzer.card_init_beep_interval_ms = (uint16_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "buzzer.removal_pattern_on_ms") == 0) {
        cfg->buzzer.removal_pattern_on_ms = (uint16_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "buzzer.removal_pattern_off_ms") == 0) {
        cfg->buzzer.removal_pattern_off_ms = (uint16_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "buzzer.removal_pattern_count") == 0) {
        cfg->buzzer.removal_pattern_count = (uint8_t)atoi(value);
        changed = true;
    } else if (strcmp(param, "buzzer.removal_pattern_repeat_ms") == 0) {
        cfg->buzzer.removal_pattern_repeat_ms = (uint16_t)atoi(value);
        changed = true;
    } else {
        USB_Log_Printf("[✗] Unknown parameter: %s\r\n", param);
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    if (changed) {
        /* Update CRC */
        cfg->crc32 = Config_CalculateCRC32(cfg);
        
        USB_Log_Printf("[✓] Set %s = %s\r\n", param, value);
        USB_Log_Printf("[→] Saving to SD card and flash...\r\n");
        
        /* Save to SD card */
        Config_Result_t sd_result = Config_SaveToSD();
        if (sd_result == CONFIG_OK) {
            USB_Log_Printf("[✓] Saved to SD card\r\n");
        } else {
            USB_Log_Printf("[✗] SD save failed: %s\r\n", Config_GetResultString(sd_result));
        }
        
        /* Report to watchdog between save operations */
        System_ReportTaskStatus(SYSTEM_TASK_ID_USB_COMMAND_HANDLER, true);
        
        /* Save to flash backup */
        Config_Result_t flash_result = Config_SaveToFlash();
        if (flash_result == CONFIG_OK) {
            USB_Log_Printf("[✓] Saved to flash backup\r\n");
        } else {
            USB_Log_Printf("[✗] Flash save failed: %s\r\n", Config_GetResultString(flash_result));
        }
        
        return USB_CMD_OK;
    }
    
    return USB_CMD_ERROR;
}

static USB_Command_Status_t cmd_cardlog(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("[✗] Usage: cardlog <UID_HEX>\r\n");
        USB_Log_Printf("Example: cardlog 42680B06\r\n");
        USB_Log_Printf("Example: cardlog 04A1B2C3D4E5F6\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    const char* uid_hex = argv[1];
    size_t uid_hex_len = strlen(uid_hex);
    
    // UID must be hex string of even length (4 bytes = 8 chars, or 7 bytes = 14 chars)
    if (uid_hex_len % 2 != 0) {
        USB_Log_Printf("[✗] Invalid UID format - must be even number of hex digits\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    uint8_t uid_length = uid_hex_len / 2;
    if (uid_length != 4 && uid_length != 7) {
        USB_Log_Printf("[✗] Invalid UID length - must be 4 or 7 bytes (8 or 14 hex chars)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    // Convert hex string to bytes
    uint8_t card_uid[7];
    for (uint8_t i = 0; i < uid_length; i++) {
        char hex_byte[3] = {uid_hex[i*2], uid_hex[i*2+1], '\0'};
        char* endptr;
        unsigned long byte_val = strtoul(hex_byte, &endptr, 16);
        
        if (*endptr != '\0') {
            USB_Log_Printf("[✗] Invalid hex character in UID\r\n");
            return USB_CMD_ERROR_INVALID_PARAM;
        }
        
        card_uid[i] = (uint8_t)byte_val;
    }
    
    // Print the card log
    if (!SD_Logger_PrintCardLog(card_uid, uid_length)) {
        return USB_CMD_ERROR;
    }
    
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_card_init(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    USB_Log_Printf("\r\n[→] Card Re-initialization Requested\r\n");
    
    // Get current configuration
    const SystemConfig_t* config = Config_Get();
    uint32_t init_balance_ml = config->mifare.card_init_default_balance_ml;
    
    USB_Log_Printf("[→] Default balance: %lu ml\r\n", init_balance_ml);
    
    // Set up pending command - MIFARE task will execute when card is stable
    s_pending_command.command = USB_PENDING_CMD_CARD_INIT;
    s_pending_command.param_value = 0;  // Not used for cardinit
    s_pending_command.expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(USB_CMD_PENDING_TIMEOUT_MS);
    s_pending_command.active = true;
    
    if (MIFARE_IsCardReady()) {
        USB_Log_Printf("[→] Card present - initializing...\r\n");
    } else {
        USB_Log_Printf("[→] Waiting for card... (10 second timeout)\r\n");
        USB_Log_Printf("[→] Present card to reader to initialize\r\n\r\n");
    }
    
    return USB_CMD_OK;
}

static USB_Command_Status_t cmd_topup(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("[✗] Usage: topup <milliliters>\r\n");
        USB_Log_Printf("Example: topup 5000    (add 5 liters)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    uint32_t topup_ml = (uint32_t)atoi(argv[1]);
    
    if (topup_ml == 0 || topup_ml > 100000) {
        USB_Log_Printf("[✗] Invalid volume: %lu ml (must be 1-100000 ml)\r\n", topup_ml);
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    USB_Log_Printf("\r\n[→] Topup Requested: %lu ml\r\n", topup_ml);
    
    // Set up pending command - MIFARE task will execute when card is stable
    s_pending_command.command = USB_PENDING_CMD_TOPUP;
    s_pending_command.param_value = topup_ml;
    s_pending_command.expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(USB_CMD_PENDING_TIMEOUT_MS);
    s_pending_command.active = true;
    
    if (MIFARE_IsCardReady()) {
        USB_Log_Printf("[→] Card present - adding balance...\r\n");
    } else {
        USB_Log_Printf("[→] Waiting for card... (10 second timeout)\r\n");
        USB_Log_Printf("[→] Present card to reader to add %lu ml\r\n\r\n", topup_ml);
    }
    
    return USB_CMD_OK;
}

/**
 * @brief Parse hex string to bytes
 * @param hex_str Hex string (e.g., "42680B06" or "0x42680B06")
 * @param out_bytes Output byte array
 * @param max_bytes Maximum bytes to parse
 * @return Number of bytes parsed, or 0 on error
 */
static uint8_t parse_hex_uid(const char *hex_str, uint8_t *out_bytes, uint8_t max_bytes)
{
    if (hex_str == NULL || out_bytes == NULL) return 0;
    
    // Skip "0x" or "0X" prefix if present
    if (hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        hex_str += 2;
    }
    
    size_t len = strlen(hex_str);
    if (len == 0 || len % 2 != 0 || len / 2 > max_bytes) {
        return 0;
    }
    
    uint8_t byte_count = 0;
    for (size_t i = 0; i < len; i += 2) {
        char byte_str[3] = { hex_str[i], hex_str[i+1], '\0' };
        char *end;
        unsigned long val = strtoul(byte_str, &end, 16);
        if (*end != '\0' || val > 255) {
            return 0;  // Invalid hex
        }
        out_bytes[byte_count++] = (uint8_t)val;
    }
    
    return byte_count;
}

static USB_Command_Status_t cmd_recover(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("\r\n[✗] Usage: recover <UID_HEX>\r\n");
        USB_Log_Printf("Example: recover 42680B06\r\n");
        USB_Log_Printf("         recover 0x42680B06\r\n\r\n");
        USB_Log_Printf("This command recovers a corrupted card from its SD card log.\r\n");
        USB_Log_Printf("1. Searches SD for CARD_<UID>.log\r\n");
        USB_Log_Printf("2. Finds the last logged balance\r\n");
        USB_Log_Printf("3. Waits for the matching card to be presented\r\n");
        USB_Log_Printf("4. Initializes the card with the recovered balance\r\n\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    // Parse the UID from hex string
    uint8_t target_uid[7];
    uint8_t uid_length = parse_hex_uid(argv[1], target_uid, 7);
    
    if (uid_length < 4) {
        USB_Log_Printf("[✗] Invalid UID format. Expected 4-7 hex bytes (e.g., 42680B06)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    USB_Log_Printf("\r\n═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("                    CARD RECOVERY FROM SD LOG\r\n");
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    
    USB_Log_Printf("[→] Target card UID: ");
    for (int i = 0; i < uid_length; i++) {
        USB_Log_Printf("%02X", target_uid[i]);
    }
    USB_Log_Printf("\r\n");
    
    // Try to find the balance in SD card log
    uint32_t recovered_balance_ml = 0;
    if (!SD_Logger_RecoverCardBalance(target_uid, uid_length, &recovered_balance_ml)) {
        USB_Log_Printf("[✗] Failed to recover balance from SD card\r\n");
        USB_Log_Printf("[→] Make sure CARD_");
        for (int i = 0; i < uid_length; i++) {
            USB_Log_Printf("%02X", target_uid[i]);
        }
        USB_Log_Printf(".log exists on SD card\r\n\r\n");
        return USB_CMD_ERROR;
    }
    
    USB_Log_Printf("[✓] Recovered balance: %lu ml (%.2f L)\r\n", 
                   recovered_balance_ml, recovered_balance_ml / 1000.0f);
    
    // Check if the matching card is already present
    if (MIFARE_IsCardReady()) {
        PN532_CardInfo_t card_info;
        if (MIFARE_GetCurrentCardInfo(&card_info)) {
            // Check if UID matches
            if (card_info.uid_length == uid_length && 
                memcmp(card_info.uid, target_uid, uid_length) == 0) {
                USB_Log_Printf("[→] Target card already present - recovering now...\r\n");
                return USB_Command_ExecuteCardRecover(recovered_balance_ml, target_uid, uid_length);
            } else {
                USB_Log_Printf("[!] A different card is present. Remove it and present the target card.\r\n");
            }
        }
    }
    
    // Set up pending command with recovered balance and target UID
    s_pending_command.command = USB_PENDING_CMD_RECOVER;
    s_pending_command.param_value = recovered_balance_ml;
    memcpy(s_pending_command.target_uid, target_uid, uid_length);
    s_pending_command.target_uid_length = uid_length;
    s_pending_command.expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(30000);  // 30 second timeout
    s_pending_command.active = true;
    
    USB_Log_Printf("\r\n[→] Waiting for card with UID ");
    for (int i = 0; i < uid_length; i++) {
        USB_Log_Printf("%02X", target_uid[i]);
    }
    USB_Log_Printf("... (30 second timeout)\r\n");
    USB_Log_Printf("[→] Present the corrupted card to the reader to recover it\r\n\r\n");
    
    return USB_CMD_OK;
}

/* ========================================================================== */
/*                    CARD COMMAND EXECUTION HELPERS                          */
/* ========================================================================== */

/**
 * @brief Execute card initialization (used for both immediate and pending execution)
 * @return USB_CMD_OK on success, error code otherwise
 */
static USB_Command_Status_t USB_Command_ExecuteCardInit(void)
{
    // Get current configuration
    const SystemConfig_t* config = Config_Get();
    uint32_t init_balance_ml = config->mifare.card_init_default_balance_ml;
    
    // Get card info to derive customer ID from UID
    PN532_CardInfo_t card_info;
    if (!MIFARE_GetCurrentCardInfo(&card_info)) {
        USB_Log_Printf("[✗] Failed to read card info: No Card\r\n");
        return USB_CMD_ERROR;
    }
    
    // Derive customer ID from UID
    uint64_t customer_id = 0;
    for (int i = 0; i < card_info.uid_length; i++) {
        customer_id = (customer_id << 8) | card_info.uid[i];
    }
    
    USB_Log_Printf("[→] Card UID: ");
    for (int i = 0; i < card_info.uid_length; i++) {
        USB_Log_Printf("%02X", card_info.uid[i]);
    }
    USB_Log_Printf(" (Customer ID: %llu)\r\n", (unsigned long long)customer_id);
    
    // Force re-initialization
    USB_Log_Printf("[→] Initializing card with %lu ml...\r\n", init_balance_ml);
    MIFARE_Result_t result = MIFARE_InitializeNewCustomerCard(init_balance_ml, customer_id, false);
    
    if (result == MIFARE_RESULT_OK) {
        USB_Log_Printf("[✓] Card initialized successfully!\r\n");
        USB_Log_Printf("[✓] Balance: %lu ml\r\n", init_balance_ml);
        USB_Log_Printf("[✓] Customer ID: %llu\r\n", (unsigned long long)customer_id);
        USB_Log_Printf("[→] Card initialized - remove card to complete\r\n\r\n");
        // Card initialized - transition to INITIALIZED state (waiting for removal)
        MIFARE_SetTransactionState(TRANSACTION_STATE_INITIALIZED);
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Card initialization failed: %s\r\n", MIFARE_GetResultString(result));
        USB_Log_Printf("[→] Remove card and re-insert to retry.\r\n\r\n");
        MIFARE_SetErrorState_WriteFailed();
        return USB_CMD_ERROR;
    }
}

/**
 * @brief Execute card topup (used for both immediate and pending execution)
 * @param topup_ml Volume to add in milliliters
 * @return USB_CMD_OK on success, error code otherwise
 */
static USB_Command_Status_t USB_Command_ExecuteTopup(uint32_t topup_ml)
{
    uint32_t balance_before = MIFARE_GetBalanceMl();
    
    USB_Log_Printf("[→] Adding %lu ml to card...\r\n", topup_ml);
    USB_Log_Printf("[→] Current balance: %lu ml\r\n", balance_before);
    
    DispenserResult_t result = MIFARE_Dispenser_TopupCard(topup_ml);
    
    if (result == DISPENSER_RESULT_OK) {
        uint32_t balance_after = MIFARE_GetBalanceMl();
        USB_Log_Printf("[✓] Topup successful!\r\n");
        USB_Log_Printf("[✓] New balance: %lu ml (+ %lu ml)\r\n", balance_after, topup_ml);
        USB_Log_Printf("[→] Card ready - balance updated on screen\r\n\r\n");
        // State already set to READY_AFTER_TOPUP by MIFARE_TopupCardBalance - don't override it
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Topup failed: error code %d\r\n", result);
        USB_Log_Printf("[→] Remove card and re-insert to retry.\r\n\r\n");
        MIFARE_SetErrorState_WriteFailed();
        return USB_CMD_ERROR;
    }
}

/**
 * @brief Execute card recovery from SD log (used for both immediate and pending execution)
 * @param balance_ml Balance to restore (recovered from SD card log)
 * @param target_uid Target card UID to match
 * @param uid_length Length of target UID
 * @return USB_CMD_OK on success, error code otherwise
 */
static USB_Command_Status_t USB_Command_ExecuteCardRecover(uint32_t balance_ml, const uint8_t *target_uid, uint8_t uid_length)
{
    // Get current card info
    PN532_CardInfo_t card_info;
    if (!MIFARE_GetCurrentCardInfo(&card_info)) {
        USB_Log_Printf("[✗] Failed to read card info: No Card\r\n");
        return USB_CMD_ERROR;
    }
    
    // Verify UID matches the target
    if (card_info.uid_length != uid_length || 
        memcmp(card_info.uid, target_uid, uid_length) != 0) {
        USB_Log_Printf("[✗] Card UID mismatch!\r\n");
        USB_Log_Printf("[→] Expected: ");
        for (int i = 0; i < uid_length; i++) {
            USB_Log_Printf("%02X", target_uid[i]);
        }
        USB_Log_Printf("\r\n[→] Got: ");
        for (int i = 0; i < card_info.uid_length; i++) {
            USB_Log_Printf("%02X", card_info.uid[i]);
        }
        USB_Log_Printf("\r\n");
        return USB_CMD_ERROR;
    }
    
    // Derive customer ID from UID
    uint64_t customer_id = 0;
    for (int i = 0; i < card_info.uid_length; i++) {
        customer_id = (customer_id << 8) | card_info.uid[i];
    }
    
    USB_Log_Printf("[→] Card UID verified: ");
    for (int i = 0; i < card_info.uid_length; i++) {
        USB_Log_Printf("%02X", card_info.uid[i]);
    }
    USB_Log_Printf("\r\n");
    
    // Initialize card with recovered balance
    USB_Log_Printf("[→] Recovering card with balance: %lu ml (%.2f L)\r\n", 
                   balance_ml, balance_ml / 1000.0f);
    
    MIFARE_Result_t result = MIFARE_InitializeNewCustomerCard(balance_ml, customer_id, false);
    
    if (result == MIFARE_RESULT_OK) {
        USB_Log_Printf("\r\n═══════════════════════════════════════════════════════════════\r\n");
        USB_Log_Printf("[✓] CARD RECOVERY SUCCESSFUL!\r\n");
        USB_Log_Printf("[✓] Balance restored: %lu ml (%.2f L)\r\n", balance_ml, balance_ml / 1000.0f);
        USB_Log_Printf("[✓] Customer ID: %llu\r\n", (unsigned long long)customer_id);
        USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
        USB_Log_Printf("[→] Remove card and re-insert to continue.\r\n\r\n");
        MIFARE_SetTransactionState(TRANSACTION_STATE_INITIALIZED);  // Success - show as initialized
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] Card recovery failed: %s\r\n", MIFARE_GetResultString(result));
        USB_Log_Printf("[→] Remove card and re-insert to retry.\r\n\r\n");
        MIFARE_SetErrorState_WriteFailed();
        return USB_CMD_ERROR;
    }
}

/* ========================================================================== */
/*                    PENDING COMMAND PUBLIC API                              */
/* ========================================================================== */

/**
 * @brief Get pending card command (if any and not expired)
 * @return Pointer to pending command state, or NULL if none/expired
 */
USB_PendingCommandState_t* USB_Command_GetPendingCommand(void)
{
    if (!s_pending_command.active) {
        return NULL;
    }
    
    // Check if command has expired
    if (xTaskGetTickCount() >= s_pending_command.expire_tick) {
        USB_Log_Printf("[✗] Pending command timed out\r\n\r\n");
        s_pending_command.active = false;
        s_pending_command.command = USB_PENDING_CMD_NONE;
        return NULL;
    }
    
    return &s_pending_command;
}

/**
 * @brief Clear the pending card command
 */
void USB_Command_ClearPendingCommand(void)
{
    s_pending_command.active = false;
    s_pending_command.command = USB_PENDING_CMD_NONE;
    s_pending_command.param_value = 0;
    s_pending_command.expire_tick = 0;
}

/**
 * @brief Execute a pending card command
 * @param pending Pointer to pending command state
 * @return USB_CMD_OK on success, error code otherwise
 */
USB_Command_Status_t USB_Command_ExecutePendingCommand(USB_PendingCommandState_t* pending)
{
    if (pending == NULL || !pending->active) {
        return USB_CMD_ERROR;
    }
    
    USB_Command_Status_t result = USB_CMD_ERROR;
    
    USB_Log_Printf("[→] Executing pending command...\r\n");
    
    switch (pending->command) {
        case USB_PENDING_CMD_CARD_INIT:
            result = USB_Command_ExecuteCardInit();
            break;
            
        case USB_PENDING_CMD_TOPUP:
            result = USB_Command_ExecuteTopup(pending->param_value);
            break;
            
        case USB_PENDING_CMD_RECOVER:
            result = USB_Command_ExecuteCardRecover(pending->param_value, 
                                                     pending->target_uid, 
                                                     pending->target_uid_length);
            break;
            
        case USB_PENDING_CMD_DECRYPT_CARD:
            result = USB_Command_ExecuteDecryptCard();
            break;
            
        default:
            USB_Log_Printf("[✗] Unknown pending command type\r\n");
            result = USB_CMD_ERROR;
            break;
    }
    
    // Clear the pending command after execution
    USB_Command_ClearPendingCommand();
    
    return result;
}

/* ========================================================================== */
/*                        MODULE CONTROL COMMANDS                             */
/* ========================================================================== */

/**
 * @brief Parse module name string to enum
 * @param name Module name string
 * @return Module enum or MODULE_COUNT if invalid
 */
static System_Module_t parse_module_name(const char* name)
{
    if (strcasecmp(name, "lcd") == 0 || strcasecmp(name, "lcd_display") == 0) {
        return MODULE_LCD_DISPLAY;
    }
    if (strcasecmp(name, "mifare") == 0 || strcasecmp(name, "mifare_polling") == 0 || strcasecmp(name, "nfc") == 0) {
        return MODULE_MIFARE_POLLING;
    }
    if (strcasecmp(name, "dispenser") == 0 || strcasecmp(name, "disp") == 0) {
        return MODULE_DISPENSER;
    }
    if (strcasecmp(name, "buzzer") == 0 || strcasecmp(name, "beep") == 0) {
        return MODULE_BUZZER;
    }
    if (strcasecmp(name, "ioexp") == 0 || strcasecmp(name, "io_expander") == 0 || strcasecmp(name, "gpio") == 0) {
        return MODULE_IO_EXPANDER;
    }
    if (strcasecmp(name, "rs485") == 0 || strcasecmp(name, "rs485_comm") == 0 || strcasecmp(name, "serial") == 0) {
        return MODULE_RS485;
    }
    return MODULE_COUNT;  /* Invalid */
}

/**
 * @brief Show module status
 */
static USB_Command_Status_t cmd_modules(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    System_PrintModuleStatus();
    return USB_CMD_OK;
}

/**
 * @brief Start a module
 */
static USB_Command_Status_t cmd_start(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("Usage: start <module>\r\n");
        USB_Log_Printf("Modules: lcd, mifare, dispenser, buzzer, ioexp, rs485\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    System_Module_t module = parse_module_name(argv[1]);
    if (module >= MODULE_COUNT) {
        USB_Log_Printf("[✗] Unknown module: '%s'\r\n", argv[1]);
        USB_Log_Printf("Valid modules: lcd, mifare, dispenser, buzzer, ioexp, rs485\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    bool success = System_StartModule(module);
    return success ? USB_CMD_OK : USB_CMD_ERROR;
}

/**
 * @brief Stop a module
 */
static USB_Command_Status_t cmd_stop(int argc, char** argv)
{
    if (argc < 2) {
        USB_Log_Printf("Usage: stop <module>\r\n");
        USB_Log_Printf("Stoppable modules: mifare, buzzer\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    System_Module_t module = parse_module_name(argv[1]);
    if (module >= MODULE_COUNT) {
        USB_Log_Printf("[✗] Unknown module: '%s'\r\n", argv[1]);
        USB_Log_Printf("Valid modules: mifare, buzzer (lcd, dispenser, ioexp cannot be stopped)\r\n");
        return USB_CMD_ERROR_INVALID_PARAM;
    }
    
    bool success = System_StopModule(module);
    return success ? USB_CMD_OK : USB_CMD_ERROR;
}
/**
 * @brief Show firmware version information
 */
static USB_Command_Status_t cmd_version(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    FW_PrintVersionInfo();
    return USB_CMD_OK;
}

/**
 * @brief Reset cryptographic keys to factory defaults
 */
static USB_Command_Status_t cmd_factorykeys(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    USB_Log_Printf("\r\n[] Resetting keys to factory defaults...\r\n");
    Config_Result_t result = Config_ResetKeysToFactory();
    if (result == CONFIG_OK) {
        USB_Log_Printf("[] Keys reset to factory\r\n");
        Config_SaveToFlash();
        return USB_CMD_OK;
    }
    return USB_CMD_ERROR;
}

/**
 * @brief Read encrypted card and write back unencrypted
 */
static USB_Command_Status_t cmd_decryptcard(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    
    USB_Log_Printf("\r\n[→] Card Decryption Requested\r\n");
    USB_Log_Printf("[→] Will read encrypted data and write back unencrypted\r\n");
    USB_Log_Printf("[→] All sector trailers will be reset to factory keys (0xFF...)\r\n");
    
    // Set up pending command - MIFARE task will execute when card is stable
    s_pending_command.command = USB_PENDING_CMD_DECRYPT_CARD;
    s_pending_command.param_value = 0;  // Not used for decryptcard
    s_pending_command.expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(USB_CMD_PENDING_TIMEOUT_MS);
    s_pending_command.active = true;
    
    if (MIFARE_IsCardReady()) {
        USB_Log_Printf("[→] Card present - starting decryption...\r\n");
    } else {
        USB_Log_Printf("[→] Waiting for card... (10 second timeout)\r\n");
        USB_Log_Printf("[→] Present card to reader to decrypt\r\n\r\n");
    }
    
    return USB_CMD_OK;
}

/**
 * @brief Set security mode
 */
static USB_Command_Status_t cmd_secmode(int argc, char** argv)
{
    if (argc < 2) return USB_CMD_ERROR;
    bool enable = strcmp(argv[1], "on") == 0;
    SystemConfig_t *cfg = &g_system_config;
    cfg->mifare.security.encryption_enabled = enable;
    cfg->mifare.security.enable_hmac_auth = enable;
    cfg->mifare.security.enable_replay_protection = enable;
    cfg->mifare.security.enable_challenge_response = enable;
    cfg->mifare.security.use_custom_sector_keys = enable;
    cfg->mifare.security.encrypt_user_data = enable;
    cfg->mifare.security.encrypt_transactions = enable;
    cfg->mifare.security.encrypt_token_cache = enable;
    cfg->mifare.security.encrypt_account_data = enable;
    cfg->crc32 = Config_CalculateCRC32(cfg);
    Config_SaveToSD();
    Config_SaveToFlash();
    USB_Log_Printf("[] Security: %s\r\n", enable ? "ON" : "OFF");
    return USB_CMD_OK;
}

/**
 * @brief Display current date/time
 */



/**
 * @brief Execute card decryption - decrypt all sectors including trailers
 */
static USB_Command_Status_t USB_Command_ExecuteDecryptCard(void)
{
    USB_Log_Printf("[→] Decrypting all sectors + restoring factory trailers...\r\n");
    
    PN532_CardInfo_t card_info;
    if (!MIFARE_GetCurrentCardInfo(&card_info)) {
        USB_Log_Printf("[✗] No card detected\r\n");
        MIFARE_SetErrorState_ValidationFailed();
        return USB_CMD_ERROR;
    }
    
    // Save all security settings to restore later
    SystemConfig_t* config = (SystemConfig_t*)Config_Get();
    bool orig_enc = config->mifare.security.encryption_enabled;
    bool orig_custom_keys = config->mifare.security.use_custom_sector_keys;
    bool orig_hmac = config->mifare.security.enable_hmac_auth;
    bool orig_replay = config->mifare.security.enable_replay_protection;
    bool orig_challenge = config->mifare.security.enable_challenge_response;
    
    // Enable encryption/HMAC for READING encrypted data
    config->mifare.security.encryption_enabled = true;
    config->mifare.security.enable_hmac_auth = true;
    
    // CRITICAL: Disable custom keys to enable factory key fallback for ALL sectors
    // This handles mixed-key cards (some sectors custom, some factory) from partial decrypt
    config->mifare.security.use_custom_sector_keys = false;
    
    // Read ONLY data blocks we actually use (sectors 12-15: blocks 48-63)
    // These contain: transaction log (48-51), account data (52-55), user data (56-59), primary data (60-63)
    uint8_t blocks_read = 0;
    uint8_t blocks_failed = 0;
    uint8_t decrypted_blocks[16][16];  // Only 16 blocks needed (48-63)
    bool block_valid[16] = {false};
    
    USB_Log_Printf("[→] Reading encrypted data blocks (sectors 12-15)...\r\n");
    
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t block = 48 + i;  // Blocks 48-63
        MIFARE_Result_t read_result = MIFARE_ReadBlock(block, decrypted_blocks[i]);
        
        if (read_result == MIFARE_RESULT_OK) {
            block_valid[i] = true;
            blocks_read++;
            if ((block % 4) == 3) {
                USB_Log_Printf("  Block %2d: [✓] Trailer read\r\n", block);
            } else {
                USB_Log_Printf("  Block %2d: [✓] Read + decrypted\r\n", block);
            }
        } else {
            blocks_failed++;
            USB_Log_Printf("  Block %2d: [✗] Failed (%s)\r\n", block, MIFARE_GetResultString(read_result));
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    USB_Log_Printf("\r\n[→] Summary: %d data blocks read, %d failed\r\n", blocks_read, blocks_failed);
    
    if (blocks_read == 0) {
        USB_Log_Printf("[✗] No blocks could be read\r\n");
        config->mifare.security.encryption_enabled = orig_enc;
        config->mifare.security.use_custom_sector_keys = orig_custom_keys;
        config->mifare.security.enable_hmac_auth = orig_hmac;
        config->mifare.security.enable_replay_protection = orig_replay;
        config->mifare.security.enable_challenge_response = orig_challenge;
        return USB_CMD_ERROR;
    }
    
    // Disable encryption for WRITING unencrypted data
    config->mifare.security.encryption_enabled = false;
    config->mifare.security.use_custom_sector_keys = false;
    config->mifare.security.enable_hmac_auth = false;
    config->mifare.security.enable_replay_protection = false;
    config->mifare.security.enable_challenge_response = false;
    
    USB_Log_Printf("[→] Writing factory trailers to ALL sectors...\r\n");
    
    uint8_t blocks_written = 0;
    uint8_t write_failures = 0;
    
    // Factory trailer: 0xFF keys + standard access bits
    uint8_t factory_trailer[16];
    memset(factory_trailer, 0xFF, 6);        // Key A = 0xFF...
    factory_trailer[6] = 0xFF;               // Access bits
    factory_trailer[7] = 0x07;
    factory_trailer[8] = 0x80;
    factory_trailer[9] = 0x69;
    memset(&factory_trailer[10], 0xFF, 6);   // Key B = 0xFF...
    
    uint8_t consecutive_card_removed_failures = 0;
    const uint8_t MAX_CARD_REMOVED_FAILURES = 3;  // Abort after 3 consecutive CARD_REMOVED errors
    
    // Write factory trailers to ALL sectors (blocks 3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63)
    for (uint8_t sector = 0; sector < 16; sector++) {
        uint8_t trailer_block = (sector * 4) + 3;
        
        MIFARE_Result_t write_result = MIFARE_WriteBlock(trailer_block, factory_trailer, true);
        
        if (write_result == MIFARE_RESULT_OK) {
            blocks_written++;
            consecutive_card_removed_failures = 0;
            USB_Log_Printf("  Sector %2d (block %2d): [✓] TRAILER → factory keys\r\n", sector, trailer_block);
        } else {
            write_failures++;
            USB_Log_Printf("  Sector %2d (block %2d): [✗] TRAILER failed (%s)\r\n", sector, trailer_block, MIFARE_GetResultString(write_result));
            
            if (write_result == MIFARE_RESULT_CARD_REMOVED) {
                consecutive_card_removed_failures++;
                if (consecutive_card_removed_failures >= MAX_CARD_REMOVED_FAILURES) {
                    USB_Log_Printf("\r\n[✗] Card lost - aborting after %d consecutive failures\r\n", consecutive_card_removed_failures);
                    break;
                }
            } else {
                consecutive_card_removed_failures = 0;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    
    USB_Log_Printf("\r\n[→] Writing unencrypted data blocks (sectors 12-15)...\r\n");
    
    // Write back decrypted data blocks (skip trailers, only write successfully read blocks)
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t block = 48 + i;
        
        // Skip trailers - already written above
        if ((block % 4) == 3) {
            continue;
        }
        
        // Skip blocks that failed to read
        if (!block_valid[i]) {
            continue;
        }
        
        MIFARE_Result_t write_result = MIFARE_WriteBlock(block, decrypted_blocks[i], false);
        
        if (write_result == MIFARE_RESULT_OK) {
            blocks_written++;
            consecutive_card_removed_failures = 0;
            USB_Log_Printf("  Block %2d: [✓] Written (unencrypted)\r\n", block);
        } else {
            write_failures++;
            USB_Log_Printf("  Block %2d: [✗] Write failed (%s)\r\n", block, MIFARE_GetResultString(write_result));
            
            if (write_result == MIFARE_RESULT_CARD_REMOVED) {
                consecutive_card_removed_failures++;
                if (consecutive_card_removed_failures >= MAX_CARD_REMOVED_FAILURES) {
                    USB_Log_Printf("\r\n[✗] Card lost - aborting after %d consecutive failures\r\n", consecutive_card_removed_failures);
                    break;
                }
            } else {
                consecutive_card_removed_failures = 0;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    
    // Restore all security settings
    config->mifare.security.encryption_enabled = orig_enc;
    config->mifare.security.use_custom_sector_keys = orig_custom_keys;
    config->mifare.security.enable_hmac_auth = orig_hmac;
    config->mifare.security.enable_replay_protection = orig_replay;
    config->mifare.security.enable_challenge_response = orig_challenge;
    
    USB_Log_Printf("\r\n[→] Decryption complete:\r\n");
    USB_Log_Printf("    - Blocks decrypted: %d\r\n", blocks_read);
    USB_Log_Printf("    - Blocks written: %d\r\n", blocks_written);
    USB_Log_Printf("    - Read failures: %d\r\n", blocks_failed);
    USB_Log_Printf("    - Write failures: %d\r\n", write_failures);
    
    if (blocks_written > 0) {
        USB_Log_Printf("[✓] Card decrypted + factory trailers restored\r\n");
        USB_Log_Printf("[→] Remove card and re-insert to auto-initialize.\r\n\r\n");
        
        // Transition to initialized state - polling will detect removal and go to IDLE
        MIFARE_SetTransactionState(TRANSACTION_STATE_INITIALIZED);
        
        return USB_CMD_OK;
    } else {
        USB_Log_Printf("[✗] No blocks could be written\r\n");
        USB_Log_Printf("[→] Remove card and re-insert to retry.\r\n\r\n");
        MIFARE_SetErrorState_WriteFailed();
        return USB_CMD_ERROR;
    }
}
