/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * FreeRTOS Task Stack and Priority Configuration
 *
 * This header defines stack sizes and priorities for all FreeRTOS tasks.
 * Stack sizes are in bytes and converted to words for xTaskCreate().
 *
 ******************************************************************************
 */

#ifndef TASK_STACK_CONFIG_H
#define TASK_STACK_CONFIG_H

#include "FreeRTOS.h"

/* ============================================================================
 * Stack Size Calculation Macros
 * ============================================================================ */

/* Convert bytes to FreeRTOS stack words (StackType_t is 4 bytes) */
#define STACK_BYTES_TO_WORDS(bytes) ((bytes) / sizeof(StackType_t))

/* ============================================================================
 * Task Stack Sizes (in bytes)
 * ============================================================================ */

/* System Task - Monitors watchdog and system health */
#define SYSTEM_TASK_STACK_BYTES     (384u * 4)   /* 1536 bytes - optimized: 266w free (59%) */
#define SYSTEM_TASK_STACK_WORDS     STACK_BYTES_TO_WORDS(SYSTEM_TASK_STACK_BYTES)

/* SD Logger Task - File I/O with fatfs library (high memory usage) */
#define SD_LOGGER_TASK_STACK_BYTES  (896u * 4)   /* 3584 bytes - increased: 68w free → need 25% margin */
#define SD_LOGGER_TASK_STACK_WORDS  STACK_BYTES_TO_WORDS(SD_LOGGER_TASK_STACK_BYTES)

/* USB CDC Task - USB communication and logging */
#define USB_CDC_TASK_STACK_BYTES    (224u * 4)   /* 896 bytes - increased: 60w free → need safer margin */
#define USB_CDC_TASK_STACK_WORDS    STACK_BYTES_TO_WORDS(USB_CDC_TASK_STACK_BYTES)

/* USB Command Handler Task - Parse and execute commands */
#define USB_COMMAND_HANDLER_TASK_STACK_BYTES (224u * 4)  /* 896 bytes - increased: 66w free → need safer margin */
#define USB_COMMAND_HANDLER_TASK_STACK_WORDS STACK_BYTES_TO_WORDS(USB_COMMAND_HANDLER_TASK_STACK_BYTES)

/* LCD Display Task - LVGL rendering and UI updates */
#define LCD_DISPLAY_TASK_STACK_BYTES (960u * 4)  /* 3840 bytes - CRITICAL: only 40w free! */
#define LCD_DISPLAY_TASK_STACK_WORDS STACK_BYTES_TO_WORDS(LCD_DISPLAY_TASK_STACK_BYTES)

/* Car Wash Controller Task - State machine and relay control */
#define CARWASH_TASK_STACK_BYTES    (256u * 4)   /* 1024 bytes - optimized from 384w */
#define CARWASH_TASK_STACK_WORDS    STACK_BYTES_TO_WORDS(CARWASH_TASK_STACK_BYTES)

/* Buzzer Polling Task - Check module states for audible feedback */
#define BUZZER_POLLING_TASK_STACK_BYTES     (96u * 4)   /* 384 bytes - optimized: 114w free (89%) */
#define BUZZER_POLLING_TASK_STACK_WORDS     STACK_BYTES_TO_WORDS(BUZZER_POLLING_TASK_STACK_BYTES)

/* MIFARE Polling Task - Card detection and polling */
#define MIFARE_POLLING_TASK_STACK_BYTES (700u * 4) /* 2560 bytes - was hardcoded 2048w, actual usage ~602w */
#define MIFARE_POLLING_TASK_STACK_WORDS  STACK_BYTES_TO_WORDS(MIFARE_POLLING_TASK_STACK_BYTES)

/* IO Expander Control Task - I/O expander interrupt handling */
#define IO_EXPANDER_TASK_STACK_BYTES (192u * 4)  /* 768 bytes - increased: 36w free → need safer margin */
#define IO_EXPANDER_TASK_STACK_WORDS STACK_BYTES_TO_WORDS(IO_EXPANDER_TASK_STACK_BYTES)

/* RTC Task - RTC manager and SD persistence */
#define RTC_TASK_STACK_BYTES        (512u * 4)   /* 2048 bytes - increased: 42w free → need safer margin */
#define RTC_TASK_STACK_WORDS        STACK_BYTES_TO_WORDS(RTC_TASK_STACK_BYTES)

/* RS485 Task - RS485 communication and firmware update */
#define RS485_TASK_STACK_BYTES      (576u * 4)   /* 2304 bytes - INCREASED: only 70w free (critical!) */
#define RS485_TASK_STACK_WORDS      STACK_BYTES_TO_WORDS(RS485_TASK_STACK_BYTES)

/* ============================================================================
 * Task Priorities
 * ============================================================================
 * FreeRTOS priority levels: 0 = Idle, higher = higher priority
 * Typical range: tskIDLE_PRIORITY (0) to (configMAX_PRIORITIES - 1)
 * 
 * Priority scheme (lower to higher):
 *   Idle priority (0)      - Not used, reserved for idle task
 *   Polling tasks (1)      - Buzzer, MIFARE polling, low-priority monitor
 *   Normal tasks (2)       - LCD display, IO expander, RTC, SD logger (if not critical)
 *   Critical tasks (3)     - USB CDC (prevents priority inversion), System monitor
 *
 * IMPORTANT: Higher number = higher priority. USB is highest to prevent
 * priority inversion when logging from other tasks.
 * ============================================================================ */

/* USB CDC Task - Highest priority to prevent deadlocks */
#define USB_CDC_TASK_PRIORITY           (tskIDLE_PRIORITY + 3)

/* USB Transaction Task - Higher priority than USB CDC to ensure smooth data flow */
#define USB_TRANSACTION_TASK_PRIORITY   (tskIDLE_PRIORITY + 4)

/* System Watchdog Monitor Task - High priority */
#define SYSTEM_TASK_PRIORITY            (tskIDLE_PRIORITY + 2)

/* SD Logger Task - Normal priority */
#define SD_LOGGER_TASK_PRIORITY         (tskIDLE_PRIORITY + 1)

/* LCD Display Task - Normal priority */
#define LCD_DISPLAY_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)

/* USB Command Handler Task - Normal priority */
#define USB_COMMAND_HANDLER_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)

/* Car Wash Controller Task - Normal priority */
#define CARWASH_TASK_PRIORITY           (tskIDLE_PRIORITY + 1)

/* MIFARE Polling Task - Low priority (polling is less urgent) */
#define MIFARE_POLLING_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)

/* Buzzer Polling Task - Low priority (audible feedback is non-critical) */
#define BUZZER_POLLING_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)

/* IO Expander Control Task - Low priority */
#define IO_EXPANDER_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)

/* RTC Task - Low priority (RTC updates are infrequent) */
#define RTC_TASK_PRIORITY               (tskIDLE_PRIORITY + 1)

/* RS485 Task - Normal priority (communication is time-sensitive) */
#define RS485_TASK_PRIORITY             (tskIDLE_PRIORITY + 1)

#endif /* TASK_STACK_CONFIG_H */
