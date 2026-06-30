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

/* #define BUILD_TYPE_CAR_WASH */
#ifndef BUILD_TYPE_WATER_DISPENSER
#define BUILD_TYPE_WATER_DISPENSER
#endif

/* Driver Feature Toggles (Read by sevantica_drivers CMake) */
#define USE_DRIVERS_NFC           1
#define USE_DRIVERS_RS485         1
#define USE_DRIVERS_RS485_SLAVE   1
#define USE_DRIVERS_FATFS         1
#define USE_DRIVERS_USB           1
#define USE_DRIVERS_CRYPTO        1
#define USE_DRIVERS_IO_EXPANDER   1
#define USE_DRIVERS_FLASH         1  /* Enable centralized Flash Task */
#define USE_DRIVERS_CONNECTIVITY  0
#define USE_DRIVERS_DISPLAY       1
#define USE_DRIVERS_FEEDBACK      1

/*Includes ----------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "MIFARE_Config_Structs.h"

/*Defines ------------------------------------------------------------*/
#define CONFIG_FILE_PATH            "0:/config.txt"
#define CONFIG_FILE_PATH_FORMAT     "0:/config_v%d.txt"
#define CONFIG_MAX_VERSION_SEARCH   10      /* Search up to version 10 */
#define CONFIG_SD_TIMEOUT_MS        3000    /* 3 second timeout for SD card */
#define CONFIG_MAGIC_NUMBER         0x42594C57  /* "BYLW" - CCH config marker */
#define CONFIG_VERSION              15      /* Bump to 15: dispenser_logic.flow_pulses_per_liter */

/* Flash storage configuration - use last 4KB sector of 2MB flash */
#define CONFIG_FLASH_SIZE           (2 * 1024 * 1024)           /* 2MB flash */
#define CONFIG_FLASH_SECTOR_SIZE    (4 * 1024)                  /* 4KB sector */
#define CONFIG_FLASH_OFFSET         (CONFIG_FLASH_SIZE - CONFIG_FLASH_SECTOR_SIZE)  /* Last sector (User Config) */
#define CONFIG_FACTORY_FLASH_OFFSET (CONFIG_FLASH_OFFSET - CONFIG_FLASH_SECTOR_SIZE) /* Sector before last (Factory Config) */
#define CONFIG_WDT_LOG_FLASH_OFFSET (CONFIG_FACTORY_FLASH_OFFSET - CONFIG_FLASH_SECTOR_SIZE) /* Sector 509 (WDT Logs) */

/*Typedefs -----------------------------------------------------------*/

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
 * For CCH (Central Control Hub):
 *   - loyalty_threshold = liters purchased to earn reward (e.g., 100) or washes
 *   - loyalty_reward = free liters earned (e.g., 20) or free wash
 */
typedef struct {
    uint32_t wash_duration_seconds;         /* Wash timer duration in seconds (default 1200 = 20 min) */
    uint32_t card_removal_delay_ms;         /* Delay after card removal before starting (default 1000) */
    bool loyalty_enabled;                   /* Enable loyalty program (default true for CCH) */
    uint32_t loyalty_threshold;             /* Threshold to earn reward: washes or ml */
    uint32_t loyalty_reward;                /* Reward amount: 1 wash or ml */
    /* Self-clean (CCH-orchestrated periodic flush) */
    uint32_t self_clean_volume_ml;          /* Default target volume per clean cycle (mL) */
    uint32_t self_clean_max_duration_sec;   /* Hard time cap per clean cycle (seconds) */
    bool     self_clean_safety_boot_enabled;/* If true, slave runs its own clean on boot when stale */
    uint32_t self_clean_safety_max_hours;   /* Threshold (hours) for boot-time safety clean */
    uint32_t last_clean_unix_time;          /* RTC time of last successful clean (0 = never) */
    /* Filter life tracking (slave-side, persisted in flash) */
    uint32_t filter_capacity_ml;            /* Filter rated capacity in mL (0 = disabled) */
    uint32_t filter_used_ml;                /* Cumulative mL through filter since reset */
    /* Hard cap on water per wash session (mL). BigYellow only; MyWota leaves
     * this at 0 since dispenser balance already gates volume. */
    uint32_t max_wash_volume_ml;
    /* Flow sensor calibration: YS-S201 pulses per liter. Increase to reduce a
     * measured-volume over-read, decrease to correct an under-read. 0 = use the
     * driver default (YS_S201_PULSES_PER_LITER). */
    uint16_t flow_pulses_per_liter;
} DispenserLogic_Config_t;

/**
 * @brief CCH-side periodic clean scheduler config (unused on slaves but
 *        kept here for binary-compat with the shared System_Config.c).
 */
typedef struct {
    bool     enabled;
    uint32_t interval_hours;
    uint32_t target_volume_ml;
    uint32_t max_duration_sec;
    uint32_t failure_backoff_min;
} CleanScheduler_Config_t;

/**
 * @brief Buzzer configuration
 */
#include "Buzzer_Config_Structs.h"

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
    bool dispenser_enabled;                 /* Enable dispenser controller */
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
    /* Network configuration (for Pico W) */
    char wifi_ssid[33];                     /* Last connected SSID (32 chars + null) */
    char wifi_password[65];                 /* Last connected Password (64 chars + null) */
    bool wifi_auto_connect;                 /* Auto connect on boot */
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
    DispenserLogic_Config_t dispenser_logic;       /* Dispenser logic module (formerly carwash) */
    CleanScheduler_Config_t clean_scheduler;       /* CCH-orchestrated periodic clean (unused on slave) */
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
 * @brief Update last self-clean timestamp and persist to flash (single-field write).
 * @param unix_ts RTC unix time of the just-completed clean cycle.
 */
Config_Result_t Config_UpdateLastCleanTime(uint32_t unix_ts);

/**
 * @brief Update filter usage counter and persist to flash.
 * @param used_ml New cumulative used-mL value (since last reset).
 */
Config_Result_t Config_UpdateFilterUsedMl(uint32_t used_ml);

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

/**
 * @brief Print current configuration to a generic CLI channel
 */
void Config_PrintToChannel(const void* channel);

/**
 * @brief Reset system configuration to factor defaults and save
 * @return Config_Result_t Result of operation
 */
Config_Result_t Config_FactoryReset(void);

/**
 * @brief Save current configuration as factory default in flash
 * @note This should only be used during production or by authorized personnel
 * @return Config_Result_t Result of save operation
 */
Config_Result_t Config_SaveFactoryToFlash(void);

/**
 * @brief Load factory default configuration from flash
 * @return Config_Result_t Result of load operation
 */
Config_Result_t Config_LoadFromFactoryFlash(void);

#endif /* APPLICATION_INCLUDE_SYSTEM_CONFIG_H_ */
