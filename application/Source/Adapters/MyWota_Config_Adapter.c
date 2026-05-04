/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file MyWota_Config_Adapter.c
 * @brief MyWota System Configuration Adapter
 * @details Defines the configuration schema, defaults, and callbacks for MyWota
 */

/* Includes ------------------------------------------------------------------*/
#include "MyWota_Config_Adapter.h"
#include "System_Config.h"
#include "USB_Logging.h"
#include <string.h>
#include <stdio.h>

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_CONFIG_ADAPTER_EN    0

#if LOG_DEBUG_CONFIG_ADAPTER_EN
    #define LOG_DEBUG_ADAPTER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_ADAPTER(...)
#endif

/*===========================================================================*/
/*                       External Config Instance                             */
/*===========================================================================*/

/* Use the existing global config instance from System_Config.c */
extern SystemConfig_t g_system_config;

/*===========================================================================*/
/*                       Config Schema Definition                             */
/*===========================================================================*/

/**
 * @brief MyWota configuration schema
 * @note Defines all config keys and their mappings to the g_system_config structure
 */
static const Config_Key_Entry_t mywota_config_schema[] = {
    /* System settings */
    {"system.test_mode_enabled",    CONFIG_VAL_BOOL,   &g_system_config.system.test_mode_enabled,   0, "Enable test mode"},
    {"system.log_level",            CONFIG_VAL_UINT8,  &g_system_config.system.log_level,           0, "Log level (0-5)"},
    {"system.device_id",            CONFIG_VAL_STRING, &g_system_config.system.device_id,          16, "Device ID"},
    {"system.site_id",              CONFIG_VAL_STRING, &g_system_config.system.site_id,            16, "Site ID"},
    
    /* Module enable flags */
    {"modules.lcd_display_enabled",     CONFIG_VAL_BOOL, &g_system_config.modules.lcd_display_enabled,     0, "Enable LCD"},
    {"modules.mifare_polling_enabled",  CONFIG_VAL_BOOL, &g_system_config.modules.mifare_polling_enabled,  0, "Enable MIFARE"},
    {"modules.dispenser_enabled",       CONFIG_VAL_BOOL, &g_system_config.modules.dispenser_enabled,       0, "Enable dispenser"},
    {"modules.buzzer_enabled",          CONFIG_VAL_BOOL, &g_system_config.modules.buzzer_enabled,          0, "Enable buzzer"},
    {"modules.io_expander_enabled",     CONFIG_VAL_BOOL, &g_system_config.modules.io_expander_enabled,     0, "Enable IO expander"},
    {"modules.rs485_enabled",           CONFIG_VAL_BOOL, &g_system_config.modules.rs485_enabled,           0, "Enable RS485"},
    
    /* MIFARE basic settings */
    {"mifare.card_timeout_ms",         CONFIG_VAL_UINT32, &g_system_config.mifare.card_timeout_ms,         0, "Card timeout (ms)"},
    {"mifare.max_retries",             CONFIG_VAL_UINT8,  &g_system_config.mifare.max_retries,             0, "Max retries"},
    {"mifare.card_removal_fail_count", CONFIG_VAL_UINT8,  &g_system_config.mifare.card_removal_fail_count, 0, "Removal fail count"},
    {"mifare.stability_timeout_ms",    CONFIG_VAL_UINT32, &g_system_config.mifare.stability_timeout_ms,    0, "Stability timeout"},
    {"mifare.removal_stability_ms",    CONFIG_VAL_UINT32, &g_system_config.mifare.removal_stability_ms,    0, "Removal stability"},
    {"mifare.auth_key",               CONFIG_VAL_HEX_BYTES, &g_system_config.mifare.auth_key,               6, "Auth key (6 bytes)"},
    {"mifare.card_init_default_balance", CONFIG_VAL_UINT32, &g_system_config.mifare.card_init_default_balance, 0, "Default balance"},
    {"mifare.auto_reinit_on_corruption", CONFIG_VAL_BOOL, &g_system_config.mifare.auto_reinit_on_corruption, 0, "Auto reinit"},
    {"mifare.card_init_phone_number",  CONFIG_VAL_STRING, &g_system_config.mifare.card_init_phone_number,  16, "Init phone"},
    {"mifare.card_init_validity",      CONFIG_VAL_UINT8,  &g_system_config.mifare.card_init_validity,       0, "Init validity"},
    {"mifare.no_card_user_id",         CONFIG_VAL_STRING, &g_system_config.mifare.no_card_user_id,         16, "No card user"},
    
    /* MIFARE security settings */
    {"mifare.security.encryption_enabled",      CONFIG_VAL_BOOL,   &g_system_config.mifare.security.encryption_enabled,       0, "Enable encryption"},
    {"mifare.security.pbkdf2_iterations",       CONFIG_VAL_UINT32, &g_system_config.mifare.security.pbkdf2_iterations,        0, "PBKDF2 iterations"},
    {"mifare.security.use_custom_sector_keys",  CONFIG_VAL_BOOL,   &g_system_config.mifare.security.use_custom_sector_keys,   0, "Custom sector keys"},
    {"mifare.security.enable_challenge_response", CONFIG_VAL_BOOL, &g_system_config.mifare.security.enable_challenge_response, 0, "Challenge response"},
    {"mifare.security.enable_replay_protection", CONFIG_VAL_BOOL,  &g_system_config.mifare.security.enable_replay_protection,  0, "Replay protection"},
    {"mifare.security.enable_hmac_auth",        CONFIG_VAL_BOOL,   &g_system_config.mifare.security.enable_hmac_auth,         0, "HMAC auth"},
    {"mifare.security.max_timestamp_drift_sec", CONFIG_VAL_UINT32, &g_system_config.mifare.security.max_timestamp_drift_sec,  0, "Max timestamp drift"},
    {"mifare.security.failed_challenge_lockout", CONFIG_VAL_UINT8, &g_system_config.mifare.security.failed_challenge_lockout, 0, "Challenge lockout"},
    {"mifare.security.encrypt_user_data",       CONFIG_VAL_BOOL,   &g_system_config.mifare.security.encrypt_user_data,        0, "Encrypt user data"},
    {"mifare.security.encrypt_transactions",    CONFIG_VAL_BOOL,   &g_system_config.mifare.security.encrypt_transactions,     0, "Encrypt transactions"},
    {"mifare.security.encrypt_token_cache",     CONFIG_VAL_BOOL,   &g_system_config.mifare.security.encrypt_token_cache,      0, "Encrypt token cache"},
    {"mifare.security.encrypt_account_data",    CONFIG_VAL_BOOL,   &g_system_config.mifare.security.encrypt_account_data,     0, "Encrypt account data"},
    {"mifare.security.master_key",             CONFIG_VAL_HEX_BYTES, &g_system_config.mifare.security.master_key,             32, "Master key"},
    {"mifare.security.hmac_key",               CONFIG_VAL_HEX_BYTES, &g_system_config.mifare.security.hmac_key,               32, "HMAC key"},
    
    /* UI global settings */
    {"ui.display_refresh_ms",       CONFIG_VAL_UINT32, &g_system_config.ui.display_refresh_ms,       0, "Display refresh"},
    {"ui.screen_switch_delay_ms",   CONFIG_VAL_UINT32, &g_system_config.ui.screen_switch_delay_ms,   0, "Screen switch delay"},
    {"ui.ui_hide_delay_ms",         CONFIG_VAL_UINT32, &g_system_config.ui.ui_hide_delay_ms,         0, "UI hide delay"},
    {"ui.led_flash_interval_ms",    CONFIG_VAL_UINT32, &g_system_config.ui.led_flash_interval_ms,    0, "LED flash interval"},
    {"ui.data_poll_interval_ms",    CONFIG_VAL_UINT32, &g_system_config.ui.data_poll_interval_ms,    0, "Data poll interval"},
    {"ui.lvgl_task_period_ms",      CONFIG_VAL_UINT32, &g_system_config.ui.lvgl_task_period_ms,      0, "LVGL task period"},
    {"ui.lcd_reset_delay_ms",       CONFIG_VAL_UINT32, &g_system_config.ui.lcd_reset_delay_ms,       0, "LCD reset delay"},
    {"ui.screen_brightness_percent", CONFIG_VAL_UINT8, &g_system_config.ui.screen_brightness_percent, 0, "Screen brightness"},
    {"ui.init_customer_id",         CONFIG_VAL_STRING, &g_system_config.ui.init_customer_id,        32, "Init customer ID"},
    {"ui.no_card_customer_id",      CONFIG_VAL_STRING, &g_system_config.ui.no_card_customer_id,     32, "No card customer ID"},
    {"ui.bg_color",                CONFIG_VAL_HEX_COLOR, &g_system_config.ui.bg_color,                0, "Background color"},
    {"ui.bg_grad_color",           CONFIG_VAL_HEX_COLOR, &g_system_config.ui.bg_grad_color,           0, "Gradient color"},
    {"ui.bg_main_stop",             CONFIG_VAL_UINT8,  &g_system_config.ui.bg_main_stop,             0, "Gradient start"},
    {"ui.bg_grad_stop",             CONFIG_VAL_UINT8,  &g_system_config.ui.bg_grad_stop,             0, "Gradient end"},
    {"ui.title_bar_color",         CONFIG_VAL_HEX_COLOR, &g_system_config.ui.title_bar_color,         0, "Title bar color"},
    
    /* Dispenser settings */
    {"dispenser.max_dispense_duration_seconds", CONFIG_VAL_UINT32, &g_system_config.dispenser_logic.wash_duration_seconds, 0, "Max dispense duration (sec)"},
    {"dispenser.card_removal_delay_ms", CONFIG_VAL_UINT32, &g_system_config.dispenser_logic.card_removal_delay_ms, 0, "Card removal delay"},
    
    /* Buzzer settings */
    {"buzzer.enabled",                CONFIG_VAL_BOOL,   &g_system_config.buzzer.enabled,                 0, "Enable buzzer"},
    {"buzzer.default_duration_ms",    CONFIG_VAL_UINT16, &g_system_config.buzzer.default_duration_ms,     0, "Default duration"},
    {"buzzer.double_beep_on_ms",      CONFIG_VAL_UINT16, &g_system_config.buzzer.double_beep_on_ms,       0, "Double beep on"},
    {"buzzer.double_beep_off_ms",     CONFIG_VAL_UINT16, &g_system_config.buzzer.double_beep_off_ms,      0, "Double beep off"},
    {"buzzer.card_init_beep_interval_ms", CONFIG_VAL_UINT16, &g_system_config.buzzer.card_init_beep_interval_ms, 0, "Card init beep"},
    {"buzzer.removal_pattern_on_ms",  CONFIG_VAL_UINT16, &g_system_config.buzzer.removal_pattern_on_ms,   0, "Removal on"},
    {"buzzer.removal_pattern_off_ms", CONFIG_VAL_UINT16, &g_system_config.buzzer.removal_pattern_off_ms,  0, "Removal off"},
    {"buzzer.removal_pattern_count",  CONFIG_VAL_UINT8,  &g_system_config.buzzer.removal_pattern_count,   0, "Removal count"},
    {"buzzer.removal_pattern_repeat_ms", CONFIG_VAL_UINT16, &g_system_config.buzzer.removal_pattern_repeat_ms, 0, "Removal repeat"},
    
    /* SD Logger settings */
    {"sd_logger.init_retry_delay_ms",  CONFIG_VAL_UINT32, &g_system_config.sd_logger.init_retry_delay_ms,   0, "Init retry delay"},
    {"sd_logger.mount_retry_delay_ms", CONFIG_VAL_UINT32, &g_system_config.sd_logger.mount_retry_delay_ms,  0, "Mount retry delay"},
    {"sd_logger.max_retry_count",      CONFIG_VAL_UINT8,  &g_system_config.sd_logger.max_retry_count,       0, "Max retry count"},
    
    /* IO Expander settings */
    {"io_expander.poll_rate_ms",       CONFIG_VAL_UINT32, &g_system_config.io_expander.poll_rate_ms,        0, "Poll rate"},
    {"io_expander.button_debounce_count", CONFIG_VAL_UINT8, &g_system_config.io_expander.button_debounce_count, 0, "Button debounce"},
    
    /* Hardware bus settings */
    {"hardware_bus.spi0_baudrate",    CONFIG_VAL_UINT32, &g_system_config.hardware_bus.spi0_baudrate,     0, "SPI0 baudrate"},
    {"hardware_bus.i2c0_baudrate",    CONFIG_VAL_UINT32, &g_system_config.hardware_bus.i2c0_baudrate,     0, "I2C0 baudrate"},
    {"hardware_bus.i2c1_baudrate",    CONFIG_VAL_UINT32, &g_system_config.hardware_bus.i2c1_baudrate,     0, "I2C1 baudrate"},
    {"hardware_bus.i2c_timeout_us",   CONFIG_VAL_UINT32, &g_system_config.hardware_bus.i2c_timeout_us,    0, "I2C timeout"},
};

#define MYWOTA_CONFIG_SCHEMA_COUNT (sizeof(mywota_config_schema) / sizeof(mywota_config_schema[0]))

/*===========================================================================*/
/*                       Interface Callbacks                                  */
/*===========================================================================*/

/**
 * @brief Get MyWota config schema
 */
const Config_Key_Entry_t* mywota_get_schema(size_t* count)
{
    if (count != NULL) {
        *count = MYWOTA_CONFIG_SCHEMA_COUNT;
    }
    return mywota_config_schema;
}

/**
 * @brief Wrapper for Config_InitDefaults
 */
static void mywota_init_defaults(void)
{
    /* Call the existing defaults function from System_Config.c */
    Config_InitDefaults();
}

/**
 * @brief Calculate CRC for MyWota config
 */
static uint32_t mywota_calc_crc(void)
{
    return Config_CalculateCRC32(&g_system_config);
}

/**
 * @brief Get config filename
 */
static int mywota_get_filename(char* buffer, size_t buffer_size)
{
    return snprintf(buffer, buffer_size, "0:/config_v%d.txt", CONFIG_VERSION);
}

/*===========================================================================*/
/*                       Interface Definition                                 */
/*===========================================================================*/

static const System_Config_Interface_t mywota_config_interface = {
    .init_defaults = mywota_init_defaults,
    .get_schema = mywota_get_schema,
    .calc_crc = mywota_calc_crc,
    .validate = NULL,  /* Use existing validation in System_Config.c */
    .post_load = NULL, /* Use existing post-load in System_Config.c */
    .get_filename = mywota_get_filename,
    .project_name = "MyWota",
    .config_version = CONFIG_VERSION,
    .magic_number = CONFIG_MAGIC_NUMBER
};

/*===========================================================================*/
/*                       Public Functions                                     */
/*===========================================================================*/

/**
 * @brief Initialize MyWota configuration adapter
 */
Config_Interface_Result_t MyWota_Config_Adapter_Init(void)
{
    LOG_DEBUG_ADAPTER("[CONFIG_ADAPTER] Registering MyWota config schema (%zu keys)...\r\n", 
                      MYWOTA_CONFIG_SCHEMA_COUNT);
    
    Config_Interface_Result_t result = SystemConfig_RegisterInterface(&mywota_config_interface);
    
    if (result == CONFIG_INTERFACE_OK) {
        LOG_DEBUG_ADAPTER("[CONFIG_ADAPTER] MyWota config adapter registered\r\n");
    } else {
        USB_Log_Printf("[CONFIG_ADAPTER] Failed to register config adapter\r\n");
    }
    
    return result;
}

/**
 * @brief Get MyWota config schema for external use
 */
const Config_Key_Entry_t* MyWota_Config_GetSchema(size_t* count)
{
    return mywota_get_schema(count);
}
