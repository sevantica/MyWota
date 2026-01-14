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

#ifndef APPLICATION_INCLUDE_SYSTEM_CONFIG_H_
#define APPLICATION_INCLUDE_SYSTEM_CONFIG_H_

#define BUILD_TYPE_CAR_WASH

/*Includes ----------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>

/*Defines ------------------------------------------------------------*/
#define CONFIG_FILE_PATH            "0:/config.txt"
#define CONFIG_FILE_PATH_FORMAT     "0:/config_v%d.txt"
#define CONFIG_MAX_VERSION_SEARCH   10      /* Search up to version 10 */
#define CONFIG_SD_TIMEOUT_MS        3000    /* 3 second timeout for SD card */
#define CONFIG_MAGIC_NUMBER         0x42594C57  /* "BYLW" - BigYellow config marker */
#define CONFIG_VERSION              6       /* Current config version */

/* Flash storage configuration - use last 4KB sector of 2MB flash */
#define CONFIG_FLASH_SIZE           (2 * 1024 * 1024)           /* 2MB flash */
#define CONFIG_FLASH_SECTOR_SIZE    (4 * 1024)                  /* 4KB sector */
#define CONFIG_FLASH_OFFSET         (CONFIG_FLASH_SIZE - CONFIG_FLASH_SECTOR_SIZE)  /* Last sector */

/*Typedefs -----------------------------------------------------------*/

/**
 * @brief MIFARE security configuration parameters
 */
typedef struct {
    /* Encryption settings */
    bool encryption_enabled;                /* Master encryption enable/disable */
    uint8_t master_key[32];                 /* Master secret for key derivation */
    uint8_t hmac_key[32];                   /* Separate HMAC key */
    uint32_t pbkdf2_iterations;             /* Key derivation iterations */
    
    /* Sector keys (custom MIFARE keys) */
    uint8_t sector_keys_a[4][6];            /* Key A for sectors 1-4 */
    uint8_t sector_keys_b[4][6];            /* Key B for sectors 1-4 */
    bool use_custom_sector_keys;            /* Use custom keys vs factory defaults */
    
    /* Security feature enables */
    bool enable_challenge_response;         /* Challenge-response anti-cloning */
    bool enable_replay_protection;          /* Rolling counter protection */
    bool enable_hmac_auth;                  /* HMAC authentication tags */
    uint32_t max_timestamp_drift_sec;       /* Max clock drift allowed (86400 = 24h) */
    uint8_t failed_challenge_lockout;       /* Failed attempts before lockout */
    
    /* Block encryption selection */
    bool encrypt_user_data;                 /* Encrypt blocks 5,6 (user data) */
    bool encrypt_transactions;              /* Encrypt blocks 9,10 (transaction log) */
    bool encrypt_token_cache;               /* Encrypt blocks 13,14 (token cache) */
    bool encrypt_account_data;              /* Encrypt block 16 (phone number) */
} MIFARE_Security_Config_t;

/**
 * @brief MIFARE configuration parameters
 */
typedef struct {
    uint32_t card_timeout_ms;               /* Card response timeout */
    uint8_t max_retries;                    /* Maximum retry attempts */
    uint8_t card_removal_fail_count;        /* Failures to infer card removal */
    uint32_t stability_timeout_ms;          /* Card stable detection time */
    uint32_t removal_stability_ms;          /* Card removal confirmation time */
    uint8_t auth_key[6];                    /* MIFARE authentication key */
    uint32_t card_init_default_tokens;      /* Default token count for new cards */
    bool auto_reinit_on_corruption;         /* Auto-reinitialize corrupt cards */
    char card_init_phone_number[16];        /* Default phone number */
    uint8_t card_init_validity;             /* Default validity level */
    char no_card_user_id[16];               /* User ID when no card present */
    MIFARE_Security_Config_t security;      /* Security configuration */
} MIFARE_Config_t;

/**
 * @brief UI display states for state-based configuration
 */
typedef enum {
    UI_STATE_IDLE = 0,              /* No card present - idle state */
    UI_STATE_CARD_INITIALIZING,     /* Card detected, initializing */
    UI_STATE_CARD_READY,            /* Card authenticated and ready */
    UI_STATE_DISPENSING,            /* Actively dispensing */
    UI_STATE_ERROR,                 /* Error state */
    UI_STATE_COUNT                  /* Number of states */
} UI_State_t;

/**
 * @brief UI element visibility and color configuration for a specific state
 * @note Simplified for car wash UI - configurable per-state appearance
 */
typedef struct {
    /* Visibility flags */
    bool show_customer_id;
    
    /* Image brightness (opacity 0-255, where 255=fully visible, 77=30%) */
    uint8_t image_brightness_active;     /* Brightness for selected/active image */
    uint8_t image_brightness_inactive;   /* Brightness for unselected/inactive images */
    
    /* Ring opacity (0-255, where 255=fully visible) */
    uint8_t ring_opacity_active;         /* Ring opacity when active */
    uint8_t ring_opacity_inactive;       /* Ring opacity when inactive */
    
    /* Color ring colors (0xRRGGBB format) */
    uint32_t ring_color_vacuum_active;      /* Vacuum cleaner ring when active */
    uint32_t ring_color_vacuum_inactive;    /* Vacuum cleaner ring when inactive */
    uint32_t ring_color_brush_active;       /* Wash brush ring when active */
    uint32_t ring_color_brush_inactive;     /* Wash brush ring when inactive */
    uint32_t ring_color_pressure_active;    /* Pressure washer ring when active */
    uint32_t ring_color_pressure_inactive;  /* Pressure washer ring when inactive */
} UI_State_Config_t;

/**
 * @brief UI display configuration parameters
 */
typedef struct {
    /* Global UI timing parameters */
    uint32_t display_refresh_ms;            /* Display refresh rate */
    uint32_t screen_switch_delay_ms;        /* Screen switch delay */
    uint32_t ui_hide_delay_ms;              /* UI hide after dispense */
    uint32_t led_flash_interval_ms;         /* LED flash interval */
    uint32_t data_poll_interval_ms;         /* Data polling interval */
    uint32_t lvgl_task_period_ms;           /* LVGL refresh rate (default 5ms) */
    uint32_t lcd_reset_delay_ms;            /* LCD reset delay (default 500ms) */
    
    /* Display hardware */
    uint8_t screen_brightness_percent;      /* Screen brightness 0-100% (default 100) */
    
    /* Global text configuration */
    char init_customer_id[32];              /* Initial customer ID text */
    char no_card_customer_id[32];           /* Customer ID when no card present */
    
    /* Background colors (0xRRGGBB format) */
    uint32_t bg_color;                      /* Main background color (top) */
    uint32_t bg_grad_color;                 /* Background gradient color (bottom) */
    uint8_t bg_main_stop;                   /* Gradient main stop (0-255) */
    uint8_t bg_grad_stop;                   /* Gradient end stop (0-255) */
    
    /* Title bar color */
    uint32_t title_bar_color;               /* Title bar background color */
    
    /* State-based configuration for each UI state */
    UI_State_Config_t states[UI_STATE_COUNT];
} UI_Config_t;

/**
 * @brief Car wash / loyalty configuration parameters
 * 
 * For BigYellow (car wash):
 *   - loyalty_threshold = washes to earn 1 free wash (e.g., 5)
 *   - loyalty_reward = 1 (1 free wash per threshold)
 * 
 * For MyWota (water dispenser):
 *   - loyalty_threshold = liters purchased to earn reward (e.g., 100)
 *   - loyalty_reward = free liters earned (e.g., 20)
 */
typedef struct {
    uint32_t wash_duration_seconds;         /* Wash timer duration in seconds (default 1200 = 20 min) */
    uint32_t card_removal_delay_ms;         /* Delay after card removal before starting (default 1000) */
    bool loyalty_enabled;                   /* Enable loyalty program (default true for BY, false for MY) */
    uint32_t loyalty_threshold;             /* Threshold to earn reward: washes (BY) or ml (MY, e.g., 100000) */
    uint32_t loyalty_reward;                /* Reward amount: 1 wash (BY) or ml (MY, e.g., 20000) */
} CarWash_Config_t;

/**
 * @brief Buzzer configuration
 */
typedef struct {
    bool enabled;                           /* Master enable/disable */
    uint16_t default_duration_ms;           /* Default beep duration (default 200ms) */
    uint16_t double_beep_on_ms;             /* Double beep on time (default 50ms) */
    uint16_t double_beep_off_ms;            /* Double beep off time (default 50ms) */
    uint16_t card_init_beep_interval_ms;    /* Card init beep interval (default 500ms) */
    uint16_t removal_pattern_on_ms;         /* Removal pattern on time (default 50ms) */
    uint16_t removal_pattern_off_ms;        /* Removal pattern off time (default 50ms) */
    uint8_t removal_pattern_count;          /* Removal pattern repeat count (default 8) */
    uint16_t removal_pattern_repeat_ms;     /* Removal pattern repeat interval (default 3000ms) */
} Buzzer_Config_t;

/**
 * @brief SD Logger configuration
 */
typedef struct {
    uint32_t init_retry_delay_ms;           /* Init retry delay (default 5000ms) */
    uint32_t mount_retry_delay_ms;          /* Mount retry delay (default 2000ms) */
    uint8_t max_retry_count;                /* Max retry attempts (default 1) */
} SDLogger_Config_t;

/**
 * @brief I/O Expander configuration
 */
typedef struct {
    uint32_t poll_rate_ms;                  /* Polling frequency (default 50ms) */
    uint8_t button_debounce_count;          /* Debounce threshold (default 3) */
} IOExpander_Config_t;

/**
 * @brief Hardware bus configuration
 */
typedef struct {
    uint32_t spi0_baudrate;                 /* SPI0 speed (default 62500000) */
    uint32_t i2c0_baudrate;                 /* I2C0 speed (default 400000) */
    uint32_t i2c1_baudrate;                 /* I2C1 speed (default 400000) */
    uint32_t i2c_timeout_us;                /* I2C timeout (default 50000) */
} Hardware_Bus_Config_t;

/**
 * @brief Module enable configuration
 * @details Controls which modules are started at boot. Modules can also be
 *          started/stopped at runtime via USB commands.
 */
typedef struct {
    bool lcd_display_enabled;               /* Enable LCD display task */
    bool mifare_polling_enabled;            /* Enable MIFARE card polling */
    bool carwash_enabled;                   /* Enable car wash controller */
    bool buzzer_enabled;                    /* Enable buzzer polling */
    bool io_expander_enabled;               /* Enable I/O expander control */
    bool rs485_enabled;                     /* Enable RS485 communication */
} Modules_Config_t;

/**
 * @brief System-wide configuration parameters
 */
typedef struct {
    bool test_mode_enabled;                 /* Enable/disable test mode */
    uint8_t log_level;                      /* Log verbosity (0=none, 1=error, 2=critical, 3=debug) */
    char device_id[16];                     /* Device serial number */
    char site_id[16];                       /* Site identifier */
} System_Config_t;

/**
 * @brief Complete system configuration structure
 */
typedef struct {
    uint32_t magic;                         /* Config file marker */
    uint8_t version;                        /* Config file version */
    
    /* System-wide configuration (FIRST) */
    System_Config_t system;                 /* System-wide parameters */
    Modules_Config_t modules;               /* Module enable/disable configuration */
    
    /* Module-specific configurations (grouped together) */
    MIFARE_Config_t mifare;                 /* MIFARE card reader module (contains .security nested) */
    UI_Config_t ui;                         /* UI display module */
    CarWash_Config_t carwash;               /* Car wash controller module */
    Buzzer_Config_t buzzer;                 /* Buzzer module */
    SDLogger_Config_t sd_logger;            /* SD logger module */
    IOExpander_Config_t io_expander;        /* I/O expander module */
    Hardware_Bus_Config_t hardware_bus;     /* Hardware bus (SPI/I2C) */
    
    uint32_t crc32;                         /* Config integrity checksum */
} SystemConfig_t;

/**
 * @brief Configuration load result
 */
typedef enum {
    CONFIG_OK = 0,
    CONFIG_SD_NOT_AVAILABLE,
    CONFIG_FILE_NOT_FOUND,
    CONFIG_FILE_CORRUPT,
    CONFIG_VERSION_MISMATCH,
    CONFIG_WRITE_ERROR,
    CONFIG_READ_ERROR,
    CONFIG_TIMEOUT,
    CONFIG_FLASH_ERROR,
    CONFIG_FLASH_NOT_FOUND
} Config_Result_t;

/*Extern Variables ---------------------------------------------------*/
extern SystemConfig_t g_system_config;

/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Initialize configuration with defaults
 */
void Config_InitDefaults(void);

/**
 * @brief Load configuration from SD card (single attempt)
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromSD(void);

/**
 * @brief Load configuration from already-mounted filesystem
 * @note Call this AFTER SD card is successfully mounted (e.g., from SD Logger Task)
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromMountedFS(void);

/**
 * @brief Save current configuration to SD card
 * @return Config_Result_t Result of save operation
 */
Config_Result_t Config_SaveToSD(void);

/**
 * @brief Check if configuration file exists on SD card
 * @return true if file exists
 */
bool Config_FileExists(void);

/**
 * @brief Create default configuration file on SD card
 * @return Config_Result_t Result of create operation
 */
Config_Result_t Config_CreateDefaultFile(void);

/**
 * @brief Get pointer to current configuration
 * @return Pointer to SystemConfig_t structure
 */
const SystemConfig_t* Config_Get(void);

/**
 * @brief Reset cryptographic keys to factory defaults
 * @return Config_Result_t Result of reset operation
 */
Config_Result_t Config_ResetKeysToFactory(void);

/**
 * @brief Get string representation of config result
 * @param result Config result code
 * @return const char* Result string
 */
const char* Config_GetResultString(Config_Result_t result);

/**
 * @brief Calculate CRC32 for configuration data
 * @param config Pointer to config structure
 * @return uint32_t CRC32 value
 */
uint32_t Config_CalculateCRC32(const SystemConfig_t* config);

/**
 * @brief Save current configuration to flash memory
 * @return Config_Result_t Result of save operation
 */
Config_Result_t Config_SaveToFlash(void);

/**
 * @brief Load configuration from flash memory
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromFlash(void);

/**
 * @brief Check if valid configuration exists in flash
 * @return true if valid config found in flash
 */
bool Config_FlashHasValidConfig(void);

/**
 * @brief Find highest config version file on SD card
 * @param found_version Pointer to store found version number
 * @return true if version file found
 */
bool Config_FindHighestVersion(uint8_t *found_version);

/**
 * @brief Save configuration to versioned file
 * @return Config_Result_t Result of save operation
 */
Config_Result_t Config_SaveToVersionedFile(void);

/**
 * @brief Print current configuration to USB log
 */
void Config_PrintToUSB(void);

#endif /* APPLICATION_INCLUDE_SYSTEM_CONFIG_H_ */
