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
 * @file System_Config.c
 * @brief System configuration management with SD card persistence
 * @details Loads/saves configuration from SD card with timeout and defaults
 */

/* Includes ------------------------------------------------------------------*/
#include "System_Config.h"
#include "SD_SPI_Driver.h"
#include "ff.h"
#include "USB_Logging.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Pico flash includes */
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

/* Private defines -----------------------------------------------------------*/
#define LOG_DEBUG_CONFIG_EN      0
#define LOG_CRITICAL_CONFIG_EN   1
#define LOG_ERROR_CONFIG_EN      1

#if LOG_DEBUG_CONFIG_EN
    #define LOG_DEBUG_CONFIG(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_CONFIG(...)
#endif

#if LOG_CRITICAL_CONFIG_EN
    #define LOG_CRITICAL_CONFIG(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_CONFIG(...)
#endif

#if LOG_ERROR_CONFIG_EN
    #define LOG_ERROR_CONFIG(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_CONFIG(...)
#endif

/* Global configuration instance */
SystemConfig_t g_system_config;

/* Config source tracking */
typedef enum {
    CONFIG_SOURCE_DEFAULTS,
    CONFIG_SOURCE_SD_CARD,
    CONFIG_SOURCE_FLASH
} Config_Source_t;

static Config_Source_t config_source = CONFIG_SOURCE_DEFAULTS;

/* Private variables ---------------------------------------------------------*/
static FATFS config_fatfs;
static bool config_sd_mounted = false;
static bool config_params_missing = false;  /* Track if any parameters were missing during parse */

/* Private function prototypes -----------------------------------------------*/
static uint32_t config_crc32(const uint8_t *data, size_t length);
static bool config_mount_sd(void);
static void config_unmount_sd(void);
static Config_Result_t config_parse_file(FIL *file);
static Config_Result_t config_write_file(FIL *file);
static void build_config_filename(char *buffer, size_t buffer_size, uint8_t version);
static Config_Result_t config_load_with_upgrade(void);
static UI_State_t parse_ui_state_name(const char* state_name);
static const char* get_ui_state_name(UI_State_t state);
static bool parse_ui_state_param(const char* key, const char* value, UI_State_t state, uint32_t* params_found);

/* ========================================================================== */
/*                            DEFAULT VALUES                                  */
/* ========================================================================== */

/**
 * @brief Initialize configuration with default values
 */
void Config_InitDefaults(void)
{
    LOG_CRITICAL_CONFIG("[CONFIG] Initializing with default values\r\n");
    
    /* Set magic and version */
    g_system_config.magic = CONFIG_MAGIC_NUMBER;
    g_system_config.version = CONFIG_VERSION;
    
    /* ========================================================================== */
    /* SYSTEM-WIDE CONFIGURATION                                                  */
    /* ========================================================================== */
    
    /* System defaults */
    g_system_config.system.test_mode_enabled = false;
    g_system_config.system.log_level = 3;  /* Debug level */
    strncpy(g_system_config.system.device_id, "BIGYELLOW-001", sizeof(g_system_config.system.device_id) - 1);
    g_system_config.system.device_id[sizeof(g_system_config.system.device_id) - 1] = '\0';
    strncpy(g_system_config.system.site_id, "SITE-001", sizeof(g_system_config.system.site_id) - 1);
    g_system_config.system.site_id[sizeof(g_system_config.system.site_id) - 1] = '\0';
    
    /* Module enable defaults */
    g_system_config.modules.lcd_display_enabled = true;
    g_system_config.modules.mifare_polling_enabled = true;
    g_system_config.modules.dispenser_enabled = true;
    g_system_config.modules.buzzer_enabled = true;
    g_system_config.modules.io_expander_enabled = true;
    g_system_config.modules.rs485_enabled = true;
    
    /* ========================================================================== */
    /* MODULE-SPECIFIC CONFIGURATION                                               */
    /* ========================================================================== */
    
    /* MIFARE defaults */
    g_system_config.mifare.card_timeout_ms = 2000;
    g_system_config.mifare.max_retries = 3;
    g_system_config.mifare.card_removal_fail_count = 3;
    g_system_config.mifare.stability_timeout_ms = 50;
    g_system_config.mifare.removal_stability_ms = 1000;
    /* Default MIFARE key: FF FF FF FF FF FF */
    memset(g_system_config.mifare.auth_key, 0xFF, 6);
    /* Card initialization defaults */
    #if defined(BUILD_TYPE_WATER_DISPENSER)
    g_system_config.mifare.card_init_default_balance = 60000;  /* 60000 ml = 60L default */
    #else
    g_system_config.mifare.card_init_default_balance = 10;    /* 10 tokens default */
    #endif
    g_system_config.mifare.auto_reinit_on_corruption = true;      /* Enable auto-recovery */
    strncpy(g_system_config.mifare.card_init_phone_number, "07970242024", sizeof(g_system_config.mifare.card_init_phone_number) - 1);
    g_system_config.mifare.card_init_phone_number[sizeof(g_system_config.mifare.card_init_phone_number) - 1] = '\0';
    g_system_config.mifare.card_init_validity = 2;  /* CARD_VALIDITY_NORMAL */
    strncpy(g_system_config.mifare.no_card_user_id, "BigYellow", sizeof(g_system_config.mifare.no_card_user_id) - 1);
    g_system_config.mifare.no_card_user_id[sizeof(g_system_config.mifare.no_card_user_id) - 1] = '\0';
    
    /* MIFARE Security defaults (nested inside mifare struct) */
    g_system_config.mifare.security.encryption_enabled = true;  /* Enable encryption by default */
    g_system_config.mifare.security.pbkdf2_iterations = 1000;  /* 1k iterations - safe for RP2040 WDT */
    g_system_config.mifare.security.use_custom_sector_keys = true;  /* Use UID-derived keys by default */
    g_system_config.mifare.security.enable_challenge_response = false;  /* Disabled by default */
    g_system_config.mifare.security.enable_replay_protection = true;  /* Enable replay protection */
    g_system_config.mifare.security.enable_hmac_auth = true;  /* Enable HMAC authentication */
    g_system_config.mifare.security.max_timestamp_drift_sec = 86400;  /* 24 hours clock drift */
    g_system_config.mifare.security.failed_challenge_lockout = 5;  /* Lock after 5 failed attempts */
    g_system_config.mifare.security.encrypt_user_data = true;  /* Encrypt blocks 5,6 */
    g_system_config.mifare.security.encrypt_transactions = true;  /* Encrypt blocks 9,10 */
    g_system_config.mifare.security.encrypt_token_cache = true;  /* Encrypt blocks 13,14 */
    g_system_config.mifare.security.encrypt_account_data = true;  /* Encrypt block 16 */
    
    /* Generate default master keys (should be replaced in production) */
    for (int i = 0; i < 32; i++) {
        g_system_config.mifare.security.master_key[i] = (uint8_t)(0xA5 + i);  /* Default pattern */
        g_system_config.mifare.security.hmac_key[i] = (uint8_t)(0x5A + i);  /* Default pattern */
    }
    
    /* Initialize sector keys with factory defaults (0xFF...) */
    for (int i = 0; i < 4; i++) {
        memset(g_system_config.mifare.security.sector_keys_a[i], 0xFF, 6);
        memset(g_system_config.mifare.security.sector_keys_b[i], 0xFF, 6);
    }
    
    /* UI defaults - optimized for performance */
    g_system_config.ui.display_refresh_ms = 20;  /* 50Hz refresh (was 5ms = 200Hz) */
    g_system_config.ui.screen_switch_delay_ms = 4000;
    g_system_config.ui.ui_hide_delay_ms = 5000;
    g_system_config.ui.led_flash_interval_ms = 500;
    g_system_config.ui.data_poll_interval_ms = 50;  /* 20Hz data updates for smoother response */
    g_system_config.ui.lvgl_task_period_ms = 20;  /* LVGL refresh rate (20ms for complex UI) */
    g_system_config.ui.lcd_reset_delay_ms = 500;  /* LCD reset delay (default 500ms) */
    g_system_config.ui.screen_brightness_percent = 100;  /* Full brightness */
    
    strncpy(g_system_config.ui.init_customer_id, "Welcome", sizeof(g_system_config.ui.init_customer_id) - 1);
    g_system_config.ui.init_customer_id[sizeof(g_system_config.ui.init_customer_id) - 1] = '\0';
    strncpy(g_system_config.ui.no_card_customer_id, "Big Yellow", sizeof(g_system_config.ui.no_card_customer_id) - 1);
    g_system_config.ui.no_card_customer_id[sizeof(g_system_config.ui.no_card_customer_id) - 1] = '\0';
    
    /* Background color defaults (from SquareLine design) */
    g_system_config.ui.bg_color = 0x8200E1;           /* Purple (top) */
    g_system_config.ui.bg_grad_color = 0xC98330;      /* Orange (bottom) */
    g_system_config.ui.bg_main_stop = 50;             /* Gradient start position */
    g_system_config.ui.bg_grad_stop = 180;            /* Gradient end position */
    g_system_config.ui.title_bar_color = 0x00A000;    /* Green title bar */
    
    /* UI State: IDLE (no card present) - dimmed appearance */
    g_system_config.ui.states[UI_STATE_IDLE].show_customer_id = true;
    g_system_config.ui.states[UI_STATE_IDLE].image_brightness_active = 255;   /* Full brightness */
    g_system_config.ui.states[UI_STATE_IDLE].image_brightness_inactive = 77;  /* 30% opacity */
    g_system_config.ui.states[UI_STATE_IDLE].ring_opacity_active = 255;       /* Full opacity */
    g_system_config.ui.states[UI_STATE_IDLE].ring_opacity_inactive = 77;      /* 30% opacity */
    g_system_config.ui.states[UI_STATE_IDLE].ring_color_vacuum_active = 0xFF0000;     /* Red */
    g_system_config.ui.states[UI_STATE_IDLE].ring_color_vacuum_inactive = 0x4D0000;   /* Dark red */
    g_system_config.ui.states[UI_STATE_IDLE].ring_color_brush_active = 0x00FF00;      /* Green */
    g_system_config.ui.states[UI_STATE_IDLE].ring_color_brush_inactive = 0x004D00;    /* Dark green */
    g_system_config.ui.states[UI_STATE_IDLE].ring_color_pressure_active = 0x0000FF;   /* Blue */
    g_system_config.ui.states[UI_STATE_IDLE].ring_color_pressure_inactive = 0x00004D; /* Dark blue */
    
    /* UI State: CARD_INITIALIZING - same as idle */
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].show_customer_id = true;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].image_brightness_active = 255;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].image_brightness_inactive = 77;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_opacity_active = 255;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_opacity_inactive = 77;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_color_vacuum_active = 0xFF0000;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_color_vacuum_inactive = 0x4D0000;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_color_brush_active = 0x00FF00;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_color_brush_inactive = 0x004D00;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_color_pressure_active = 0x0000FF;
    g_system_config.ui.states[UI_STATE_CARD_INITIALIZING].ring_color_pressure_inactive = 0x00004D;
    
    /* UI State: CARD_READY - normal appearance */
    g_system_config.ui.states[UI_STATE_CARD_READY].show_customer_id = true;
    g_system_config.ui.states[UI_STATE_CARD_READY].image_brightness_active = 255;
    g_system_config.ui.states[UI_STATE_CARD_READY].image_brightness_inactive = 77;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_opacity_active = 255;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_opacity_inactive = 77;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_color_vacuum_active = 0xFF0000;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_color_vacuum_inactive = 0x4D0000;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_color_brush_active = 0x00FF00;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_color_brush_inactive = 0x004D00;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_color_pressure_active = 0x0000FF;
    g_system_config.ui.states[UI_STATE_CARD_READY].ring_color_pressure_inactive = 0x00004D;
    
    /* UI State: DISPENSING - bright/active appearance */
    g_system_config.ui.states[UI_STATE_DISPENSING].show_customer_id = true;
    g_system_config.ui.states[UI_STATE_DISPENSING].image_brightness_active = 255;
    g_system_config.ui.states[UI_STATE_DISPENSING].image_brightness_inactive = 128;  /* 50% for dispensing */
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_opacity_active = 255;
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_opacity_inactive = 128;      /* 50% for dispensing */
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_color_vacuum_active = 0xFF0000;
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_color_vacuum_inactive = 0x800000;   /* Medium red */
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_color_brush_active = 0x00FF00;
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_color_brush_inactive = 0x008000;    /* Medium green */
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_color_pressure_active = 0x0000FF;
    g_system_config.ui.states[UI_STATE_DISPENSING].ring_color_pressure_inactive = 0x000080; /* Medium blue */
    
    /* UI State: ERROR - muted/error appearance */
    g_system_config.ui.states[UI_STATE_ERROR].show_customer_id = true;
    g_system_config.ui.states[UI_STATE_ERROR].image_brightness_active = 200;
    g_system_config.ui.states[UI_STATE_ERROR].image_brightness_inactive = 50;
    g_system_config.ui.states[UI_STATE_ERROR].ring_opacity_active = 200;
    g_system_config.ui.states[UI_STATE_ERROR].ring_opacity_inactive = 50;
    g_system_config.ui.states[UI_STATE_ERROR].ring_color_vacuum_active = 0xFF4040;    /* Light red */
    g_system_config.ui.states[UI_STATE_ERROR].ring_color_vacuum_inactive = 0x400000;
    g_system_config.ui.states[UI_STATE_ERROR].ring_color_brush_active = 0x40FF40;     /* Light green */
    g_system_config.ui.states[UI_STATE_ERROR].ring_color_brush_inactive = 0x004000;
    g_system_config.ui.states[UI_STATE_ERROR].ring_color_pressure_active = 0x4040FF;  /* Light blue */
    g_system_config.ui.states[UI_STATE_ERROR].ring_color_pressure_inactive = 0x000040;
    
    /* Dispenser defaults */
    g_system_config.dispenser_logic.wash_duration_seconds = 1200;  /* 20 minutes */
    g_system_config.dispenser_logic.card_removal_delay_ms = 1000;  /* 1 second */
    g_system_config.dispenser_logic.loyalty_enabled = true;        /* Enable loyalty by default */
    
    #if defined(BUILD_TYPE_WATER_DISPENSER)
    g_system_config.dispenser_logic.loyalty_threshold = 50000;     /* 50L threshold */
    g_system_config.dispenser_logic.loyalty_reward = 1000;         /* 1L reward */
    #else
    g_system_config.dispenser_logic.loyalty_threshold = 5;         /* 5 washes = 1 free wash */
    g_system_config.dispenser_logic.loyalty_reward = 1;            /* 1 free wash per threshold */
    #endif
    
    /* Buzzer defaults */
    g_system_config.buzzer.enabled = true;
    g_system_config.buzzer.default_duration_ms = 200;
    g_system_config.buzzer.double_beep_on_ms = 50;
    g_system_config.buzzer.double_beep_off_ms = 50;
    g_system_config.buzzer.card_init_beep_interval_ms = 500;
    g_system_config.buzzer.removal_pattern_on_ms = 50;
    g_system_config.buzzer.removal_pattern_off_ms = 50;
    g_system_config.buzzer.removal_pattern_count = 5;       /* 5 fast beeps */
    g_system_config.buzzer.removal_pattern_repeat_ms = 1000; /* Repeat every 1s */
    
    /* SD Logger defaults */
    g_system_config.sd_logger.init_retry_delay_ms = 5000;
    g_system_config.sd_logger.mount_retry_delay_ms = 2000;
    g_system_config.sd_logger.max_retry_count = 1;
    
    /* I/O Expander defaults */
    g_system_config.io_expander.poll_rate_ms = 50;
    g_system_config.io_expander.button_debounce_count = 3;
    
    /* Hardware Bus defaults */
    g_system_config.hardware_bus.spi0_baudrate = 62500000;  /* 62.5 MHz */
    g_system_config.hardware_bus.i2c0_baudrate = 400000;     /* 400 kHz */
    g_system_config.hardware_bus.i2c1_baudrate = 400000;     /* 400 kHz */
    g_system_config.hardware_bus.i2c_timeout_us = 50000;     /* 50ms */
    
    /* Calculate CRC */
    g_system_config.crc32 = Config_CalculateCRC32(&g_system_config);
    
    LOG_DEBUG_CONFIG("[CONFIG] Defaults initialized - CRC32: 0x%08lX\r\n", g_system_config.crc32);
}

/* ========================================================================== */
/*                            SD CARD OPERATIONS                              */
/* ========================================================================== */

/**
 * @brief Mount SD card filesystem
 * @return true if mounted successfully
 */
static bool config_mount_sd(void)
{
    if (config_sd_mounted) {
        return true;
    }
    
    /* Initialize SD card hardware */
    SD_Status_t status = SD_Init();
    if (status != SD_OK) {
        LOG_ERROR_CONFIG("[CONFIG] SD card init failed: %s\r\n", SD_GetStatusString(status));
        return false;
    }
    
    /* Mount filesystem */
    FRESULT result = f_mount(&config_fatfs, "0:", 1);
    if (result != FR_OK) {
        LOG_ERROR_CONFIG("[CONFIG] Filesystem mount failed: %d\r\n", result);
        return false;
    }
    
    config_sd_mounted = true;
    LOG_DEBUG_CONFIG("[CONFIG] SD card mounted successfully\r\n");
    return true;
}

/**
 * @brief Unmount SD card filesystem
 */
static void config_unmount_sd(void)
{
    if (config_sd_mounted) {
        f_mount(NULL, "0:", 0);
        config_sd_mounted = false;
        LOG_DEBUG_CONFIG("[CONFIG] SD card unmounted\r\n");
    }
}

/**
 * @brief Load configuration from SD card (single attempt, no timeout)
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromSD(void)
{
    LOG_CRITICAL_CONFIG("[CONFIG] Loading configuration from SD card...\r\n");
    
    Config_Result_t result = CONFIG_OK;
    
    /* Try to mount SD card once */
    if (!config_mount_sd()) {
        LOG_CRITICAL_CONFIG("[CONFIG] *** Using DEFAULTS (SD card not available) ***\r\n");
        Config_InitDefaults();
        config_source = CONFIG_SOURCE_DEFAULTS;
        return CONFIG_SD_NOT_AVAILABLE;
    }
    
    /* Try to load with version upgrade support */
    result = config_load_with_upgrade();
    
    if (result == CONFIG_OK) {
        const SystemConfig_t* cfg = Config_Get();
        LOG_CRITICAL_CONFIG("[CONFIG] Loaded: Device=%s Site=%s TestMode=%s\r\n",
                           cfg->system.device_id, cfg->system.site_id,
                           cfg->system.test_mode_enabled ? "ON" : "OFF");
        config_source = CONFIG_SOURCE_SD_CARD;
        
        /* If parameters were missing, write them back to SD */
        if (config_params_missing) {
            LOG_CRITICAL_CONFIG("[CONFIG] Missing parameters detected - updating SD config file\r\n");
            Config_SaveToVersionedFile();
            config_params_missing = false;
        }
    } else if (result == CONFIG_FILE_NOT_FOUND) {
        LOG_CRITICAL_CONFIG("[CONFIG] *** Using DEFAULTS (no config file found) ***\r\n");
        Config_InitDefaults();
        result = Config_CreateDefaultFile();
        return (result == CONFIG_OK) ? CONFIG_FILE_NOT_FOUND : result;
    } else {
        LOG_CRITICAL_CONFIG("[CONFIG] *** Using DEFAULTS (load/parse failed) ***\r\n");
        Config_InitDefaults();
        
        /* Try to recreate the config file */
        Config_CreateDefaultFile();
    }
    
    return result;
}

/**
 * @brief Load configuration from already-mounted filesystem
 * @note This function assumes SD is already initialized and mounted
 *       Call this from SD Logger Task after successful mount
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromMountedFS(void)
{
    LOG_CRITICAL_CONFIG("[CONFIG] Loading configuration from mounted filesystem...\r\n");
    
    /* Mark as mounted since SD Logger already did it */
    config_sd_mounted = true;
    
    /* Try to load with version upgrade support */
    Config_Result_t result = config_load_with_upgrade();
    
    if (result == CONFIG_OK) {
        config_source = CONFIG_SOURCE_SD_CARD;
        const SystemConfig_t* cfg = Config_Get();
        LOG_CRITICAL_CONFIG("[CONFIG] *** Loaded from SD card ***\r\n");
        LOG_CRITICAL_CONFIG("[CONFIG] Device=%s Site=%s TestMode=%s\r\n",
                           cfg->system.device_id, cfg->system.site_id,
                           cfg->system.test_mode_enabled ? "ON" : "OFF");
        
        /* If parameters were missing, write them back to SD */
        if (config_params_missing) {
            LOG_CRITICAL_CONFIG("[CONFIG] Missing parameters detected - updating SD config file\r\n");
            Config_SaveToVersionedFile();
            config_params_missing = false;
        }
        
        /* Save to flash as backup */
        LOG_DEBUG_CONFIG("[CONFIG] Backing up config to flash...\r\n");
        Config_SaveToFlash();
    } else if (result == CONFIG_FILE_NOT_FOUND) {
        config_source = CONFIG_SOURCE_DEFAULTS;
        LOG_CRITICAL_CONFIG("[CONFIG] *** Using DEFAULTS (no config file on SD) ***\r\n");
        Config_InitDefaults();
        Config_SaveToVersionedFile();
    } else {
        config_source = CONFIG_SOURCE_DEFAULTS;
        LOG_CRITICAL_CONFIG("[CONFIG] *** Using DEFAULTS (load failed) ***\r\n");
        Config_InitDefaults();
        Config_SaveToVersionedFile();
    }
    
    return result;
}

/**
 * @brief Save current configuration to SD card
 * @return Config_Result_t Result of save operation
 */
Config_Result_t Config_SaveToSD(void)
{
    /* Redirect to versioned save */
    return Config_SaveToVersionedFile();
}

/**
 * @brief Check if configuration file exists on SD card
 * @return true if file exists
 */
bool Config_FileExists(void)
{
    if (!config_mount_sd()) {
        return false;
    }
    
    uint8_t version;
    return Config_FindHighestVersion(&version);
}

/**
 * @brief Create default configuration file on SD card
 * @return Config_Result_t Result of create operation
 */
Config_Result_t Config_CreateDefaultFile(void)
{
    LOG_CRITICAL_CONFIG("[CONFIG] Creating default configuration file\r\n");
    
    /* Make sure defaults are set */
    Config_InitDefaults();
    
    return Config_SaveToVersionedFile();
}

/**
 * @brief Get pointer to current configuration
 * @return Pointer to SystemConfig_t structure
 */
const SystemConfig_t* Config_Get(void)
{
    return &g_system_config;
}

/**
 * @brief Get string representation of config result
 * @param result Config result code
 * @return const char* Result string
 */
const char* Config_GetResultString(Config_Result_t result)
{
    switch (result) {
        case CONFIG_OK:                 return "OK";
        case CONFIG_SD_NOT_AVAILABLE:   return "SD Not Available";
        case CONFIG_FILE_NOT_FOUND:     return "File Not Found";
        case CONFIG_FILE_CORRUPT:       return "File Corrupt";
        case CONFIG_VERSION_MISMATCH:   return "Version Mismatch";
        case CONFIG_WRITE_ERROR:        return "Write Error";
        case CONFIG_READ_ERROR:         return "Read Error";
        case CONFIG_TIMEOUT:            return "Timeout";
        default:                        return "Unknown";
    }
}

/* ========================================================================== */
/*                            CRC32 CALCULATION                               */
/* ========================================================================== */

/**
 * @brief Calculate CRC32 for configuration data
 * @param config Pointer to config structure
 * @return uint32_t CRC32 value
 */
uint32_t Config_CalculateCRC32(const SystemConfig_t* config)
{
    /* Calculate CRC over all data except the CRC field itself */
    size_t data_size = offsetof(SystemConfig_t, crc32);
    return config_crc32((const uint8_t*)config, data_size);
}

/**
 * @brief CRC32 calculation (same algorithm as MIFARE)
 */
static uint32_t config_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }

    return ~crc;
}

/* ========================================================================== */
/*                         FILE PARSING / WRITING                             */
/* ========================================================================== */

/**
 * @brief Parse UI state name string to enum
 * @param state_name State name string (e.g., "idle", "ready", "dispensing")
 * @return UI_State_t State enum or UI_STATE_COUNT if invalid
 */
static UI_State_t parse_ui_state_name(const char* state_name)
{
    if (strcmp(state_name, "idle") == 0) return UI_STATE_IDLE;
    if (strcmp(state_name, "initializing") == 0) return UI_STATE_CARD_INITIALIZING;
    if (strcmp(state_name, "ready") == 0) return UI_STATE_CARD_READY;
    if (strcmp(state_name, "dispensing") == 0) return UI_STATE_DISPENSING;
    if (strcmp(state_name, "error") == 0) return UI_STATE_ERROR;
    return UI_STATE_COUNT;  /* Invalid */
}

/**
 * @brief Get UI state name string from enum
 * @param state UI state enum
 * @return const char* State name string
 */
static const char* get_ui_state_name(UI_State_t state)
{
    switch (state) {
        case UI_STATE_IDLE: return "idle";
        case UI_STATE_CARD_INITIALIZING: return "initializing";
        case UI_STATE_CARD_READY: return "ready";
        case UI_STATE_DISPENSING: return "dispensing";
        case UI_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

/**
 * @brief Parse UI state-specific parameter
 * @param key Parameter key after state prefix (e.g., "show_customer_id")
 * @param value Parameter value string
 * @param state UI state to configure
 * @param params_found Pointer to parameter counter
 * @return true if parameter was parsed
 */
static bool parse_ui_state_param(const char* key, const char* value, UI_State_t state, uint32_t* params_found)
{
    if (state >= UI_STATE_COUNT) return false;
    
    UI_State_Config_t* cfg = &g_system_config.ui.states[state];
    
    if (strcmp(key, "show_customer_id") == 0) {
        cfg->show_customer_id = (atoi(value) != 0);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "image_brightness_active") == 0) {
        cfg->image_brightness_active = (uint8_t)atoi(value);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "image_brightness_inactive") == 0) {
        cfg->image_brightness_inactive = (uint8_t)atoi(value);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_opacity_active") == 0) {
        cfg->ring_opacity_active = (uint8_t)atoi(value);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_opacity_inactive") == 0) {
        cfg->ring_opacity_inactive = (uint8_t)atoi(value);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_color_vacuum_active") == 0) {
        cfg->ring_color_vacuum_active = (uint32_t)strtoul(value, NULL, 16);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_color_vacuum_inactive") == 0) {
        cfg->ring_color_vacuum_inactive = (uint32_t)strtoul(value, NULL, 16);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_color_brush_active") == 0) {
        cfg->ring_color_brush_active = (uint32_t)strtoul(value, NULL, 16);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_color_brush_inactive") == 0) {
        cfg->ring_color_brush_inactive = (uint32_t)strtoul(value, NULL, 16);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_color_pressure_active") == 0) {
        cfg->ring_color_pressure_active = (uint32_t)strtoul(value, NULL, 16);
        (*params_found)++;
        return true;
    } else if (strcmp(key, "ring_color_pressure_inactive") == 0) {
        cfg->ring_color_pressure_inactive = (uint32_t)strtoul(value, NULL, 16);
        (*params_found)++;
        return true;
    }
    
    return false;
}

/**
 * @brief Parse configuration from text file
 * @param file Pointer to open file
 * @return Config_Result_t Result of parse operation
 */
static Config_Result_t config_parse_file(FIL *file)
{
    char line[128];
    char key[64];
    char value[64];
    
    LOG_DEBUG_CONFIG("[CONFIG] Parsing configuration file...\r\n");
    
    /* Initialize with defaults first */
    Config_InitDefaults();
    
    /* Assume we'll find all parameters (set to false when any key is found) */
    /* This will be set to true if we load successfully but used defaults for anything */
    config_params_missing = false;
    uint32_t params_found = 0;
    
    while (f_gets(line, sizeof(line), file) != NULL) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        
        /* Parse key=value pairs */
        if (sscanf(line, "%63[^=]=%63[^\r\n]", key, value) == 2) {
            /* Trim whitespace */
            char *k = key;
            char *v = value;
            while (*k == ' ') k++;
            while (*v == ' ') v++;
            
            /* MIFARE settings */
            if (strcmp(k, "mifare.card_timeout_ms") == 0) {
                g_system_config.mifare.card_timeout_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.max_retries") == 0) {
                g_system_config.mifare.max_retries = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.card_removal_fail_count") == 0) {
                g_system_config.mifare.card_removal_fail_count = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.stability_timeout_ms") == 0) {
                g_system_config.mifare.stability_timeout_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.removal_stability_ms") == 0) {
                g_system_config.mifare.removal_stability_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.auth_key") == 0) {
                /* Parse hex key: FFFFFFFFFFFF */
                for (int i = 0; i < 6 && v[i*2] && v[i*2+1]; i++) {
                    char hex[3] = {v[i*2], v[i*2+1], 0};
                    g_system_config.mifare.auth_key[i] = (uint8_t)strtol(hex, NULL, 16);
                }
                params_found++;
            } else if (strcmp(k, "mifare.card_init_default_balance") == 0) {
                g_system_config.mifare.card_init_default_balance = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.auto_reinit_on_corruption") == 0) {
                g_system_config.mifare.auto_reinit_on_corruption = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.card_init_phone_number") == 0) {
                strncpy(g_system_config.mifare.card_init_phone_number, v, sizeof(g_system_config.mifare.card_init_phone_number) - 1);
                g_system_config.mifare.card_init_phone_number[sizeof(g_system_config.mifare.card_init_phone_number) - 1] = '\0';
                params_found++;
            } else if (strcmp(k, "mifare.card_init_validity") == 0) {
                g_system_config.mifare.card_init_validity = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.no_card_user_id") == 0) {
                strncpy(g_system_config.mifare.no_card_user_id, v, sizeof(g_system_config.mifare.no_card_user_id) - 1);
                g_system_config.mifare.no_card_user_id[sizeof(g_system_config.mifare.no_card_user_id) - 1] = '\0';
                params_found++;
            }
            /* MIFARE Security settings */
            else if (strcmp(k, "mifare.security.encryption_enabled") == 0) {
                g_system_config.mifare.security.encryption_enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.pbkdf2_iterations") == 0) {
                g_system_config.mifare.security.pbkdf2_iterations = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.security.use_custom_sector_keys") == 0) {
                g_system_config.mifare.security.use_custom_sector_keys = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.enable_challenge_response") == 0) {
                g_system_config.mifare.security.enable_challenge_response = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.enable_replay_protection") == 0) {
                g_system_config.mifare.security.enable_replay_protection = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.enable_hmac_auth") == 0) {
                g_system_config.mifare.security.enable_hmac_auth = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.max_timestamp_drift_sec") == 0) {
                g_system_config.mifare.security.max_timestamp_drift_sec = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.security.failed_challenge_lockout") == 0) {
                g_system_config.mifare.security.failed_challenge_lockout = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "mifare.security.encrypt_user_data") == 0) {
                g_system_config.mifare.security.encrypt_user_data = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.encrypt_transactions") == 0) {
                g_system_config.mifare.security.encrypt_transactions = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.encrypt_token_cache") == 0) {
                g_system_config.mifare.security.encrypt_token_cache = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.encrypt_account_data") == 0) {
                g_system_config.mifare.security.encrypt_account_data = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "mifare.security.master_key") == 0) {
                /* Parse hex key (32 bytes): ABCDEF0123456789... */
                for (int i = 0; i < 32 && v[i*2] && v[i*2+1]; i++) {
                    char hex[3] = {v[i*2], v[i*2+1], 0};
                    g_system_config.mifare.security.master_key[i] = (uint8_t)strtol(hex, NULL, 16);
                }
                params_found++;
            } else if (strcmp(k, "mifare.security.hmac_key") == 0) {
                /* Parse hex key (32 bytes) */
                for (int i = 0; i < 32 && v[i*2] && v[i*2+1]; i++) {
                    char hex[3] = {v[i*2], v[i*2+1], 0};
                    g_system_config.mifare.security.hmac_key[i] = (uint8_t)strtol(hex, NULL, 16);
                }
                params_found++;
            }
            /* UI settings */
            else if (strcmp(k, "ui.display_refresh_ms") == 0) {
                g_system_config.ui.display_refresh_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.screen_switch_delay_ms") == 0) {
                g_system_config.ui.screen_switch_delay_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.ui_hide_delay_ms") == 0) {
                g_system_config.ui.ui_hide_delay_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.led_flash_interval_ms") == 0) {
                g_system_config.ui.led_flash_interval_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.data_poll_interval_ms") == 0) {
                g_system_config.ui.data_poll_interval_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.init_customer_id") == 0) {
                strncpy(g_system_config.ui.init_customer_id, v, sizeof(g_system_config.ui.init_customer_id) - 1);
                g_system_config.ui.init_customer_id[sizeof(g_system_config.ui.init_customer_id) - 1] = '\0';
                params_found++;
            } else if (strcmp(k, "ui.no_card_customer_id") == 0) {
                strncpy(g_system_config.ui.no_card_customer_id, v, sizeof(g_system_config.ui.no_card_customer_id) - 1);
                g_system_config.ui.no_card_customer_id[sizeof(g_system_config.ui.no_card_customer_id) - 1] = '\0';
                params_found++;
            }
            /* Background color settings */
            else if (strcmp(k, "ui.bg_color") == 0) {
                g_system_config.ui.bg_color = (uint32_t)strtoul(v, NULL, 16);
                params_found++;
            } else if (strcmp(k, "ui.bg_grad_color") == 0) {
                g_system_config.ui.bg_grad_color = (uint32_t)strtoul(v, NULL, 16);
                params_found++;
            } else if (strcmp(k, "ui.bg_main_stop") == 0) {
                g_system_config.ui.bg_main_stop = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.bg_grad_stop") == 0) {
                g_system_config.ui.bg_grad_stop = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.title_bar_color") == 0) {
                g_system_config.ui.title_bar_color = (uint32_t)strtoul(v, NULL, 16);
                params_found++;
            }
            /* UI state-specific parameters (format: ui.<state>.<param>) */
            else if (strncmp(k, "ui.", 3) == 0) {
                /* Extract state name and parameter */
                char state_name[32];
                char param_name[64];
                const char* dot_pos = strchr(k + 3, '.');
                if (dot_pos != NULL) {
                    size_t state_len = dot_pos - (k + 3);
                    if (state_len < sizeof(state_name)) {
                        strncpy(state_name, k + 3, state_len);
                        state_name[state_len] = '\0';
                        strcpy(param_name, dot_pos + 1);
                        
                        UI_State_t state = parse_ui_state_name(state_name);
                        if (state < UI_STATE_COUNT) {
                            parse_ui_state_param(param_name, v, state, &params_found);
                        }
                    }
                }
            }
            /* System settings */
            else if (strcmp(k, "system.test_mode_enabled") == 0) {
                g_system_config.system.test_mode_enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "system.log_level") == 0) {
                g_system_config.system.log_level = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "system.device_id") == 0) {
                strncpy(g_system_config.system.device_id, v, sizeof(g_system_config.system.device_id) - 1);
                params_found++;
            } else if (strcmp(k, "system.site_id") == 0) {
                strncpy(g_system_config.system.site_id, v, sizeof(g_system_config.system.site_id) - 1);
                params_found++;
            }
            /* Module enable/disable settings */
            else if (strcmp(k, "modules.lcd_display_enabled") == 0) {
                g_system_config.modules.lcd_display_enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "modules.mifare_polling_enabled") == 0) {
                g_system_config.modules.mifare_polling_enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "modules.dispenser_enabled") == 0) {
                g_system_config.modules.dispenser_enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "modules.buzzer_enabled") == 0) {
                g_system_config.modules.buzzer_enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "modules.io_expander_enabled") == 0) {
                g_system_config.modules.io_expander_enabled = (atoi(v) != 0);
                params_found++;
            }
            /* Additional UI settings */
            else if (strcmp(k, "ui.lvgl_task_period_ms") == 0) {
                g_system_config.ui.lvgl_task_period_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.lcd_reset_delay_ms") == 0) {
                g_system_config.ui.lcd_reset_delay_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "ui.screen_brightness_percent") == 0) {
                g_system_config.ui.screen_brightness_percent = (uint8_t)atoi(v);
                params_found++;
            }
            /* Dispenser settings */
            else if (strcmp(k, "dispenser.max_dispense_duration_seconds") == 0) {
                g_system_config.dispenser_logic.wash_duration_seconds = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "dispenser.card_removal_delay_ms") == 0) {
                g_system_config.dispenser_logic.card_removal_delay_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "dispenser.loyalty_enabled") == 0) {
                g_system_config.dispenser_logic.loyalty_enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "dispenser.loyalty_threshold") == 0) {
                g_system_config.dispenser_logic.loyalty_threshold = (uint32_t)strtoul(v, NULL, 10);
                params_found++;
            } else if (strcmp(k, "dispenser.loyalty_reward") == 0) {
                g_system_config.dispenser_logic.loyalty_reward = (uint32_t)strtoul(v, NULL, 10);
                params_found++;
            }
            /* Buzzer settings */
            else if (strcmp(k, "buzzer.enabled") == 0) {
                g_system_config.buzzer.enabled = (atoi(v) != 0);
                params_found++;
            } else if (strcmp(k, "buzzer.default_duration_ms") == 0) {
                g_system_config.buzzer.default_duration_ms = (uint16_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "buzzer.double_beep_on_ms") == 0) {
                g_system_config.buzzer.double_beep_on_ms = (uint16_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "buzzer.double_beep_off_ms") == 0) {
                g_system_config.buzzer.double_beep_off_ms = (uint16_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "buzzer.card_init_beep_interval_ms") == 0) {
                g_system_config.buzzer.card_init_beep_interval_ms = (uint16_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "buzzer.removal_pattern_on_ms") == 0) {
                g_system_config.buzzer.removal_pattern_on_ms = (uint16_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "buzzer.removal_pattern_off_ms") == 0) {
                g_system_config.buzzer.removal_pattern_off_ms = (uint16_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "buzzer.removal_pattern_count") == 0) {
                g_system_config.buzzer.removal_pattern_count = (uint8_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "buzzer.removal_pattern_repeat_ms") == 0) {
                g_system_config.buzzer.removal_pattern_repeat_ms = (uint16_t)atoi(v);
                params_found++;
            }
            /* SD Logger settings */
            else if (strcmp(k, "sd_logger.init_retry_delay_ms") == 0) {
                g_system_config.sd_logger.init_retry_delay_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "sd_logger.mount_retry_delay_ms") == 0) {
                g_system_config.sd_logger.mount_retry_delay_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "sd_logger.max_retry_count") == 0) {
                g_system_config.sd_logger.max_retry_count = (uint8_t)atoi(v);
                params_found++;
            }
            /* I/O Expander settings */
            else if (strcmp(k, "io_expander.poll_rate_ms") == 0) {
                g_system_config.io_expander.poll_rate_ms = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "io_expander.button_debounce_count") == 0) {
                g_system_config.io_expander.button_debounce_count = (uint8_t)atoi(v);
                params_found++;
            }
            /* Hardware Bus settings */
            else if (strcmp(k, "hardware_bus.spi0_baudrate") == 0) {
                g_system_config.hardware_bus.spi0_baudrate = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "hardware_bus.i2c0_baudrate") == 0) {
                g_system_config.hardware_bus.i2c0_baudrate = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "hardware_bus.i2c1_baudrate") == 0) {
                g_system_config.hardware_bus.i2c1_baudrate = (uint32_t)atoi(v);
                params_found++;
            } else if (strcmp(k, "hardware_bus.i2c_timeout_us") == 0) {
                g_system_config.hardware_bus.i2c_timeout_us = (uint32_t)atoi(v);
                params_found++;
            }
        }
    }
    
    /* Check if we found all expected parameters (v6)
     * MIFARE basic: 11 params
     * MIFARE Security: 14 params
     * System: 4 params
     * Modules: 5 params (lcd_display_enabled, mifare_polling_enabled, carwash_enabled,
     *   buzzer_enabled, io_expander_enabled)
     * UI global: 12 params (7 original + 5 background colors)
     * UI infrastructure: 3 params (lvgl_task_period_ms, lcd_reset_delay_ms, screen_brightness_percent)
     * UI states: 5 states * 1 param each = 5 params
     * Car wash: 2 params
     * Buzzer: 9 params
     * SD Logger: 3 params
     * I/O Expander: 2 params
     * Hardware Bus: 4 params
     * Total: 11 + 14 + 4 + 5 + 12 + 3 + 5 + 2 + 9 + 3 + 2 + 4 = 74 params
     */
    const uint32_t EXPECTED_PARAMS_V6 = 74;
    if (params_found < EXPECTED_PARAMS_V6) {
        LOG_DEBUG_CONFIG("[CONFIG] Found %lu/%lu parameters - some missing, will use defaults\r\n", 
                        params_found, EXPECTED_PARAMS_V6);
        config_params_missing = true;
    } else {
        LOG_DEBUG_CONFIG("[CONFIG] All %lu parameters found\r\n", params_found);
        config_params_missing = false;  // Explicitly mark as complete
    }
    
    /* Update CRC with loaded values */
    g_system_config.crc32 = Config_CalculateCRC32(&g_system_config);
    
    LOG_CRITICAL_CONFIG("[CONFIG] Configuration loaded successfully\r\n");
    LOG_DEBUG_CONFIG("[CONFIG]   Device ID: %s\r\n", g_system_config.system.device_id);
    LOG_DEBUG_CONFIG("[CONFIG]   Site ID: %s\r\n", g_system_config.system.site_id);
    LOG_DEBUG_CONFIG("[CONFIG]   Test Mode: %s\r\n", g_system_config.system.test_mode_enabled ? "Enabled" : "Disabled");
    
    return CONFIG_OK;
}

/**
 * @brief Write configuration to text file
 * @param file Pointer to open file
 * @return Config_Result_t Result of write operation
 */
static Config_Result_t config_write_file(FIL *file)
{
    char buf[80];  /* Smaller buffer to reduce stack usage */
    
    LOG_CRITICAL_CONFIG("[CONFIG] Writing configuration file...\r\n");
    
    /* Feed watchdog before starting file write */
    watchdog_update();
    
    /* Write header */
    LOG_DEBUG_CONFIG("[CONFIG] Writing header...\r\n");
    f_puts("# BigYellow System Configuration\r\n", file);
    f_puts("# Auto-generated - edit with care\r\n", file);
    snprintf(buf, sizeof(buf), "# Version: %d\r\n\r\n", CONFIG_VERSION);
    f_puts(buf, file);
    
    /* MIFARE settings */
    f_puts("# === MIFARE Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "mifare.card_timeout_ms=%lu\r\n", g_system_config.mifare.card_timeout_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.max_retries=%u\r\n", g_system_config.mifare.max_retries);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.card_removal_fail_count=%u\r\n", g_system_config.mifare.card_removal_fail_count);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.stability_timeout_ms=%lu\r\n", g_system_config.mifare.stability_timeout_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.removal_stability_ms=%lu\r\n", g_system_config.mifare.removal_stability_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.auth_key=%02X%02X%02X%02X%02X%02X\r\n",
             g_system_config.mifare.auth_key[0], g_system_config.mifare.auth_key[1],
             g_system_config.mifare.auth_key[2], g_system_config.mifare.auth_key[3],
             g_system_config.mifare.auth_key[4], g_system_config.mifare.auth_key[5]);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.card_init_default_balance=%lu\r\n", g_system_config.mifare.card_init_default_balance);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.auto_reinit_on_corruption=%d\r\n", g_system_config.mifare.auto_reinit_on_corruption ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.card_init_phone_number=%s\r\n", g_system_config.mifare.card_init_phone_number);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.card_init_validity=%u\r\n", g_system_config.mifare.card_init_validity);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "mifare.no_card_user_id=%s\r\n\r\n", g_system_config.mifare.no_card_user_id);
    f_puts(buf, file);
    
    /* Feed watchdog after MIFARE section */
    watchdog_update();
    
    /* UI settings */
    f_puts("# === UI Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "ui.display_refresh_ms=%lu\r\n", g_system_config.ui.display_refresh_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.screen_switch_delay_ms=%lu\r\n", g_system_config.ui.screen_switch_delay_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.ui_hide_delay_ms=%lu\r\n", g_system_config.ui.ui_hide_delay_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.led_flash_interval_ms=%lu\r\n", g_system_config.ui.led_flash_interval_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.data_poll_interval_ms=%lu\r\n", g_system_config.ui.data_poll_interval_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.init_customer_id=%s\r\n", g_system_config.ui.init_customer_id);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.no_card_customer_id=%s\r\n", g_system_config.ui.no_card_customer_id);
    f_puts(buf, file);
    
    /* Background color settings */
    f_puts("\r\n# Background Colors (hex RRGGBB format)\r\n", file);
    snprintf(buf, sizeof(buf), "ui.bg_color=%06lX\r\n", g_system_config.ui.bg_color);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.bg_grad_color=%06lX\r\n", g_system_config.ui.bg_grad_color);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.bg_main_stop=%u\r\n", g_system_config.ui.bg_main_stop);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.bg_grad_stop=%u\r\n", g_system_config.ui.bg_grad_stop);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.title_bar_color=%06lX\r\n\r\n", g_system_config.ui.title_bar_color);
    f_puts(buf, file);
    
    /* UI State Configurations */
    for (UI_State_t state = UI_STATE_IDLE; state < UI_STATE_COUNT; state++) {
        const char* state_name = get_ui_state_name(state);
        const UI_State_Config_t* cfg = &g_system_config.ui.states[state];
        
        snprintf(buf, sizeof(buf), "# UI State: %s\r\n", state_name);
        f_puts(buf, file);
        
        snprintf(buf, sizeof(buf), "ui.%s.show_customer_id=%d\r\n", state_name, cfg->show_customer_id ? 1 : 0);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.image_brightness_active=%u\r\n", state_name, cfg->image_brightness_active);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.image_brightness_inactive=%u\r\n", state_name, cfg->image_brightness_inactive);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_opacity_active=%u\r\n", state_name, cfg->ring_opacity_active);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_opacity_inactive=%u\r\n", state_name, cfg->ring_opacity_inactive);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_color_vacuum_active=%06lX\r\n", state_name, cfg->ring_color_vacuum_active);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_color_vacuum_inactive=%06lX\r\n", state_name, cfg->ring_color_vacuum_inactive);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_color_brush_active=%06lX\r\n", state_name, cfg->ring_color_brush_active);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_color_brush_inactive=%06lX\r\n", state_name, cfg->ring_color_brush_inactive);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_color_pressure_active=%06lX\r\n", state_name, cfg->ring_color_pressure_active);
        f_puts(buf, file);
        snprintf(buf, sizeof(buf), "ui.%s.ring_color_pressure_inactive=%06lX\r\n\r\n", state_name, cfg->ring_color_pressure_inactive);
        f_puts(buf, file);
    }
    
    /* System settings */
    f_puts("# === System Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "system.test_mode_enabled=%d\r\n", g_system_config.system.test_mode_enabled ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "system.log_level=%u\r\n", g_system_config.system.log_level);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "system.device_id=%s\r\n", g_system_config.system.device_id);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "system.site_id=%s\r\n\r\n", g_system_config.system.site_id);
    f_puts(buf, file);
    
    /* Module enable/disable settings */
    f_puts("# === Module Enable/Disable ===\r\n", file);
    snprintf(buf, sizeof(buf), "modules.lcd_display_enabled=%d\r\n", g_system_config.modules.lcd_display_enabled ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "modules.mifare_polling_enabled=%d\r\n", g_system_config.modules.mifare_polling_enabled ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "modules.dispenser_enabled=%d\r\n", g_system_config.modules.dispenser_enabled ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "modules.buzzer_enabled=%d\r\n", g_system_config.modules.buzzer_enabled ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "modules.io_expander_enabled=%d\r\n\r\n", g_system_config.modules.io_expander_enabled ? 1 : 0);
    f_puts(buf, file);
    
    /* Additional UI infrastructure settings */
    f_puts("# === UI Infrastructure ===\r\n", file);
    snprintf(buf, sizeof(buf), "ui.lvgl_task_period_ms=%lu\r\n", g_system_config.ui.lvgl_task_period_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.lcd_reset_delay_ms=%lu\r\n", g_system_config.ui.lcd_reset_delay_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "ui.screen_brightness_percent=%u\r\n\r\n", g_system_config.ui.screen_brightness_percent);
    f_puts(buf, file);
    
    /* Dispenser settings */
    f_puts("# === Dispenser Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "dispenser.max_dispense_duration_seconds=%lu\r\n", g_system_config.dispenser_logic.wash_duration_seconds);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "dispenser.card_removal_delay_ms=%lu\r\n", g_system_config.dispenser_logic.card_removal_delay_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "dispenser.loyalty_enabled=%d\r\n", g_system_config.dispenser_logic.loyalty_enabled ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "dispenser.loyalty_threshold=%lu\r\n", g_system_config.dispenser_logic.loyalty_threshold);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "dispenser.loyalty_reward=%lu\r\n\r\n", g_system_config.dispenser_logic.loyalty_reward);
    f_puts(buf, file);
    
    /* Buzzer settings */
    f_puts("# === Buzzer Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "buzzer.enabled=%d\r\n", g_system_config.buzzer.enabled ? 1 : 0);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.default_duration_ms=%u\r\n", g_system_config.buzzer.default_duration_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.double_beep_on_ms=%u\r\n", g_system_config.buzzer.double_beep_on_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.double_beep_off_ms=%u\r\n", g_system_config.buzzer.double_beep_off_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.card_init_beep_interval_ms=%u\r\n", g_system_config.buzzer.card_init_beep_interval_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.removal_pattern_on_ms=%u\r\n", g_system_config.buzzer.removal_pattern_on_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.removal_pattern_off_ms=%u\r\n", g_system_config.buzzer.removal_pattern_off_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.removal_pattern_count=%u\r\n", g_system_config.buzzer.removal_pattern_count);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "buzzer.removal_pattern_repeat_ms=%u\r\n\r\n", g_system_config.buzzer.removal_pattern_repeat_ms);
    f_puts(buf, file);
    
    /* SD Logger settings */
    f_puts("# === SD Logger Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "sd_logger.init_retry_delay_ms=%lu\r\n", g_system_config.sd_logger.init_retry_delay_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "sd_logger.mount_retry_delay_ms=%lu\r\n", g_system_config.sd_logger.mount_retry_delay_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "sd_logger.max_retry_count=%u\r\n\r\n", g_system_config.sd_logger.max_retry_count);
    f_puts(buf, file);
    
    /* I/O Expander settings */
    f_puts("# === I/O Expander Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "io_expander.poll_rate_ms=%lu\r\n", g_system_config.io_expander.poll_rate_ms);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "io_expander.button_debounce_count=%u\r\n\r\n", g_system_config.io_expander.button_debounce_count);
    f_puts(buf, file);
    
    /* Hardware Bus settings */
    f_puts("# === Hardware Bus Configuration ===\r\n", file);
    snprintf(buf, sizeof(buf), "hardware_bus.spi0_baudrate=%lu\r\n", g_system_config.hardware_bus.spi0_baudrate);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "hardware_bus.i2c0_baudrate=%lu\r\n", g_system_config.hardware_bus.i2c0_baudrate);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "hardware_bus.i2c1_baudrate=%lu\r\n", g_system_config.hardware_bus.i2c1_baudrate);
    f_puts(buf, file);
    snprintf(buf, sizeof(buf), "hardware_bus.i2c_timeout_us=%lu\r\n\r\n", g_system_config.hardware_bus.i2c_timeout_us);
    f_puts(buf, file);
    
    /* Write footer with CRC */
    snprintf(buf, sizeof(buf), "# CRC32: 0x%08lX\r\n", g_system_config.crc32);
    f_puts(buf, file);
    
    /* Feed watchdog before sync */
    watchdog_update();
    
    /* Sync to ensure data is written */
    f_sync(file);
    
    /* Feed watchdog after sync */
    watchdog_update();
    
    LOG_DEBUG_CONFIG("[CONFIG] Configuration file written successfully\r\n");
    return CONFIG_OK;
}

/* ========================================================================== */
/*                          FLASH STORAGE OPERATIONS                          */
/* ========================================================================== */

/**
 * @brief Save current configuration to flash memory
 * @return Config_Result_t Result of save operation
 * @note Uses direct flash operations with FreeRTOS scheduler suspended
 */
Config_Result_t Config_SaveToFlash(void)
{
    LOG_CRITICAL_CONFIG("[CONFIG] Saving configuration to flash...\r\n");
    
    /* Update CRC before saving */
    g_system_config.crc32 = Config_CalculateCRC32(&g_system_config);
    
    /* Calculate write size - must be multiple of FLASH_PAGE_SIZE (256 bytes) */
    size_t write_size = ((sizeof(SystemConfig_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
    
    /* Feed watchdog before flash operations */
    watchdog_update();
    
    /* Only disable interrupts for flash write (don't suspend scheduler to allow watchdog task to run) */
    uint32_t interrupts = save_and_disable_interrupts();
    
    /* Erase the sector first (required before write) */
    flash_range_erase(CONFIG_FLASH_OFFSET, CONFIG_FLASH_SECTOR_SIZE);
    
    /* Re-enable interrupts temporarily to allow watchdog update */
    restore_interrupts(interrupts);
    watchdog_update();
    interrupts = save_and_disable_interrupts();
    
    /* Write the config */
    flash_range_program(CONFIG_FLASH_OFFSET, (const uint8_t*)&g_system_config, write_size);
    
    /* Restore interrupts */
    restore_interrupts(interrupts);
    
    /* Feed watchdog after flash operations */
    watchdog_update();
    
    LOG_CRITICAL_CONFIG("[CONFIG] Configuration saved to flash (CRC: 0x%08lX)\r\n", g_system_config.crc32);
    return CONFIG_OK;
}

/**
 * @brief Load configuration from flash memory
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromFlash(void)
{
    LOG_DEBUG_CONFIG("[CONFIG] Loading configuration from flash...\r\n");
    
    /* Flash is memory-mapped, so we can read directly */
    const SystemConfig_t* flash_config = (const SystemConfig_t*)(XIP_BASE + CONFIG_FLASH_OFFSET);
    
    /* Check magic number */
    if (flash_config->magic != CONFIG_MAGIC_NUMBER) {
        LOG_DEBUG_CONFIG("[CONFIG] Flash config magic invalid (0x%08lX != 0x%08lX)\r\n", 
                        flash_config->magic, CONFIG_MAGIC_NUMBER);
        return CONFIG_FLASH_NOT_FOUND;
    }
    
    /* Check version */
    if (flash_config->version != CONFIG_VERSION) {
        LOG_DEBUG_CONFIG("[CONFIG] Flash config version mismatch (%d != %d)\r\n", 
                        flash_config->version, CONFIG_VERSION);
        return CONFIG_VERSION_MISMATCH;
    }
    
    /* Verify CRC */
    uint32_t stored_crc = flash_config->crc32;
    
    /* Copy to temp for CRC calculation (we need to zero out CRC field) */
    /* Use static to avoid stack overflow - SystemConfig_t is 416 bytes */
    static SystemConfig_t temp_config;
    memcpy(&temp_config, flash_config, sizeof(SystemConfig_t));
    temp_config.crc32 = 0;
    
    uint32_t calc_crc = config_crc32((const uint8_t*)&temp_config, 
                                     sizeof(SystemConfig_t) - sizeof(uint32_t));
    
    if (calc_crc != stored_crc) {
        LOG_ERROR_CONFIG("[CONFIG] Flash config CRC mismatch (0x%08lX != 0x%08lX)\r\n", 
                        calc_crc, stored_crc);
        return CONFIG_FILE_CORRUPT;
    }
    
    /* CRC valid - copy config */
    memcpy(&g_system_config, flash_config, sizeof(SystemConfig_t));
    config_source = CONFIG_SOURCE_FLASH;
    
    LOG_CRITICAL_CONFIG("[CONFIG] *** Loaded from FLASH ***\r\n");
    LOG_CRITICAL_CONFIG("[CONFIG] Device=%s Site=%s TestMode=%s\r\n",
                       g_system_config.system.device_id, 
                       g_system_config.system.site_id,
                       g_system_config.system.test_mode_enabled ? "ON" : "OFF");
    
    return CONFIG_OK;
}

/**
 * @brief Check if valid configuration exists in flash
 * @return true if valid config found in flash
 */
bool Config_FlashHasValidConfig(void)
{
    const SystemConfig_t* flash_config = (const SystemConfig_t*)(XIP_BASE + CONFIG_FLASH_OFFSET);
    
    /* Check magic and version */
    if (flash_config->magic != CONFIG_MAGIC_NUMBER || 
        flash_config->version != CONFIG_VERSION) {
        return false;
    }
    
    /* Verify CRC */
    uint32_t stored_crc = flash_config->crc32;
    SystemConfig_t temp_config;
    memcpy(&temp_config, flash_config, sizeof(SystemConfig_t));
    temp_config.crc32 = 0;
    
    uint32_t calc_crc = config_crc32((const uint8_t*)&temp_config, 
                                     sizeof(SystemConfig_t) - sizeof(uint32_t));
    
    return (calc_crc == stored_crc);
}

/* ========================================================================== */
/*                        VERSIONED FILE OPERATIONS                           */
/* ========================================================================== */

/**
 * @brief Build versioned config filename
 * @param buffer Buffer to write filename to
 * @param buffer_size Size of buffer
 * @param version Version number
 */
static void build_config_filename(char *buffer, size_t buffer_size, uint8_t version)
{
    snprintf(buffer, buffer_size, CONFIG_FILE_PATH_FORMAT, version);
}

/**
 * @brief Find the highest version config file on SD card
 * @param[out] found_version Version number found (0 if none)
 * @return true if a config file was found
 */
bool Config_FindHighestVersion(uint8_t *found_version)
{
    char filename[32];
    FILINFO fno;
    uint8_t highest = 0;
    bool found = false;
    
    /* Search from highest to lowest for efficiency */
    for (int v = CONFIG_MAX_VERSION_SEARCH; v >= 1; v--) {
        build_config_filename(filename, sizeof(filename), v);
        
        if (f_stat(filename, &fno) == FR_OK) {
            highest = v;
            found = true;
            LOG_DEBUG_CONFIG("[CONFIG] Found config file: %s\r\n", filename);
            break;  /* Found highest version, stop searching */
        }
    }
    
    if (found_version) {
        *found_version = highest;
    }
    
    return found;
}

/**
 * @brief Load configuration from specific version file
 * @param version Version number to load
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromVersionedFile(uint8_t version)
{
    char filename[32];
    build_config_filename(filename, sizeof(filename), version);
    
    LOG_DEBUG_CONFIG("[CONFIG] Loading from version file: %s\r\n", filename);
    
    FIL file;
    FRESULT fres = f_open(&file, filename, FA_READ);
    
    if (fres != FR_OK) {
        LOG_ERROR_CONFIG("[CONFIG] Failed to open %s: %d\r\n", filename, fres);
        return (fres == FR_NO_FILE) ? CONFIG_FILE_NOT_FOUND : CONFIG_READ_ERROR;
    }
    
    Config_Result_t result = config_parse_file(&file);
    f_close(&file);
    
    return result;
}

/**
 * @brief Save configuration to versioned file (current version)
 * @return Config_Result_t Result of save operation
 */
Config_Result_t Config_SaveToVersionedFile(void)
{
    char filename[32];
    build_config_filename(filename, sizeof(filename), CONFIG_VERSION);
    
    LOG_CRITICAL_CONFIG("[CONFIG] Saving to version file: %s\r\n", filename);
    
    /* If not marked as mounted, try to mount */
    if (!config_sd_mounted && !config_mount_sd()) {
        LOG_ERROR_CONFIG("[CONFIG] Cannot save - SD card not available\r\n");
        return CONFIG_SD_NOT_AVAILABLE;
    }
    
    /* Update CRC before saving */
    g_system_config.crc32 = Config_CalculateCRC32(&g_system_config);
    FIL file;
    FRESULT fres = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
    
    if (fres != FR_OK) {
        LOG_ERROR_CONFIG("[CONFIG] Failed to create %s: %d\r\n", filename, fres);
        return CONFIG_WRITE_ERROR;
    }
    
    Config_Result_t result = config_write_file(&file);
    f_close(&file);
    
    if (result == CONFIG_OK) {
        LOG_CRITICAL_CONFIG("[CONFIG] Configuration saved to %s\r\n", filename);
    }
    
    return result;
}

/**
 * @brief Load config with automatic version upgrade support
 * @return Config_Result_t Result of load operation
 * @note Loads from highest version found, writes back if params missing
 */
static Config_Result_t config_load_with_upgrade(void)
{
    uint8_t found_version = 0;
    
    /* Find highest version config file */
    if (!Config_FindHighestVersion(&found_version)) {
        LOG_DEBUG_CONFIG("[CONFIG] No config files found\r\n");
        return CONFIG_FILE_NOT_FOUND;
    }
    
    LOG_CRITICAL_CONFIG("[CONFIG] Found config version %d (current version: %d)\r\n", 
                       found_version, CONFIG_VERSION);
    
    /* Load the config */
    Config_Result_t result = Config_LoadFromVersionedFile(found_version);
    
    if (result != CONFIG_OK) {
        return result;
    }
    
    /* Check if we need to upgrade */
    if (found_version < CONFIG_VERSION) {
        LOG_CRITICAL_CONFIG("[CONFIG] Upgrading config from v%d to v%d\r\n", 
                           found_version, CONFIG_VERSION);
        config_params_missing = true;  /* Force write to update to new version */
    }
    
    return CONFIG_OK;
}

/**
 * @brief Get string describing where config was loaded from
 */
static const char* get_config_source_string(void)
{
    switch (config_source) {
        case CONFIG_SOURCE_SD_CARD: return "SD card";
        case CONFIG_SOURCE_FLASH:   return "Flash";
        case CONFIG_SOURCE_DEFAULTS:
        default:                    return "Defaults";
    }
}

/**
 * @brief Print current configuration to USB log
 */
void Config_PrintToUSB(void)
{
    const SystemConfig_t* cfg = &g_system_config;
    
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("                  SYSTEM CONFIGURATION                          \r\n");
    USB_Log_Printf("              (Source: %s)                          \r\n", get_config_source_string());
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
    USB_Log_Printf("magic=0x%08X\r\n", (unsigned int)cfg->magic);
    USB_Log_Printf("version=%d\r\n", cfg->version);
    USB_Log_Printf("crc32=0x%08X\r\n", (unsigned int)cfg->crc32);
    
    USB_Log_Printf("\r\n--- MIFARE Configuration ---\r\n");
    USB_Log_Printf("mifare.card_timeout_ms=%lu\r\n", cfg->mifare.card_timeout_ms);
    USB_Log_Printf("mifare.max_retries=%u\r\n", cfg->mifare.max_retries);
    USB_Log_Printf("mifare.card_removal_fail_count=%u\r\n", cfg->mifare.card_removal_fail_count);
    USB_Log_Printf("mifare.stability_timeout_ms=%lu\r\n", cfg->mifare.stability_timeout_ms);
    USB_Log_Printf("mifare.removal_stability_ms=%lu\r\n", cfg->mifare.removal_stability_ms);
    USB_Log_Printf("mifare.auth_key=%02X%02X%02X%02X%02X%02X\r\n",
                  cfg->mifare.auth_key[0], cfg->mifare.auth_key[1], cfg->mifare.auth_key[2],
                  cfg->mifare.auth_key[3], cfg->mifare.auth_key[4], cfg->mifare.auth_key[5]);
    USB_Log_Printf("mifare.card_init_default_balance=%lu\r\n", cfg->mifare.card_init_default_balance);
    USB_Log_Printf("mifare.auto_reinit_on_corruption=%u\r\n", cfg->mifare.auto_reinit_on_corruption ? 1 : 0);
    USB_Log_Printf("mifare.card_init_phone_number=%s\r\n", cfg->mifare.card_init_phone_number);
    USB_Log_Printf("mifare.card_init_validity=%u\r\n", cfg->mifare.card_init_validity);
    USB_Log_Printf("mifare.no_card_user_id=%s\r\n", cfg->mifare.no_card_user_id);
    
    USB_Log_Printf("\r\n--- UI Configuration ---\r\n");
    USB_Log_Printf("ui.display_refresh_ms=%lu\r\n", cfg->ui.display_refresh_ms);
    USB_Log_Printf("  [Global Timing Parameters]\r\n");
    USB_Log_Printf("  ui.screen_switch_delay_ms=%lu\r\n", cfg->ui.screen_switch_delay_ms);
    USB_Log_Printf("  ui.ui_hide_delay_ms=%lu\r\n", cfg->ui.ui_hide_delay_ms);
    USB_Log_Printf("  ui.led_flash_interval_ms=%lu\r\n", cfg->ui.led_flash_interval_ms);
    USB_Log_Printf("  ui.data_poll_interval_ms=%lu\r\n", cfg->ui.data_poll_interval_ms);
    USB_Log_Printf("\r\n");
    USB_Log_Printf("  [Default Labels]\r\n");
    USB_Log_Printf("  ui.init_customer_id=%s\r\n", cfg->ui.init_customer_id);
    USB_Log_Printf("  ui.no_card_customer_id=%s\r\n", cfg->ui.no_card_customer_id);
    USB_Log_Printf("\r\n");
    USB_Log_Printf("  [Background Colors]\r\n");
    USB_Log_Printf("  ui.bg_color=%06lX\r\n", cfg->ui.bg_color);
    USB_Log_Printf("  ui.bg_grad_color=%06lX\r\n", cfg->ui.bg_grad_color);
    USB_Log_Printf("  ui.bg_main_stop=%u\r\n", cfg->ui.bg_main_stop);
    USB_Log_Printf("  ui.bg_grad_stop=%u\r\n", cfg->ui.bg_grad_stop);
    USB_Log_Printf("  ui.title_bar_color=%06lX\r\n", cfg->ui.title_bar_color);
    
    // Print state-based UI configuration with sectioning
    for (UI_State_t state = UI_STATE_IDLE; state < UI_STATE_COUNT; state++) {
        const char *state_name = get_ui_state_name(state);
        const UI_State_Config_t *state_cfg = &cfg->ui.states[state];
        
        USB_Log_Printf("\r\n");
        USB_Log_Printf("  [UI State: %s]\r\n", state_name);
        USB_Log_Printf("  ui.%s.show_customer_id=%u\r\n", state_name, state_cfg->show_customer_id ? 1 : 0);
        USB_Log_Printf("  ui.%s.image_brightness_active=%u\r\n", state_name, state_cfg->image_brightness_active);
        USB_Log_Printf("  ui.%s.image_brightness_inactive=%u\r\n", state_name, state_cfg->image_brightness_inactive);
        USB_Log_Printf("  ui.%s.ring_opacity_active=%u\r\n", state_name, state_cfg->ring_opacity_active);
        USB_Log_Printf("  ui.%s.ring_opacity_inactive=%u\r\n", state_name, state_cfg->ring_opacity_inactive);
        USB_Log_Printf("  ui.%s.ring_color_vacuum_active=%06lX\r\n", state_name, state_cfg->ring_color_vacuum_active);
        USB_Log_Printf("  ui.%s.ring_color_vacuum_inactive=%06lX\r\n", state_name, state_cfg->ring_color_vacuum_inactive);
        USB_Log_Printf("  ui.%s.ring_color_brush_active=%06lX\r\n", state_name, state_cfg->ring_color_brush_active);
        USB_Log_Printf("  ui.%s.ring_color_brush_inactive=%06lX\r\n", state_name, state_cfg->ring_color_brush_inactive);
        USB_Log_Printf("  ui.%s.ring_color_pressure_active=%06lX\r\n", state_name, state_cfg->ring_color_pressure_active);
        USB_Log_Printf("  ui.%s.ring_color_pressure_inactive=%06lX\r\n", state_name, state_cfg->ring_color_pressure_inactive);
    }
    
    USB_Log_Printf("\r\n--- System Configuration ---\r\n");
    USB_Log_Printf("system.test_mode_enabled=%u\r\n", cfg->system.test_mode_enabled ? 1 : 0);
    USB_Log_Printf("system.log_level=%u\r\n", cfg->system.log_level);
    USB_Log_Printf("system.device_id=%s\r\n", cfg->system.device_id);
    USB_Log_Printf("system.site_id=%s\r\n", cfg->system.site_id);
    
    USB_Log_Printf("\r\n--- Module Enable/Disable ---\r\n");
    USB_Log_Printf("modules.lcd_display_enabled=%u\r\n", cfg->modules.lcd_display_enabled ? 1 : 0);
    USB_Log_Printf("modules.mifare_polling_enabled=%u\r\n", cfg->modules.mifare_polling_enabled ? 1 : 0);
    USB_Log_Printf("modules.dispenser_enabled=%u\r\n", cfg->modules.dispenser_enabled ? 1 : 0);
    USB_Log_Printf("modules.buzzer_enabled=%u\r\n", cfg->modules.buzzer_enabled ? 1 : 0);
    USB_Log_Printf("modules.io_expander_enabled=%u\r\n", cfg->modules.io_expander_enabled ? 1 : 0);
    
    USB_Log_Printf("\r\n--- UI Infrastructure ---\r\n");
    USB_Log_Printf("ui.lvgl_task_period_ms=%lu\r\n", cfg->ui.lvgl_task_period_ms);
    USB_Log_Printf("ui.lcd_reset_delay_ms=%lu\r\n", cfg->ui.lcd_reset_delay_ms);
    USB_Log_Printf("ui.screen_brightness_percent=%u\r\n", cfg->ui.screen_brightness_percent);
    
    USB_Log_Printf("\r\n--- Dispenser Configuration ---\r\n");
    USB_Log_Printf("dispenser.max_dispense_duration_seconds=%lu\r\n", cfg->dispenser_logic.wash_duration_seconds);
    USB_Log_Printf("dispenser.card_removal_delay_ms=%lu\r\n", cfg->dispenser_logic.card_removal_delay_ms);
    
    USB_Log_Printf("\r\n--- Buzzer Configuration ---\r\n");
    USB_Log_Printf("buzzer.enabled=%u\r\n", cfg->buzzer.enabled ? 1 : 0);
    USB_Log_Printf("buzzer.default_duration_ms=%u\r\n", cfg->buzzer.default_duration_ms);
    USB_Log_Printf("buzzer.double_beep_on_ms=%u\r\n", cfg->buzzer.double_beep_on_ms);
    USB_Log_Printf("buzzer.double_beep_off_ms=%u\r\n", cfg->buzzer.double_beep_off_ms);
    USB_Log_Printf("buzzer.card_init_beep_interval_ms=%u\r\n", cfg->buzzer.card_init_beep_interval_ms);
    USB_Log_Printf("buzzer.removal_pattern_on_ms=%u\r\n", cfg->buzzer.removal_pattern_on_ms);
    USB_Log_Printf("buzzer.removal_pattern_off_ms=%u\r\n", cfg->buzzer.removal_pattern_off_ms);
    USB_Log_Printf("buzzer.removal_pattern_count=%u\r\n", cfg->buzzer.removal_pattern_count);
    USB_Log_Printf("buzzer.removal_pattern_repeat_ms=%u\r\n", cfg->buzzer.removal_pattern_repeat_ms);
    
    USB_Log_Printf("\r\n--- SD Logger Configuration ---\r\n");
    USB_Log_Printf("sd_logger.init_retry_delay_ms=%lu\r\n", cfg->sd_logger.init_retry_delay_ms);
    USB_Log_Printf("sd_logger.mount_retry_delay_ms=%lu\r\n", cfg->sd_logger.mount_retry_delay_ms);
    USB_Log_Printf("sd_logger.max_retry_count=%u\r\n", cfg->sd_logger.max_retry_count);
    
    USB_Log_Printf("\r\n--- I/O Expander Configuration ---\r\n");
    USB_Log_Printf("io_expander.poll_rate_ms=%lu\r\n", cfg->io_expander.poll_rate_ms);
    USB_Log_Printf("io_expander.button_debounce_count=%u\r\n", cfg->io_expander.button_debounce_count);
    
    USB_Log_Printf("\r\n--- Hardware Bus Configuration ---\r\n");
    USB_Log_Printf("hardware_bus.spi0_baudrate=%lu\r\n", cfg->hardware_bus.spi0_baudrate);
    USB_Log_Printf("hardware_bus.i2c0_baudrate=%lu\r\n", cfg->hardware_bus.i2c0_baudrate);
    USB_Log_Printf("hardware_bus.i2c1_baudrate=%lu\r\n", cfg->hardware_bus.i2c1_baudrate);
    USB_Log_Printf("hardware_bus.i2c_timeout_us=%lu\r\n", cfg->hardware_bus.i2c_timeout_us);
    
    USB_Log_Printf("\r\n--- MIFARE Security Configuration ---\r\n");
    USB_Log_Printf("mifare.security.encryption_enabled=%u\r\n", cfg->mifare.security.encryption_enabled ? 1 : 0);
    USB_Log_Printf("mifare.security.pbkdf2_iterations=%lu\r\n", cfg->mifare.security.pbkdf2_iterations);
    USB_Log_Printf("mifare.security.use_custom_sector_keys=%u\r\n", cfg->mifare.security.use_custom_sector_keys ? 1 : 0);
    USB_Log_Printf("mifare.security.enable_hmac_auth=%u\r\n", cfg->mifare.security.enable_hmac_auth ? 1 : 0);
    USB_Log_Printf("mifare.security.enable_replay_protection=%u\r\n", cfg->mifare.security.enable_replay_protection ? 1 : 0);
    USB_Log_Printf("mifare.security.enable_challenge_response=%u\r\n", cfg->mifare.security.enable_challenge_response ? 1 : 0);
    USB_Log_Printf("mifare.security.max_timestamp_drift_sec=%lu\r\n", cfg->mifare.security.max_timestamp_drift_sec);
    USB_Log_Printf("mifare.security.failed_challenge_lockout=%u\r\n", cfg->mifare.security.failed_challenge_lockout);
    USB_Log_Printf("mifare.security.encrypt_user_data=%u\r\n", cfg->mifare.security.encrypt_user_data ? 1 : 0);
    USB_Log_Printf("mifare.security.encrypt_transactions=%u\r\n", cfg->mifare.security.encrypt_transactions ? 1 : 0);
    USB_Log_Printf("mifare.security.encrypt_token_cache=%u\r\n", cfg->mifare.security.encrypt_token_cache ? 1 : 0);
    USB_Log_Printf("mifare.security.encrypt_account_data=%u\r\n", cfg->mifare.security.encrypt_account_data ? 1 : 0);
    USB_Log_Printf("mifare.security.master_key=%02X%02X%02X%02X...%02X%02X (32 bytes)\r\n",
                  cfg->mifare.security.master_key[0], cfg->mifare.security.master_key[1],
                  cfg->mifare.security.master_key[2], cfg->mifare.security.master_key[3],
                  cfg->mifare.security.master_key[30], cfg->mifare.security.master_key[31]);
    USB_Log_Printf("mifare.security.hmac_key=%02X%02X%02X%02X...%02X%02X (32 bytes)\r\n",
                  cfg->mifare.security.hmac_key[0], cfg->mifare.security.hmac_key[1],
                  cfg->mifare.security.hmac_key[2], cfg->mifare.security.hmac_key[3],
                  cfg->mifare.security.hmac_key[30], cfg->mifare.security.hmac_key[31]);
    USB_Log_Printf("═══════════════════════════════════════════════════════════════\r\n");
}

/**
 * @brief Reset cryptographic keys to factory defaults
 * @return Config_Result_t Result of reset operation
 */
Config_Result_t Config_ResetKeysToFactory(void)
{
    // Reset master key to default pattern
    for (int i = 0; i < 32; i++) {
        g_system_config.mifare.security.master_key[i] = (uint8_t)(0xA5 + i);
    }
    
    // Reset HMAC key to default pattern
    for (int i = 0; i < 32; i++) {
        g_system_config.mifare.security.hmac_key[i] = (uint8_t)(0x5A + i);
    }
    
    // Reset sector keys to factory defaults (0xFF...)
    for (int i = 0; i < 4; i++) {
        memset(g_system_config.mifare.security.sector_keys_a[i], 0xFF, 6);
        memset(g_system_config.mifare.security.sector_keys_b[i], 0xFF, 6);
    }
    
    // Recalculate CRC
    g_system_config.crc32 = Config_CalculateCRC32(&g_system_config);
    
    // Save to both SD and flash
    Config_SaveToSD();
    Config_SaveToFlash();
    
    LOG_CRITICAL_CONFIG("[CONFIG] Cryptographic keys reset to factory defaults\r\n");
    
    return CONFIG_OK;
}
