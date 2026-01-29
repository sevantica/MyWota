# MyWota Agent Guide: Development & Diagnosis

This guide outlines the project structure, build system, and coding standards for the **MyWota Water Dispenser** project. Follow these instructions when writing code or diagnosing issues.

## 1. Project Structure & Build System

### 1.1. Workspace Layout
- **Application Root**: `MyWota/application`
  - Contains specific application logic, tasks, and adapters.
  - Organized into `System`, `Tasks`, `Controllers`, and `Adapters`.
- **Build Root**: `MyWota/pico_port`
  - **CRITICAL**: This is the actual CMake project root.
  - Contains the main `CMakeLists.txt`, `main.c`, and linker scripts.
- **Shared Drivers**: `sevantica_drivers`
  - A comprehensive library of reusable drivers (RS485, USB, FatFs, etc.).
  - Configured via `System_Config.h` in the application.

### 1.2. The Build System (CMake)
- **Target Name**: `UI_PICO_PORT`
- **Adding New Application Files**:
  - You **MUST** manually add new source files to the `APP_SOURCES` list in `pico_port/CMakeLists.txt`.
  - Do not assume `glob` is used. Explicit listing is enforced.
- **Adding New Driver Files**:
  - Add to `sevantica_drivers/CMakeLists.txt` within the relevant feature block (e.g., `DRIVERS_ENABLE_RS485`).

### 1.3. Driver Configuration
- Drivers are feature-toggled to save flash.
- To enable a driver, edit `application/Include/System/System_Config.h` and set:
  ```c
  #define USE_DRIVERS_XYZ 1
  ```
- `sevantica_drivers/CMakeLists.txt` reads this file to decide what to compile.

---

## 2. Common Compilation Issues & Fixes

### 2.1. "Undefined Reference"
1. **Check `pico_port/CMakeLists.txt`**: Did you add the new `.c` file to `APP_SOURCES`?
2. **Check `sevantica_drivers` Toggles**: Is the required driver enabled in `System_Config.h`?
3. **Check Library Linking**: Is the driver library actually linked? (It is usually linked as `Drivers`).

### 2.2. "File Not Found"
- Ensure include paths are correct. The build adds:
  - `application/Include` (and subdirs: `System`, `Tasks`, `Controllers`, `Adapters`)
  - `sevantica_drivers/Include`
  - `sevantica_drivers/FatFs`

---

## 3. Coding Standards & Patterns (Strict Compliance)

### 3.1. Memory Management (Static Only)
- **Rule**: **NO DYNAMIC ALLOCATION**.
- Use `xTaskCreateStatic`, `xQueueCreateStatic`, `xSemaphoreCreateStatic`.
- **Reason**: `FREERTOS_HEAP` is configured for static use to ensure determinism.

### 3.2. Task Watchdog Pattern
Every FreeRTOS task **MUST** implement the heartbeat pattern:
```c
#include "Heartbeat_Task.h"

// In main loop:
for (;;) {
    TASK_HEARTBEAT_EVERY_SECOND("MyTaskName");
    System_ReportTaskStatus(SYSTEM_TASK_ID_MY_TASK, true);
    // ... task logic ...
}
```

### 3.3. Logging Pattern
**DO NOT** use raw `printf` or `USB_Log_Printf` directly. Use module-specific macros:
```c
#define LOG_DEBUG_MYMOD_EN      1
#define LOG_ERROR_MYMOD_EN      1

#if LOG_DEBUG_MYMOD_EN
    #define LOG_DEBUG_MYMOD(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_MYMOD(...)
#endif
```

### 3.4. Status Codes
**DO NOT** use `bool` for success/failure. Define a specific enum and a string converter:
```c
typedef enum {
    MYMOD_OK = 0,
    MYMOD_ERROR_BUSY,
    MYMOD_ERROR_TIMEOUT
} MyMod_Status_t;

const char* MyMod_GetStatusString(MyMod_Status_t status);
```

### 3.5. Driver Handles
Avoid bare global variables. Use a handle accessor:
```c
MyMod_Handle_t* MyMod_GetHandle(uint8_t index);
```

---

## 4. Workflow for Agents

1. **Read Request**: Identify if the change is in `App` or `Drivers`.
2. **Search**: Locate relevant files (Source in `application/Source/...`, Include in `application/Include/...`).
3. **Implement**: Write code following **Section 3** patterns.
   - **Crucial**: If adding a new task, ensure it uses **Static Allocation**.
4. **Register**: 
   - Add new source files to `pico_port/CMakeLists.txt`.
   - Update `System_Config.h` if needed.
