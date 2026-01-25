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
#define SD_LOGGER_TASK_STACK_BYTES  (1536u * 4)  /* 6144 bytes - increased for exFAT/LFN support */
#define SD_LOGGER_TASK_STACK_WORDS  STACK_BYTES_TO_WORDS(SD_LOGGER_TASK_STACK_BYTES)

/* USB CDC Task - USB communication and logging */
#define USB_CDC_TASK_STACK_BYTES    (224u * 4)   /* 896 bytes - increased: 60w free → need safer margin */
#define USB_CDC_TASK_STACK_WORDS    STACK_BYTES_TO_WORDS(USB_CDC_TASK_STACK_BYTES)

/* USB Command Handler Task - Parse and execute commands */
/* Increased for WiFi Scan operations which require significant stack depth */
#define USB_COMMAND_HANDLER_TASK_STACK_BYTES (1024u * 4)  /* 4096 bytes */
#define USB_COMMAND_HANDLER_TASK_STACK_WORDS STACK_BYTES_TO_WORDS(USB_COMMAND_HANDLER_TASK_STACK_BYTES)

/* LCD Display Task - LVGL rendering and UI updates */
#define LCD_DISPLAY_TASK_STACK_BYTES (1024u * 4)  /* 4096 bytes - Increased for safety */
#define LCD_DISPLAY_TASK_STACK_WORDS STACK_BYTES_TO_WORDS(LCD_DISPLAY_TASK_STACK_BYTES)

/* Dispenser Controller Task - State machine and valve control */
#define DISPENSER_TASK_STACK_BYTES    (768u * 4)   /* 3072 bytes - Increased for MIFARE crypto ops */
#define DISPENSER_TASK_STACK_WORDS    STACK_BYTES_TO_WORDS(DISPENSER_TASK_STACK_BYTES)

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
#define RS485_TASK_STACK_BYTES      (768u * 4)   /* 3072 bytes - Increased for safety */
#define RS485_TASK_STACK_WORDS      STACK_BYTES_TO_WORDS(RS485_TASK_STACK_BYTES)

/* Network Task - WiFi and LwIP handling */
#define NETWORK_TASK_STACK_BYTES    (3072u * 4)  /* 12288 bytes - Massive stack for safety */
#define NETWORK_TASK_STACK_WORDS    STACK_BYTES_TO_WORDS(NETWORK_TASK_STACK_BYTES)

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



/* Dispenser Controller Task - Normal priority */
#define DISPENSER_TASK_PRIORITY         (tskIDLE_PRIORITY + 1)

/* MIFARE Polling Task - Low priority (polling is less urgent) */
#define MIFARE_POLLING_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)

/* Buzzer Polling Task - Low priority (audible feedback is non-critical) */
#define BUZZER_POLLING_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)

/* IO Expander Control Task - Low priority */
#define IO_EXPANDER_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)

/* RTC Task - Low priority (RTC updates are infrequent) */
#define RTC_TASK_PRIORITY               (tskIDLE_PRIORITY + 1)

/* RS485 Task - Normal priority (communication is time-sensitive) */
#define RS485_TASK_PRIORITY             (tskIDLE_PRIORITY + 2)

/* Network Task - Higher priority for network responsiveness */
#define NETWORK_TASK_PRIORITY           (tskIDLE_PRIORITY + 2)

#endif /* TASK_STACK_CONFIG_H */
