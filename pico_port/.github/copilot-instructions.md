# Copilot Instructions for BigYellow Firmware

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
  - `MIFARE_CarWash_IsWashActive()` - Monitors wash state for car wash start/finish
- Buzzer task handles beep patterns internally based on state transitions

### Benefits
- Decoupled modules - easier testing and maintenance
- No circular dependencies
- Clear data flow direction
- Consistent architecture across all modules

### Implementation Pattern
```c
// ❌ WRONG - Direct call (push pattern)
void CarWash_StartWash(void) {
    Buzzer_Beep(buzzer, 200);  // DON'T DO THIS
}

// ✅ CORRECT - Polling pattern
bool MIFARE_CarWash_IsWashActive(void) {
    return g_wash_timer.wash_active;  // Getter for polling
}

// Buzzer task polls this getter every 50ms
static void Buzzer_PollingTaskFunc(void *pvParameters) {
    bool last_wash_state = false;
    while(1) {
        bool current = MIFARE_CarWash_IsWashActive();
        if (current != last_wash_state) {
            if (current) Buzzer_SignalDispenserStart(handle);
            else Buzzer_SignalDispenserStop(handle);
            last_wash_state = current;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

## Adapter Pattern for Cross-Project Code Sharing

When functionality differs between projects (BigYellow vs MyWota), use the **adapter pattern** to keep core logic shared in `sevantica_drivers` while isolating project-specific behavior in application adapters.

### Pattern Structure

**Core files (in sevantica_drivers/):**
- Shared implementation used by both projects
- Contains common logic, state management, and interfaces
- Auto-included via per-feature CMake blocks in sevantica_drivers/CMakeLists.txt

**Adapter files (in application/):**
- Project-specific implementations
- Small files containing only differences between projects
- Explicitly listed in project CMakeLists.txt

### Example: MIFARE Transaction Management

**Core (shared):**
- `sevantica_drivers/Source/MIFARE_Transaction_Core.c` - Common card operations
- `sevantica_drivers/Include/MIFARE_Transaction_Core.h` - Core interface

**Adapters (project-specific):**
- BigYellow: `application/Source/MIFARE_Token_Adapter.c` - Token-based car wash
- MyWota: `application/Source/MIFARE_Volume_Adapter.c` - Volume-based dispensing

### Example: USB Command Handler

**Core (shared):**
- `sevantica_drivers/Source/USB_Command_Handler.c` - Command parsing, core commands
- `sevantica_drivers/Include/USB_Command_Handler.h` - Core interface

**Adapters (project-specific):**
- BigYellow: `application/Source/USB_Command_Adapter.c` - Wash commands (washstart/washstop)
- MyWota: `application/Source/USB_Command_Adapter.c` - Dispenser commands (dispensestart/stop/wait, msc)

### Adapter Interface Functions

Each adapter must implement standard functions called by the core:

```c
// In USB_Command_Adapter.h (both projects)
const USB_Command_Adapter_Entry_t* USB_Command_Adapter_GetCommands(void);
size_t USB_Command_Adapter_GetCommandCount(void);
void USB_Command_Adapter_PrintStatus(void);
const char* USB_Command_Adapter_GetIncludesInfo(void);
```

### Benefits

✅ **Single source of truth** - Core logic maintained in one place  
✅ **Easy updates** - Bug fixes automatically apply to both projects  
✅ **Clear separation** - Project differences isolated in small adapter files  
✅ **No duplication** - Common code isn't copied between projects  
✅ **Build efficiency** - Core files added once to the library CMake feature blocks  

### When to Use Adapters

Use this pattern when:
- Functionality is >80% identical between projects
- Differences are isolated to specific operations/commands
- Projects share the same underlying architecture
- You want to avoid maintaining duplicate files

**Don't use** for completely different implementations - just make separate files.

### Critical Rule

**ALWAYS place core files in `sevantica_drivers/`, NOT in application folders.**

This ensures:
- Single file to maintain (not duplicated per project)
- Inclusion via sevantica_drivers feature toggles
- Clear separation of shared vs project-specific code

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

## CMake Source File Management

This project uses **explicit source file lists** in project-specific CMakeLists.txt instead of `file(GLOB ...)`. This provides better build reliability and ensures CMake properly detects when files are added or removed.

### Shared vs Project-Specific CMakeLists.txt

| Location | Source selection | Reason |
|----------|------------------|--------|
| `pico_port/CMakeLists.txt` | **Explicit `APP_SOURCES` list** | Project-specific, we control all files |
| `sevantica_drivers/CMakeLists.txt` | **Explicit, feature-toggled** via `USE_DRIVERS_*` macros read from the parent project's `System_Config.h` | Shared library; each project enables only the drivers it needs |

**Why the difference?**
- `sevantica_drivers` is shared by multiple projects (MyWota, BigYellow, Central Control Hub)
- Each project enables a different driver set via `USE_DRIVERS_<FEATURE>` macros in its `System_Config.h`
- The library's CMake reads those macros and conditionally appends sources to `DRIVERS_SOURCES`
- Headers in the shared library are GLOB'd, but **sources are not** — adding a new `.c` to a feature group requires editing `sevantica_drivers/CMakeLists.txt`
- Project-specific files use an explicit list for reliability

### Adding/Removing Application Source Files

Update `APP_SOURCES` in `pico_port/CMakeLists.txt`:

```cmake
set(APP_SOURCES
    ../application/Source/bigYellow_ui_driver.c
    ../application/Source/Car_Wash_Controller.c
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
1. Add the `.c` file under the appropriate `Source/<Group>/` directory.
2. **Edit `sevantica_drivers/CMakeLists.txt`** and append the new source to the matching
   feature-toggled block (e.g. inside the `if(USE_DRIVERS_NFC)` block for an NFC file).
3. If the file belongs to a brand-new feature, add a new `USE_DRIVERS_<FEATURE>` macro in
   each project's `System_Config.h` and a matching `if()` block in the library CMake.
4. Run the `Configure Build` task to pick up the changes.

> Note: only headers are GLOB'd in `sevantica_drivers/CMakeLists.txt`; sources are explicit.

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