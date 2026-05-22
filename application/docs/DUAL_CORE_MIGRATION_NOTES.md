# Dual-Core FreeRTOS Migration Notes

This note captures what is needed to move the MyWota RP2040 firmware from the current single-core FreeRTOS setup to two-core/SMP operation, plus the expected tradeoffs.

## Current State

- The project currently runs FreeRTOS on one RP2040 core.
- `pico_port/freertos_config/FreeRTOSConfig.h` hard-codes `FREE_RTOS_KERNEL_SMP` to `0`.
- `pico_port/CMakeLists.txt` selects the `GCC_RP2040` FreeRTOS port.
- The shared RP2040 FreeRTOS port already contains SMP support and launches core 1 internally when `configNUMBER_OF_CORES > 1`.
- Application code should not manually call `multicore_launch_core1()` for FreeRTOS SMP.
- The firmware uses static FreeRTOS allocation, so SMP requires memory for the extra passive idle task.

## Required Changes

### 1. Enable SMP in FreeRTOSConfig.h

Update `pico_port/freertos_config/FreeRTOSConfig.h` so SMP is enabled and FreeRTOS sees two cores.

Suggested shape:

```c
#ifndef FREE_RTOS_KERNEL_SMP
#define FREE_RTOS_KERNEL_SMP 1
#endif

#if FREE_RTOS_KERNEL_SMP
#define configNUMBER_OF_CORES                   2
#define configNUM_CORES                         configNUMBER_OF_CORES
#define configTICK_CORE                         0
#define configRUN_MULTIPLE_PRIORITIES           1
#define configUSE_CORE_AFFINITY                 1
#define configUSE_PASSIVE_IDLE_HOOK             0
#define configTASK_DEFAULT_CORE_AFFINITY        (1u << 0)
#endif
```

`configTASK_DEFAULT_CORE_AFFINITY` is optional, but recommended for the first migration step. It keeps existing tasks on core 0 by default so core 1 can be enabled without immediately changing task behavior.

### 2. Add Passive Idle Task Static Memory

Because dynamic FreeRTOS allocation is disabled, SMP needs an additional callback in `application/Source/System/FreeRTOS_Static.c`:

```c
#if ( configNUMBER_OF_CORES > 1 )
void vApplicationGetPassiveIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer,
                                           StackType_t **ppxIdleTaskStackBuffer,
                                           configSTACK_DEPTH_TYPE *puxIdleTaskStackSize,
                                           BaseType_t xPassiveIdleTaskIndex )
{
    static StaticTask_t xPassiveIdleTaskTCBs[ configNUMBER_OF_CORES - 1 ];
    static StackType_t uxPassiveIdleTaskStacks[ configNUMBER_OF_CORES - 1 ][ configMINIMAL_STACK_SIZE ];

    *ppxIdleTaskTCBBuffer = &xPassiveIdleTaskTCBs[ xPassiveIdleTaskIndex ];
    *ppxIdleTaskStackBuffer = uxPassiveIdleTaskStacks[ xPassiveIdleTaskIndex ];
    *puxIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
#endif
```

### 3. Decide Task Affinity

Do not let all tasks float between cores at first. Start with everything pinned to core 0, then move low-risk tasks one at a time.

Good candidates to evaluate for core 1 later:

- RS485 task
- feedback/polling task
- non-UI background work
- CPU-heavy calculations, if added later

Tasks to keep conservative initially:

- LVGL/LCD/UI task
- TinyUSB/USB command work
- SD/FatFS logging
- flash/config persistence
- shared I2C/SPI peripheral access

Use `xTaskCreateStaticAffinitySet()` when creating tasks, or call `vTaskCoreAffinitySet()` after `xTaskCreateStatic()`.

Example masks:

```c
#define CORE0_AFFINITY (1u << 0)
#define CORE1_AFFINITY (1u << 1)
#define ANY_CORE_AFFINITY (CORE0_AFFINITY | CORE1_AFFINITY)
```

### 4. Audit Shared Resources

Single-core FreeRTOS has concurrency, but only one task executes at a time. Two-core SMP allows true simultaneous execution, so shared state must be protected carefully.

Review at minimum:

- SPI0 sharing between LCD and SD card
- I2C bus users
- TinyUSB and USB logging paths
- LVGL calls and display buffer access
- FatFS/SD card access
- flash writes and config persistence
- watchdog task status arrays
- global controller/module state
- ring buffers, queues, and command parser state

Prefer FreeRTOS mutexes, queues, and task notifications over ad hoc global flags.

### 5. Clean Configure and Rebuild

After changing FreeRTOS core-count/config macros, do a clean configure and build rather than relying on an incremental Ninja build.

Recommended order:

1. Clean build directory.
2. Configure CMake.
3. Build.
4. Flash.
5. Watch startup logs and watchdog status.

## Disadvantages and Risks

- More race conditions: code that was effectively serialized on one core may break when two tasks execute at the same time.
- Harder debugging: faults, breakpoints, and task state become core-dependent.
- More RAM use: SMP adds per-core kernel state plus an additional idle task stack and TCB.
- More flash/code size: SMP paths and synchronization code increase firmware size.
- More scheduling overhead: cross-core yields, spinlocks, and SMP critical sections cost CPU time.
- More jitter: timing-sensitive peripheral work can become less predictable if tasks migrate or compete for shared locks.
- Higher power use: core 1 running reduces idle/sleep opportunities.
- Not automatically faster: SPI, I2C, USB, SD, flash, and LVGL remain shared bottlenecks.

## Recommended Migration Strategy

1. Enable SMP, but set `configTASK_DEFAULT_CORE_AFFINITY` to core 0.
2. Add passive idle task memory and confirm the firmware boots with two cores enabled.
3. Keep all existing tasks on core 0 for the first validation pass.
4. Move one low-risk task to core 1.
5. Test watchdog reporting, USB command handling, SD logging, UI refresh, dispenser behavior, and RS485 behavior.
6. Continue moving tasks only when there is a measured reason.

The safest first milestone is not performance. It is proving that the firmware can boot and run stably with the SMP scheduler active while behavior remains equivalent to the current single-core build.