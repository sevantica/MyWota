# Copilot Instructions for MyWota Firmware

## Driver and Module Initialization Pattern

All drivers and modules in this codebase follow a consistent initialization pattern where each driver is responsible for initializing its own hardware dependencies.

### Pattern Overview

1. **Drivers initialize their own hardware** - Each driver calls its hardware init function internally
2. **Hardware init functions are idempotent** - They check if already initialized and return early
3. **No hardware init in System.c** - System.c only calls driver-level Init functions, not low-level hardware init

### Example: LCD Driver (SPI-based)

```c
// In LCD_Driver.c
void lcd_gpio_init(void)
{
    lcd_pins = Get_LCD_Pins();
    init_lcd_hw();  // Initializes SPI0 + GPIO pins internally
}

// In Hardware_Access.c
void init_lcd_hw(void)
{
    Init_SPI_0(SPI_0_BAUDRATE);  // Idempotent - checks if already initialized
    Init_GPIO_Output(LCD_CS_PIN);
    // ... other LCD-specific GPIO setup
}
```

### Example: CAT9555 I/O Expander Driver (I2C-based)

```c
// In CAT9555_Driver.c
CAT9555_Status_t CAT9555_Init(CAT9555_Handle_t *handle, uint8_t i2c_address)
{
    init_io_expander_hw();  // Initializes I2C0 + interrupt pin internally
    io_expander_pins = Get_IO_Expander_Pins();
    // ... driver initialization
}

// In Hardware_Access.c
void init_io_expander_hw(void)
{
    Init_I2C_0(I2C_0_BAUDRATE);  // Idempotent - checks if already initialized
    Init_GPIO_Input_PullUp(IO_EXPANDER_INT_PIN);
}
```

### Example: PN532 NFC Driver (I2C-based)

```c
// In PN532_Driver.c
PN532_Status_t PN532_Init(void)
{
    init_pcd_hw();  // Initializes I2C0 + reset pin internally
    // ... driver initialization
}

// In Hardware_Access.c
void init_pcd_hw(void)
{
    Init_I2C_0(I2C_0_BAUDRATE);  // Idempotent - shared with CAT9555
    Init_GPIO_Output(PCD_RST_PIN);
}
```

### Hardware Init Functions in Hardware_Access.c

| Function | Purpose | Bus/Peripheral |
|----------|---------|----------------|
| `init_lcd_hw()` | LCD display hardware | SPI0 |
| `init_pcd_hw()` | NFC/RFID hardware | I2C0 |
| `init_io_expander_hw()` | I/O expander hardware | I2C0 |
| `init_sd_hw()` | SD card hardware | SPI0 |

### Example: RS485 Communication Task (UART-based)

```c
// In RS485_Task.c
static void RS485_Task(void* argument)
{
    rs485_hardware_init();  // Initializes UART0 + DE pin internally
    RS485_FileTransfer_Init();
    
    for(;;) {
        TASK_HEARTBEAT_EVERY_SECOND("RS485_Task");
        System_ReportTaskStatus(SYSTEM_TASK_ID_RS485, true);
        // ... task work
    }
}

// Hardware initialization is internal to the task
static void rs485_hardware_init(void)
{
    app_pins = Get_App_GPIO_Pins();
    uart_init(RS485_UART_INSTANCE, RS485_BAUDRATE);
    // DE pin already initialized by Hardware_Access
}
```

### Low-Level Init Functions (Idempotent)

| Function | Purpose |
|----------|---------|
| `Init_SPI_0(baudrate)` | Initialize SPI0 peripheral |
| `Init_I2C_0(baudrate)` | Initialize I2C0 peripheral |
| `Init_I2C_1(baudrate)` | Initialize I2C1 peripheral |
| `Init_GPIO_Output(pin)` | Configure GPIO as output |
| `Init_GPIO_Input(pin)` | Configure GPIO as input |
| `Init_GPIO_Input_PullUp(pin)` | Configure GPIO as input with pull-up |

### Key Principles

1. **Driver owns its hardware init** - Don't initialize hardware in System.c, let the driver do it
2. **Use existing init functions** - Check Hardware_Access.c for `init_*_hw()` functions
3. **Idempotency is essential** - Multiple drivers may share a bus (e.g., I2C0 shared by PN532 and CAT9555)
4. **Get pin config first** - Use `Get_*_Pins()` functions to get pin assignments

### Adding a New Driver

1. Create `init_<module>_hw()` function in Hardware_Access.c
2. Declare it in Hardware_Access.h
3. Call `init_<module>_hw()` at the start of your driver's Init function
4. Create `Get_<Module>_Pins()` if needed for pin configuration

## Watchdog Timer (WDT) Pattern

All FreeRTOS tasks MUST feed the watchdog timer to prevent system resets.

### Required in Every Task

**CRITICAL**: Tasks need TWO things for WDT monitoring:
1. `TASK_HEARTBEAT_EVERY_SECOND()` - Prints stack usage logs
2. `System_ReportTaskStatus()` - Updates WDT tracking system

```c
#include "Task_Heartbeat.h"
#include "System.h"

void My_Task(void* argument)
{
    while (1) {
        // Feed watchdog every second - MUST be first in loop
        TASK_HEARTBEAT_EVERY_SECOND("TaskName");
        System_ReportTaskStatus(SYSTEM_TASK_ID_MY_TASK, true);
        
        // ... task work ...
        
        vTaskDelay(pdMS_TO_TICKS(100));  // Or appropriate delay
    }
}
```

### Key Points

- Place `TASK_HEARTBEAT_EVERY_SECOND()` at the **top of the task loop**
- Immediately follow with `System_ReportTaskStatus(SYSTEM_TASK_ID_*, true)`
- `TASK_HEARTBEAT_EVERY_SECOND()` only prints - it does NOT update WDT tracking
- `System_ReportTaskStatus()` is what actually prevents WDT resets
- Use the actual task name string (matches xTaskCreate name)
- Task ID must be added to `System_Task_ID_t` enum in System.h
- Task ID must match WDT tracking array in System.c
- Failure to call both functions causes system reset after grace period

### Critical Race Condition Fix (Dec 2024)

**Issue**: USB_Command task showed `TIMEOUT (last: 4294967295 ms ago)` despite successfully reporting every 10ms.

**Root Cause**: Race condition in System.c watchdog monitoring. The snapshot code read `now = xTaskGetTickCount()` and then copied task status data without a critical section. Between these operations:
- The tick counter could increment (hardware interrupt)
- Another task could call `System_ReportTaskStatus()` and update `last_report_tick`
- Result: `last_report_tick > now` → unsigned underflow → `diff = 0xFFFFFFFF`

**Example**: Debug logs showed `last_tick=14658, now=14657, diff=4294967295`

**Solution**: Use `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` to make snapshot atomic:
```c
// In System.c watchdog check
taskENTER_CRITICAL();  // Disable interrupts
now = xTaskGetTickCount();  // Read time with no preemption
// Copy all task status data
for (uint8_t i = 0; i < TASK_ID_COUNT; i++) {
    status_snapshot[i].last_report_tick = s_task_wdt_status[i].last_report_tick;
    // ...
}
taskEXIT_CRITICAL();  // Re-enable interrupts
```

**Key Lesson**: When reading timestamps for comparison, always use critical sections to ensure atomicity. Even reading a single 32-bit tick counter can be interrupted on Cortex-M0+.

## Polling vs Push Architecture

### UI Updates
- **UI polls for data** - UI driver calls getter functions to retrieve state
- **Never push to UI** - Modules/drivers should NOT call UI functions directly
- Example: `Dispenser_GetDispensedAmountML()`, `MIFARE_GetCardBalance()`

### Buzzer
- **Buzzer polls module states** - Buzzer polling task checks state via getter functions
- **Never push to buzzer** - Don't call `Buzzer_Beep()` directly from other modules
- Buzzer driver polls:
  - `Dispenser_IsValveOpen()` - Monitors valve state for dispense start/stop
  - `MIFARE_Dispenser_IsDispenseActive()` - Monitors dispense state for dispenser start/finish
- Buzzer task handles beep patterns internally based on state transitions

### Benefits
- Decoupled modules - easier testing and maintenance
- No circular dependencies
- Clear data flow direction
- Consistent architecture across all modules

### Implementation Pattern
```c
// ❌ WRONG - Direct call (push pattern)
void Dispenser_StartDispense(void) {
    Buzzer_Beep(buzzer, 200);  // DON'T DO THIS
}

// ✅ CORRECT - Polling pattern
bool MIFARE_Dispenser_IsDispenseActive(void) {
    return g_dispense_timer.dispense_active;  // Getter for polling
}

// Buzzer task polls this getter every 50ms
static void Buzzer_PollingTaskFunc(void *pvParameters) {
    bool last_dispense_state = false;
    while(1) {
        bool current = MIFARE_Dispenser_IsDispenseActive();
        if (current != last_dispense_state) {
            if (current) Buzzer_SignalDispenserStart(handle);
            else Buzzer_SignalDispenserStop(handle);
            last_dispense_state = current;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

## FreeRTOS Task Patterns

### Task Naming Convention
All FreeRTOS tasks follow a consistent naming pattern:
- Task starter function: `Task_Start_<Module>_Task()`
- Internal task function: `<Module>_Task()` (static)

### Example
```c
// In <Module>.c
static void Module_Task(void* argument);

void Task_Start_Module_Task(void)
{
    xTaskCreate(Module_Task, "Module_Task", MODULE_TASK_STACK_WORDS, 
                NULL, MODULE_TASK_PRIORITY, &module_task_handle);
}
```

### Task Configuration (task_stack_config.h)
All task stack sizes and priorities are centralized in `task_stack_config.h`:

```c
/* Stack sizes in bytes, then converted to words */
#define MODULE_TASK_STACK_BYTES  (128u * N)  // Comment with sizing rationale
#define MODULE_TASK_STACK_WORDS  STACK_BYTES_TO_WORDS(MODULE_TASK_STACK_BYTES)

/* Priorities - USB highest to prevent priority inversion */
#define USB_CDC_TASK_PRIORITY           (tskIDLE_PRIORITY + 3)  // Highest
#define SYSTEM_TASK_PRIORITY            (tskIDLE_PRIORITY + 2)
#define LCD_DISPLAY_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)
```

### Task Startup Order (in System.c)
Tasks are started in a specific order in `system_init()`:
1. SD_Logger_Task - Must mount SD before config load
2. USB_CDC_Task - Enable logging
3. LCD_Display_Task - UI
4. Dispenser_Control_Task - Hardware control
5. Other polling tasks (Buzzer, etc.)

## Debug Logging Pattern

### Per-Module Logging Macros
Each module defines its own compile-time logging controls:

```c
/* Logging Configuration */
#define LOG_DEBUG_<MODULE>_EN      1
#define LOG_CRITICAL_<MODULE>_EN   1
#define LOG_ERROR_<MODULE>_EN      1

#if LOG_DEBUG_<MODULE>_EN
    #define LOG_DEBUG_<MODULE>(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_<MODULE>(...)
#endif

#if LOG_CRITICAL_<MODULE>_EN
    #define LOG_CRITICAL_<MODULE>(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_<MODULE>(...)
#endif

#if LOG_ERROR_<MODULE>_EN
    #define LOG_ERROR_<MODULE>(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_<MODULE>(...)
#endif
```

### Convenience Macro
Many modules also define a shorter convenience macro:
```c
#define MODULE_DEBUG_PRINT(fmt, ...) LOG_DEBUG_<MODULE>("<MODULE>: " fmt "\r\n", ##__VA_ARGS__)
```

### Log Message Format
- Include module prefix: `"MODULE: message\r\n"`
- Use `\r\n` for line endings (USB terminal compatibility)
- Critical messages use checkmarks/crosses: `"[✓]"`, `"[✗]"`, `"[→]"`

### Avoiding Duplicate Log Messages
**IMPORTANT:** Each significant event should be logged exactly once. If a log message appears multiple times during startup or operation, identify and remove duplicates:

1. **Search for the duplicate message** - Use grep to find all occurrences of the log text
2. **Determine the canonical location** - The log should appear in the module that owns the action
3. **Remove redundant logs** - Delete logs from callers or wrapper functions that duplicate the primary log

Example: If `Config_LoadFromMountedFS()` logs "Loaded from SD card", then `SD_Logger_Task()` should NOT also log the same message when calling that function.

## Driver Status Pattern

### Status Enum Convention
Every driver defines a status enum with consistent naming:

```c
typedef enum {
    <MODULE>_OK = 0,                    // Success (always 0)
    <MODULE>_ERROR,                     // General error
    <MODULE>_ERROR_INVALID_PARAM,       // Invalid parameter
    <MODULE>_ERROR_NOT_INITIALIZED,     // Not initialized
    <MODULE>_ERROR_TIMEOUT,             // Timeout
    <MODULE>_ERROR_<SPECIFIC>,          // Module-specific errors
} <Module>_Status_t;
```

### Status String Function
Every driver provides a `GetStatusString` function for logging:

```c
const char* <Module>_GetStatusString(<Module>_Status_t status)
{
    switch (status) {
        case <MODULE>_OK:                    return "OK";
        case <MODULE>_ERROR:                 return "Error";
        case <MODULE>_ERROR_INVALID_PARAM:   return "Invalid Param";
        case <MODULE>_ERROR_NOT_INITIALIZED: return "Not Initialized";
        default:                             return "Unknown";
    }
}
```

### Usage in Logging
```c
if (status != MODULE_STATUS_OK) {
    LOG_ERROR_MODULE("Operation failed: %s\r\n", Module_GetStatusString(status));
}
```

## Driver Handle Pattern

### Driver-Owned Handles with Getter
All drivers own their handle memory internally and expose a getter function to access instances:

```c
// In driver .h file
#define CAT9555_MAX_INSTANCES  (2)  // Maximum supported instances

CAT9555_Handle_t* CAT9555_GetHandle(uint8_t index);  // Get handle by index

// In driver .c file
static CAT9555_Handle_t s_cat9555_handles[CAT9555_MAX_INSTANCES];

CAT9555_Handle_t* CAT9555_GetHandle(uint8_t index)
{
    if (index >= CAT9555_MAX_INSTANCES) {
        return NULL;
    }
    return &s_cat9555_handles[index];
}
```

### Usage Example
```c
// In application code (e.g., System.c or a task file)
void System_Init(void)
{
    // Get handle from driver - driver owns the memory
    CAT9555_Handle_t *io_expander = CAT9555_GetHandle(0);
    
    // Initialize and use
    CAT9555_Init(io_expander, 0x27);
    CAT9555_WritePin(io_expander, IO_PIN_LED, CAT9555_PIN_HIGH);
}
```

### Multi-Instance Example
```c
void Init_Multiple_Expanders(void)
{
    // First I/O expander at address 0x20
    CAT9555_Handle_t *expander_0 = CAT9555_GetHandle(0);
    CAT9555_Init(expander_0, 0x20);
    
    // Second I/O expander at address 0x21
    CAT9555_Handle_t *expander_1 = CAT9555_GetHandle(1);
    CAT9555_Init(expander_1, 0x21);
}
```

### Benefits
- Driver owns all handle memory - no external allocation needed
- Simple API: `XXX_GetHandle(index)` returns pointer to internal handle
- Compile-time instance limit via `XXX_MAX_INSTANCES` define
- Handle parameter enables generic functions for any instance
- Easy to extend for more instances by changing the define

## Pin Configuration Pattern

### Hardware Abstraction Layer
Pin assignments are defined in Hardware_Access.h with functional names:

```c
/* I/O Expander Functional Pin Definitions */
#define IO_PIN_RELAY_CONTROL_0      CAT9555_PIN_0   // IO0_0 - Main relay
#define IO_PIN_USER_BUTTON          CAT9555_PIN_12  // IO1_4 - User button
#define IO_PIN_BUZZER               CAT9555_PIN_11  // IO1_3 - Piezo buzzer
```

### Pin Configuration Structures
Modules retrieve pin configs via getter functions:

```c
// In Hardware_Access.h
LCD_Pins_t Get_LCD_Pins(void);
NFC_Pins_t Get_NFC_Pins(void);
SD_Card_Pins_t Get_SD_Card_Pins(void);
IO_Expander_Pins_t Get_IO_Expander_Pins(void);
App_GPIO_Pins_t Get_App_GPIO_Pins(void);
```

### Usage in Driver
```c
void Driver_Init(void)
{
    module_pins = Get_Module_Pins();  // Get pin config
    init_module_hw();                  // Initialize hardware
    // ... use module_pins.some_pin
}
```

## FreeRTOS Timing Pattern

### Delay Functions
Always use `pdMS_TO_TICKS()` for portable timing:

```c
// Correct
vTaskDelay(pdMS_TO_TICKS(100));  // 100ms delay

// Wrong - ticks are platform-dependent
vTaskDelay(100);
```

### Semaphore/Mutex Timeouts
```c
if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return ERROR_TIMEOUT;
}
```

### Timer Periods
```c
xTimerCreate("Timer", pdMS_TO_TICKS(1000), pdTRUE, NULL, callback);
```

## System Configuration Organization Pattern

### Configuration Structure Philosophy
All configuration follows a strict organizational pattern for maintainability and clarity:

1. **One config struct per module** - Each module (MIFARE, UI, Dispenser, etc.) has exactly ONE configuration structure
2. **No separate timing configs** - Timing parameters are part of the main module config, not separate structs
3. **Grouped organization** - All module configs are grouped together in `SystemConfig_t`
4. **Consistent ordering** - Same order everywhere: defaults, parsing, writing, printing, example file

### SystemConfig_t Structure Order

```c
typedef struct {
    uint32_t magic;                         /* Config file marker */
    uint8_t version;                        /* Config file version */
    
    /* System-wide configuration (first) */
    System_Config_t system;                 /* System-wide parameters */
    Modules_Config_t modules;               /* Module enable/disable */
    
    /* Module-specific configurations (grouped together) */
    MIFARE_Config_t mifare;                 /* MIFARE card reader module */
    UI_Config_t ui;                         /* UI display module */
    Dispenser_Config_t dispenser;           /* Dispenser controller module */
    IOExpander_Config_t io_expander;        /* I/O expander module */
    RS485_Config_t rs485;                   /* RS485 communication module */
    Buzzer_Config_t buzzer;                 /* Buzzer module */
    SDLogger_Config_t sd_logger;            /* SD logger module */
    RTC_Config_t rtc;                       /* RTC module */
    FlowSensor_Config_t flow_sensor;        /* Flow sensor module */
    Hardware_Bus_Config_t hardware_bus;     /* Hardware bus (SPI/I2C) */
    
    uint32_t crc32;                         /* Config integrity checksum */
} SystemConfig_t;
```

### Module Config Example: MIFARE

**Correct** - All parameters in one structure:
```c
typedef struct {
    uint32_t card_timeout_ms;
    uint8_t max_retries;
    // ... other MIFARE parameters
    uint32_t post_reset_cooldown_ms;        /* Timing parameter */
    bool auto_recovery_enabled;             /* Timing parameter */
    MIFARE_Security_Config_t security;      /* Nested config OK */
} MIFARE_Config_t;
```

**Wrong** - Separate timing struct:
```c
// ❌ DON'T DO THIS
typedef struct {
    uint32_t card_timeout_ms;
    uint8_t max_retries;
} MIFARE_Config_t;

typedef struct {
    uint32_t post_reset_cooldown_ms;
    bool auto_recovery_enabled;
} MIFARE_Timing_Config_t;  // ❌ Separate timing config
```

### Config File Organization

All functions must follow the same grouping order:

1. **Config_InitDefaults()** - Group system-wide first, then all modules
2. **Config_ParseLine()** - Parse in same order as defaults
3. **Config_WriteToFile()** - Write sections in same order
4. **Config_PrintToUSB()** - Print in same order
5. **example_config.txt** - Comment sections matching code order

Example from Config_InitDefaults():
```c
void Config_InitDefaults(void)
{
    g_system_config.magic = CONFIG_MAGIC_NUMBER;
    g_system_config.version = CONFIG_VERSION;
    
    /* ========================================================================== */
    /* SYSTEM-WIDE CONFIGURATION                                                  */
    /* ========================================================================== */
    
    /* System defaults */
    g_system_config.system.test_mode_enabled = false;
    // ...
    
    /* Module enable defaults */
    g_system_config.modules.lcd_display_enabled = true;
    // ...
    
    /* ========================================================================== */
    /* MODULE-SPECIFIC CONFIGURATION                                               */
    /* ========================================================================== */
    
    /* MIFARE defaults */
    g_system_config.mifare.card_timeout_ms = 2000;
    g_system_config.mifare.post_reset_cooldown_ms = 1200;  // Timing with main config
    // ...
    
    /* UI defaults */
    g_system_config.ui.display_refresh_ms = 20;
    g_system_config.ui.lvgl_task_period_ms = 5;  // Timing with main config
    // ...
    
    /* Dispenser defaults */
    // ...
    
    /* I/O Expander defaults */
    // ...
    
    /* RS485 defaults */
    // ...
    
    /* Buzzer defaults */
    // ...
    
    /* SD Logger defaults */
    // ...
    
    /* RTC defaults */
    // ...
    
    /* Flow Sensor defaults */
    // ...
    
    /* Hardware Bus defaults */
    // ...
}
```

### Benefits of This Pattern

1. **Consistency** - Same order everywhere makes code predictable
2. **Maintainability** - Adding parameters is straightforward
3. **Clarity** - One config per module, no scattered timing structs
4. **Review-friendly** - Easy to see all parameters for a module together
5. **Config file matches code** - User-facing config.txt follows same structure

## SD Card Configuration Pattern

### Configuration File Structure
All system configuration is stored on the SD card in `/config.txt` with auto-fallback to defaults if missing or corrupted.

### File Location and Naming
- **Primary config:** `/config.txt` (SD card root directory)
- **Versioned configs:** `/config_v<version>.txt` (e.g., `/config_v1.txt`)
- **Example files in repo:**
  - `application/example_config.txt` - Full configuration template
  - `application/example_security_config.txt` - Security parameters template

### Configuration Sections
The config file uses INI-style `key=value` format with the following sections:

#### 1. MIFARE Card Configuration
```ini
mifare.card_timeout_ms=2000
mifare.max_retries=3
mifare.card_removal_fail_count=3
mifare.stability_timeout_ms=50
mifare.removal_stability_ms=1000
mifare.card_init_default_tokens=10
mifare.auto_reinit_on_corruption=1
```

#### 2. MIFARE Security Configuration (Encryption)
```ini
# Enable/disable encryption (0=disabled, 1=enabled)
mifare.security.encryption_enabled=1

# PBKDF2 key derivation iterations (1000-10000)
# Lower = faster but less secure, 1000 recommended for embedded
mifare.security.pbkdf2_iterations=1000

# Custom MIFARE sector keys (prevents generic readers)
mifare.security.use_custom_sector_keys=0

# HMAC authentication (detects tampering)
mifare.security.enable_hmac_auth=1

# Replay attack protection (timestamps + counters)
mifare.security.enable_replay_protection=1

# Challenge-response protocol (anti-cloning, optional)
mifare.security.enable_challenge_response=0
mifare.security.max_timestamp_drift_sec=86400
mifare.security.failed_challenge_lockout=5

# Per-block encryption enables (which data to encrypt)
mifare.security.encrypt_user_data=1
mifare.security.encrypt_transactions=1
mifare.security.encrypt_token_cache=1
mifare.security.encrypt_account_data=1

# Cryptographic keys (32 bytes hex, NO 0x prefix)
mifare.security.master_key=A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBFC0C1C2C3C4
mifare.security.hmac_key=5A5B5C5D5E5F606162636465666768696A6B6C6D6E6F707172737475767778

# Custom sector keys (6 bytes hex each, if use_custom_sector_keys=1)
mifare.security.sector_key_1_a=FFFFFFFFFFFF
mifare.security.sector_key_1_b=FFFFFFFFFFFF
mifare.security.sector_key_2_a=FFFFFFFFFFFF
mifare.security.sector_key_2_b=FFFFFFFFFFFF
mifare.security.sector_key_3_a=FFFFFFFFFFFF
mifare.security.sector_key_3_b=FFFFFFFFFFFF
mifare.security.sector_key_4_a=FFFFFFFFFFFF
mifare.security.sector_key_4_b=FFFFFFFFFFFF
```

#### 3. UI Configuration (Per-State)
```ini
# Global UI settings
ui.display_refresh_ms=100
ui.screen_switch_delay_ms=3000
ui.ui_hide_delay_ms=10000
ui.led_flash_interval_ms=500
ui.data_poll_interval_ms=50
ui.init_customer_id=INIT
ui.no_card_customer_id=NO_CARD

# Per-state UI configuration (states: idle, initializing, ready, dispensing, error)
ui.idle.show_customer_id=1

ui.ready.show_customer_id=1

ui.dispensing.show_customer_id=1

ui.error.show_customer_id=1
```

#### 4. System Configuration
```ini
system.device_id=DEV001
system.site_id=SITE001
system.test_mode_enabled=0
system.log_level=2
```

### Creating Config Files

#### Method 1: Copy Example File
```bash
# Copy example to SD card
cp application/example_config.txt /path/to/sd_card/config.txt
```

#### Method 2: Use USB Commands (Runtime)
```
# View current config
config

# Modify individual parameters
set mifare.card_init_default_tokens 20
set mifare.security.pbkdf2_iterations 2000
set system.test_mode_enabled 1

# Changes auto-save to SD and flash
```

#### Method 3: Manual Creation
Create `/config.txt` on SD card with desired parameters. Missing parameters auto-populate with defaults from `Config_InitDefaults()`.

### Security Key Generation
**CRITICAL:** Never use default keys in production!

```bash
# Generate random 32-byte master key
openssl rand -hex 32

# Generate random 32-byte HMAC key
openssl rand -hex 32

# Generate random 6-byte sector keys
openssl rand -hex 6
```

### Config Loading Priority
1. **SD card config file** (`/config.txt`) - Primary source
2. **Flash memory** - Cached from last successful load
3. **Defaults** - Hardcoded in `Config_InitDefaults()`

### Config Update Workflow
```c
// System automatically:
// 1. Loads config from SD on boot (System_Init)
// 2. Falls back to flash if SD fails
// 3. Falls back to defaults if both fail
// 4. Auto-saves to both SD and flash on USB 'set' commands
```

### Testing Config Changes
```
1. Edit /config.txt on SD card
2. Remove/reinsert SD card OR reset device
3. Run 'config' USB command to verify loaded values
4. Check serial log for "[CONFIG] Configuration loaded from SD"
5. Verify no "[CONFIG] Parse error" messages
```

### Common Issues

#### Config Not Loading
- Check SD card mounted: Look for `[SD_LOGGER] SD card initialized successfully`
- Check file exists: `/config.txt` in SD root
- Check syntax: Each line must be `key=value` with no spaces around `=`
- Check logs: Search for `[CONFIG]` messages in serial output

#### Security Parameters Missing
- Security section optional - defaults to encryption enabled with random keys
- To disable encryption: `mifare.security.encryption_enabled=0`
- Keys auto-generated if missing (insecure for production!)

#### Performance Issues with PBKDF2
- High iterations (>5000) cause slow card detection (~500ms+)
- Recommended: 1000-2000 iterations for embedded systems
- Trade-off: Lower iterations = faster but less brute-force protection

### USB Command Reference
```
config                           - Display all configuration
set <param> <value>              - Modify single parameter
status                           - Show system status
help                             - List all commands
```

### Adding/Removing Config Parameters

**IMPORTANT:** When adding or removing configuration parameters, you MUST increment `CONFIG_VERSION` in System_Config.h. This ensures old config files are properly migrated.

When adding or removing configuration parameters, you MUST update these locations:

1. **System_Config.h** - Add/remove struct fields and **increment `CONFIG_VERSION`**
2. **System_Config.c** - Update in multiple places:
   - `Config_InitDefaults()` - Set default value
   - `Config_ParseLine()` - Parse from config file
   - `Config_WriteToFile()` - Write to config file
   - `Config_PrintToUSB()` - Display via USB command
   - `EXPECTED_PARAMS_Vx` - Update parameter count constant and rename to new version
3. **USB_Command_Handler.c** - Add `set` command support in `handle_set_command()`

#### Example: Adding a new parameter `ui.new_param`

```c
// 1. System_Config.h - add to struct
typedef struct {
    // ...existing fields...
    uint32_t new_param;
} UI_Config_t;

// 2. System_Config.h - increment version
#define CONFIG_VERSION  3  // Was 2

// 3. System_Config.c - Config_InitDefaults()
g_system_config.ui.new_param = 100;  // Default value

// 4. System_Config.c - Config_ParseLine()
else if (strcmp(key, "ui.new_param") == 0) {
    g_system_config.ui.new_param = (uint32_t)atoi(value);
    params_found++;
}

// 5. System_Config.c - Config_WriteToFile()
snprintf(buf, sizeof(buf), "ui.new_param=%lu\r\n", cfg->ui.new_param);

// 6. System_Config.c - Config_PrintToUSB()
USB_Log_Printf("  new_param: %lu\r\n", cfg->ui.new_param);

// 7. System_Config.c - Update expected count
const uint32_t EXPECTED_PARAMS_V3 = 66;  // Was V2=65, added 1

// 8. USB_Command_Handler.c - handle_set_command()
else if (strcmp(param, "ui.new_param") == 0) {
    cfg->ui.new_param = (uint32_t)atoi(value);
    changed = true;
}
```

## Module Runtime Control System

All major subsystems are managed as runtime-controllable modules that can be started/stopped dynamically and configured to auto-start at boot.

### Module System Components

Every module must be integrated into these locations:

1. **System.h** - Add to `System_Module_t` enum
2. **System.h** - Add to `System_Task_ID_t` enum (for watchdog tracking)
3. **System.c** - Add to `Task_ID_t` internal enum
4. **System.c** - Add to `s_task_wdt_status[]` array with name string
5. **System.c** - Add to `s_module_states[]` array with initial state
6. **System.c** - Add to `s_module_names[]` array with display name
7. **System.c** - Add to `system_init()` with conditional startup based on config
8. **System.c** - Add case to `System_StartModule()` switch statement
9. **System.c** - Add case to `System_StopModule()` switch statement
10. **System.c** - Add to `boot_enabled[]` array in `System_PrintModuleStatus()`
11. **System_Config.h** - Add bool field to `Modules_Config_t` struct
12. **System_Config.c** - Add default value in `Config_InitDefaults()`
13. **System_Config.c** - Add parser in `Config_ParseLine()`
14. **System_Config.c** - Update `EXPECTED_PARAMS_Vx` count
15. **System_Config.c** - Add to `Config_WriteToFile()`
16. **System_Config.c** - Add to `Config_PrintToUSB()`
17. **USB_Command_Handler.c** - Add to `parse_module_name()` function
18. **USB_Command_Handler.c** - Update help text in `cmd_start()` and `cmd_stop()`
19. **USB_Command_Handler.c** - Add to `handle_set_command()` for runtime config

### Example: Adding RS485 Module

```c
// 1. System.h - Module enum
typedef enum {
    MODULE_LCD_DISPLAY = 0,
    MODULE_MIFARE_POLLING,
    MODULE_DISPENSER,
    MODULE_BUZZER,
    MODULE_IO_EXPANDER,
    MODULE_RS485,  // ← Add new module
    MODULE_COUNT
} System_Module_t;

// 2. System.h - Task ID for watchdog
typedef enum {
    SYSTEM_TASK_ID_SD_LOGGER = 0,
    // ... other tasks
    SYSTEM_TASK_ID_RS485,  // ← Add task ID
} System_Task_ID_t;

// 3. System.c - Watchdog tracking
static Task_WDT_Status_t s_task_wdt_status[TASK_ID_COUNT] = {
    // ... other tasks
    {"RS485", TASK_STATUS_UNKNOWN, 0}  // ← Add with name
};

// 4. System.c - Module names
static const char* s_module_names[MODULE_COUNT] = {
    // ... other names
    "RS485"  // ← Display name
};

// 5. System.c - Conditional startup
if (cfg->modules.rs485_enabled) {
    Task_Start_RS485_Task();
    s_module_states[MODULE_RS485] = MODULE_STATE_RUNNING;
    LOG_CRITICAL_SYSTEM("[→] RS485 Task started\r\n");
} else {
    LOG_CRITICAL_SYSTEM("[!] RS485 Task DISABLED by config\r\n");
}

// 6. System.c - Start/Stop handlers
case MODULE_RS485:
    Task_Start_RS485_Task();
    success = true;
    break;

// 7. System_Config.h - Config struct
typedef struct {
    bool lcd_display_enabled;
    bool mifare_polling_enabled;
    bool dispenser_enabled;
    bool buzzer_enabled;
    bool io_expander_enabled;
    bool rs485_enabled;  // ← Add config field
} Modules_Config_t;

// 8. System_Config.c - Default value
g_system_config.modules.rs485_enabled = true;

// 9. System_Config.c - Parser
else if (strcmp(k, "modules.rs485_enabled") == 0) {
    g_system_config.modules.rs485_enabled = (atoi(v) != 0);
    params_found++;
}

// 10. USB_Command_Handler.c - Name parser
if (strcasecmp(name, "rs485") == 0 || strcasecmp(name, "rs485_comm") == 0) {
    return MODULE_RS485;
}
```

### Module Status Display

The `modules` USB command shows all modules:

```
═══════════════════════════════════════════════════════════════
                    MODULE STATUS                                
═══════════════════════════════════════════════════════════════
Module               Boot Config  Runtime State
───────────────────────────────────────────────────────────────
LCD_Display          Enabled      RUNNING     
MIFARE_Polling       Enabled      RUNNING     
Dispenser            Enabled      RUNNING     
Buzzer               Enabled      RUNNING     
IO_Expander          Enabled      RUNNING     
RS485                Enabled      RUNNING     
═══════════════════════════════════════════════════════════════

Commands: start <module>, stop <module>
Modules: lcd, mifare, dispenser, buzzer, ioexp, rs485
```

### Key Points

- **Always update all 19 locations** when adding a new module
- **Module names** should be concise for USB commands (e.g., "rs485", "lcd")
- **Display names** can be more descriptive (e.g., "RS485", "LCD_Display")
- **Config version** must increment when adding module config fields
- **Expected param count** must update in System_Config.c
- Modules start **conditionally** based on `cfg->modules.<name>_enabled`

## CMake Source File Management

This project uses **explicit source file lists** in project-specific CMakeLists.txt instead of `file(GLOB ...)`. This provides better build reliability and ensures CMake properly detects when files are added or removed.

### Shared vs Project-Specific CMakeLists.txt

| Location | Uses GLOB? | Reason |
|----------|------------|--------|
| `pico_port/CMakeLists.txt` | **No** - Explicit lists | Project-specific, we control all files |
| `sevantica_drivers/CMakeLists.txt` | **Yes** - GLOB allowed | Shared library across multiple projects |

**Why the difference?**
- `sevantica_drivers` is in `Common/` and shared by multiple projects
- Each project may need different driver files
- GLOB provides flexibility for shared libraries
- Project-specific files should use explicit lists for reliability

### Adding/Removing Application Source Files

Update `APP_SOURCES` in `pico_port/CMakeLists.txt`:

```cmake
set(APP_SOURCES
    ../application/Source/MyWota_ui_driver.c
    ../application/Source/Dispenser_Controller.c
    ../application/Source/Hardware_Access.c
    ../application/Source/IO_Expander_Control.c
    ../application/Source/MIFARE_Transaction_Manager.c
    ../application/Source/SD_Logger_Task.c
    ../application/Source/System.c
    ../application/Source/System_Config.c
    ../application/Source/USB_Command_Handler.c
    # Add new application source files here
)
```

### Adding/Removing Driver Source Files

For the shared `sevantica_drivers` library:
1. Simply add/remove the `.c` or `.h` file in the appropriate directory
2. Run CMake configure (`Configure Build` task) to pick up changes
3. The GLOB pattern will automatically include new files

### Why No GLOB in Project Files?

CMake's `file(GLOB ...)` has issues:
1. **Build system doesn't detect new files** - Adding a file won't trigger CMake reconfiguration
2. **Deleted files cause stale builds** - Removed files may still be compiled from cache
3. **Hard to review changes** - Can't see what files are included in version control diffs

### Quick Reference

| File Location | CMakeLists.txt | Update Method |
|---------------|----------------|---------------|
| `application/Source/*.c` | `pico_port/CMakeLists.txt` | Edit `APP_SOURCES` list |
| `pico_port/*.c` | `pico_port/CMakeLists.txt` | Edit `MAIN_SOURCES` list |
| `sevantica_drivers/Source/*.c` | `sevantica_drivers/CMakeLists.txt` | Just add file, run Configure |
| `sevantica_drivers/Include/*.h` | `sevantica_drivers/CMakeLists.txt` | Just add file, run Configure |
