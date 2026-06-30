/*
 * MyWota UI Driver - Optimized for STM32F411 with ILI9488
 * 
 * Features:
 * - LVGL integration with ILI9488 LCD controller
 * - DMA-optimized SPI transfers with cache-aligned buffers
 * - RGB666 format support for ILI9488 SPI compatibility
 * - FreeRTOS task-based architecture with semaphore protection
 * - Dynamic UI updates with progress bar and label controls
 * - Memory-optimized for STM32F411 constraints
 * 
 * @attention Copyright (c) Sevantica 2025. All rights reserved.
 ******************************************************************************
 */

/* ========================================================================== */
/*                                 INCLUDES                                  */
/* ========================================================================== */
#include <stdint.h>
#include <stdio.h>
#include <string.h> /* For memset, memcpy functions */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "MyWota_ui_driver.h"
#include "LCD_Driver.h"
#include "lvgl.h"
#include "ui.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "System_Events.h"
#include "Hardware_Access.h" /* For SPI_MSG_DEF and centralized hardware definitions */
#include "USB_Logging.h"
#include "Event_Broker.h"
#include "MIFARE_Transaction_Core.h"  /* For shared MIFARE state enums */
#include "Dispenser_Controller.h"        /* For dispenser functions */
#include "System_Config.h"                /* For SD card configuration */
#include "CLI_Processor.h"
#include "RS485_Command_Adapter.h"
#include "RS485_FW_Update.h"
#ifdef LV_USE_ILI9341
#include "display/ili9341/lv_ili9341.h"
#endif

#ifdef LV_USE_ILI9488
#include "display/ili9488/lv_ili9488.h"
#endif

#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"

/* ========================================================================== */
/*                           PRIVATE DEFINITIONS                             */
/* ========================================================================== */

/* Timing Configuration */
#define LVGL_TASK_PERIOD_MS             5U
#define LCD_INITIAL_RENDER_TIMEOUT_MS   3000U
#define LCD_INITIAL_RENDER_LOG_MS       500U
#define UI_TOTAL_REMAINING_BAR_GREEN    0x05820AU

/* UI Display Context - tracks UI-specific state for display persistence */
typedef struct {
    /* Card removal persistence - keep UI visible for a timeout after card removed */
    bool was_card_present;              /* Previous card state for edge detection */
    uint32_t card_removed_time;         /* Timestamp when card was removed */
    bool showing_persisted_data;        /* True if showing data after card removal */
    
    /* Dispense cooldown tracking - show dispensed amount after dispense stops */
    bool was_dispensing;                /* Previous dispense state for edge detection */
    uint32_t dispense_stopped_time;     /* Timestamp when dispensing stopped */
    bool in_cooldown;                   /* True if in cooldown period after dispense */
    uint32_t dispensed_amount_ml;       /* Amount dispensed to show during cooldown */
    
    /* LED flashing state - reserved for future use */
    uint32_t last_led_toggle_time;
    bool led_is_on;
    
    /* Module failure tracking */
    bool has_module_failures;
    uint8_t failed_module_count;
    System_Module_t failed_modules[MODULE_COUNT];
    uint32_t last_failed_module_cycle_time;
    uint8_t current_failed_module_index;
    
    /* Cached display values (shown during persistence timeout) */
    char last_phone_number[12];
    uint32_t last_card_balance_ml;
    uint32_t card_balance_ml;
    uint32_t last_topup_ml;
    MIFARE_CardState_t card_state;
    MIFARE_TransactionState_t transaction_state;
    MIFARE_OperationContext_t operation_context;
    bool rfid_card_present;
    bool admin_card_present;
    bool phone_valid;
    
    /* Force UI refresh on next update (e.g., when new card inserted) */
    bool force_refresh_on_next_update;

    /* Short-lived manual dispense error banner, driven by operation-stop events */
    bool manual_no_flow_display_active;
    uint32_t manual_no_flow_display_start_time;

    /* Operation state snapshot, driven by operation events */
    bool dispense_active;
    bool manual_dispense_active;
    bool self_clean_active;
    uint32_t dispense_amount_ml;
    uint32_t dispense_remaining_ml;
} UI_Display_Context_t;

/* Timing Constants */
#define ONE_SECOND_MS                   1000U
#define FPS_UPDATE_INTERVAL_MS          ONE_SECOND_MS
#define LCD_RESET_DELAY_MS              500U
#define DISPENSE_COOLDOWN_MS            5000U  /* 5 second cooldown after dispense stops */
#define MANUAL_NO_FLOW_DISPLAY_MS       10000U
#define LCD_EVENT_QUEUE_LENGTH          8U

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_LCD_DISPLAY_DRIVER_EN      1
#define LOG_CRITICAL_LCD_DISPLAY_DRIVER_EN   1
#define LOG_ERROR_LCD_DISPLAY_DRIVER_EN      1

#if LOG_DEBUG_LCD_DISPLAY_DRIVER_EN
    #define LOG_DEBUG_LCD_DISPLAY_DRIVER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_LCD_DISPLAY_DRIVER(...)
#endif

#if LOG_CRITICAL_LCD_DISPLAY_DRIVER_EN
    #define LOG_CRITICAL_LCD_DISPLAY_DRIVER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_LCD_DISPLAY_DRIVER(...)
#endif

#if LOG_ERROR_LCD_DISPLAY_DRIVER_EN
    #define LOG_ERROR_LCD_DISPLAY_DRIVER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_LCD_DISPLAY_DRIVER(...)
#endif


/* ========================================================================== */
/*                           PRIVATE VARIABLES                               */
/* ========================================================================== */


/* Synchronization */
static SemaphoreHandle_t lvgl_mutex = NULL;
static StaticSemaphore_t lvgl_mutex_buffer;

/* Task Handle */
static TaskHandle_t lcd_task_handle;
static StaticTask_t lcd_task_tcb;
static StackType_t lcd_task_stack[LCD_DISPLAY_TASK_STACK_WORDS];
static QueueHandle_t lcd_event_queue = NULL;
static StaticQueue_t lcd_event_queue_buffer;
static uint8_t lcd_event_queue_storage[LCD_EVENT_QUEUE_LENGTH * sizeof(Event_t*)];

/* Performance Monitoring - removed unused frame_count variable */

/* UI State Variables */
static unsigned long elapsed_seconds = 0;            /* Elapsed seconds */
static uint8_t  screen_switched = 0;
static uint8_t  card_present = 0;
static unsigned long start_tick_ms = 0;              /* Start tick */
/* (FPS tracking removed - was unused) */
static unsigned long last_data_poll_ms = 0;          /* Last data poll tick */

/* UI Display Context */
static UI_Display_Context_t ui_ctx = {
    .was_card_present = false,
    .card_removed_time = 0,
    .showing_persisted_data = false,
    .was_dispensing = false,
    .dispense_stopped_time = 0,
    .in_cooldown = false,
    .dispensed_amount_ml = 0,
    .last_led_toggle_time = 0,
    .led_is_on = false,
    .has_module_failures = false,
    .failed_module_count = 0,
    .last_failed_module_cycle_time = 0,
    .current_failed_module_index = 0,
    .last_phone_number = "",  /* Will be set to config value on first update */
    .last_card_balance_ml = 0,
    .card_balance_ml = 0,
    .last_topup_ml = 0,
    .card_state = MIFARE_CARD_STATE_ABSENT,
    .transaction_state = TRANSACTION_STATE_IDLE,
    .operation_context = MIFARE_CONTEXT_NONE,
    .rfid_card_present = false,
    .admin_card_present = false,
    .phone_valid = false,
    .force_refresh_on_next_update = false,
    .manual_no_flow_display_active = false,
    .manual_no_flow_display_start_time = 0,
    .dispense_active = false,
    .manual_dispense_active = false,
    .self_clean_active = false,
    .dispense_amount_ml = 0,
    .dispense_remaining_ml = 0
};

/* Performance Optimization - Cache previous state to avoid redundant updates */
static UI_State_t last_applied_ui_state = UI_STATE_COUNT;  /* Invalid state forces first update */
static ValveState_t last_valve_state = (ValveState_t)0xFF;  /* Invalid value forces first update */
static uint32_t last_dispense_timer_seconds = 0xFFFFFFFF;  /* Cache for dispenser timer */
static uint32_t last_dispense_token_count = 0xFFFFFFFF;  /* Cache for token count display */
static char last_customer_id[32] = "";  /* Cache for customer ID */
static int32_t last_total_remaining_percentage = -1;  /* Cache for percentage display */

/* Background color cache - detect config changes for live updates */
static uint32_t last_bg_color = 0xFFFFFFFF;
static uint32_t last_bg_grad_color = 0xFFFFFFFF;
static uint32_t last_title_bar_color = 0xFFFFFFFF;
static uint8_t last_bg_main_stop = 0xFF;
static uint8_t last_bg_grad_stop = 0xFF;

/* ========================================================================== */
/*                           EXTERNAL VARIABLES                              */
/* ========================================================================== */
/* No external HAL variables needed for Pico SDK implementation */

/* ========================================================================== */
/*                           TASK CONFIGURATION                              */
/* ========================================================================== */

/* Stack size provided by centralized Task_Stack_Config.h */
static const uint16_t lcd_display_task_stack_size_words = LCD_DISPLAY_TASK_STACK_WORDS;

/* ========================================================================== */
/*                         FUNCTION PROTOTYPES                               */
/* ========================================================================== */

/* ========================================================================== */
/*                         UTILITY FUNCTION PROTOTYPES                       */
/* ========================================================================== */
static void update_ui_from_system_state(void);
static void reset_ui_to_idle(void);
static void init_ui_visibility_from_config(void);
static UI_State_t map_card_state_to_ui_state(MIFARE_CardState_t card_state, bool is_dispensing);
static void apply_ui_state_config(UI_State_t ui_state);
static void apply_background_colors_from_config(void);
static void set_obj_hidden(lv_obj_t * obj, bool hidden);
static void apply_firmware_update_percentage(int32_t percentage);
static void apply_firmware_update_ui(uint32_t bytes_received, uint32_t expected_size);
static void drain_lcd_events(void);

/* ========================================================================== */
/*                         CORE TASK FUNCTION PROTOTYPES                     */
/* ========================================================================== */
static void LCD_Display_Task(void *argument);



/* (removed dead lcd_fill_test) */

/* ========================================================================== */
/*                           MAIN TASK FUNCTION                              */
/* ========================================================================== */


static lv_display_t *lcd_display = NULL;

/**
 * @brief Custom delay callback for LVGL using FreeRTOS
 * @param ms Milliseconds to delay
 */
static void lvgl_freertos_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void drain_lcd_events(void)
{
    if (lcd_event_queue == NULL) {
        return;
    }

    Event_t* event = NULL;
    while (xQueueReceive(lcd_event_queue, &event, 0) == pdTRUE) {
        if (event != NULL) {
            if (event->header.id == EVT_OPERATION_START &&
                event->header.size >= sizeof(Event_Operation_Start_t)) {
                Event_Operation_Start_t* start_event = (Event_Operation_Start_t*)event;
                if (start_event->operation_kind == EVENT_OPERATION_KIND_DISPENSE) {
                    ui_ctx.dispense_active = true;
                    ui_ctx.manual_dispense_active = (start_event->operation_mode == EVENT_OPERATION_MODE_MANUAL);
                    ui_ctx.dispense_amount_ml = 0;
                    ui_ctx.dispense_remaining_ml = start_event->target_ml > 0 ?
                                                    start_event->target_ml :
                                                    start_event->balance_ml;
                    ui_ctx.force_refresh_on_next_update = true;
                } else if (start_event->operation_kind == EVENT_OPERATION_KIND_SELF_CLEAN) {
                    ui_ctx.self_clean_active = true;
                    ui_ctx.force_refresh_on_next_update = true;
                }
            } else if (event->header.id == EVT_OPERATION_PROGRESS &&
                       event->header.size >= sizeof(Event_Operation_Progress_t)) {
                Event_Operation_Progress_t* progress_event = (Event_Operation_Progress_t*)event;
                if (progress_event->operation_kind == EVENT_OPERATION_KIND_DISPENSE) {
                    ui_ctx.dispense_active = true;
                    ui_ctx.manual_dispense_active = (progress_event->operation_mode == EVENT_OPERATION_MODE_MANUAL);
                    ui_ctx.dispense_amount_ml = progress_event->amount_ml;
                    ui_ctx.dispense_remaining_ml = progress_event->remaining_ml;
                } else if (progress_event->operation_kind == EVENT_OPERATION_KIND_SELF_CLEAN) {
                    ui_ctx.self_clean_active = true;
                }
            } else if (event->header.id == EVT_OPERATION_STOP &&
                event->header.size >= sizeof(Event_Operation_Stop_t)) {
                Event_Operation_Stop_t* stop_event = (Event_Operation_Stop_t*)event;
                if (stop_event->operation_kind == EVENT_OPERATION_KIND_DISPENSE) {
                    bool manual_dispense = (stop_event->operation_mode == EVENT_OPERATION_MODE_MANUAL);
                    ui_ctx.dispense_active = false;
                    ui_ctx.manual_dispense_active = false;
                    ui_ctx.dispense_amount_ml = stop_event->amount_ml;
                    ui_ctx.dispense_remaining_ml = 0;

                    if (manual_dispense && stop_event->stop_reason == EVENT_OPERATION_STOP_REASON_NO_FLOW) {
                        ui_ctx.manual_no_flow_display_active = true;
                        ui_ctx.manual_no_flow_display_start_time = xTaskGetTickCount();
                    } else if (manual_dispense) {
                        ui_ctx.manual_no_flow_display_active = false;
                        ui_ctx.manual_no_flow_display_start_time = 0;
                    }
                    ui_ctx.force_refresh_on_next_update = true;
                } else if (stop_event->operation_kind == EVENT_OPERATION_KIND_SELF_CLEAN) {
                    ui_ctx.self_clean_active = false;
                    ui_ctx.force_refresh_on_next_update = true;
                }
            } else if (event->header.id == EVT_RFID_STATE_CHANGED &&
                       event->header.size >= sizeof(Event_RFID_State_t)) {
                Event_RFID_State_t* state_event = (Event_RFID_State_t*)event;
                bool auto_dispense_active = ui_ctx.dispense_active && !ui_ctx.manual_dispense_active;
                ui_ctx.card_state = (MIFARE_CardState_t)state_event->card_state;
                ui_ctx.transaction_state = (MIFARE_TransactionState_t)state_event->transaction_state;
                ui_ctx.operation_context = (MIFARE_OperationContext_t)state_event->operation_context;
                ui_ctx.rfid_card_present = (state_event->flags & EVENT_RFID_STATE_FLAG_CARD_PRESENT) != 0u;
                ui_ctx.admin_card_present = (state_event->flags & EVENT_RFID_STATE_FLAG_ADMIN_CARD) != 0u;
                ui_ctx.phone_valid = (state_event->flags & EVENT_RFID_STATE_FLAG_PHONE_VALID) != 0u;
                if (!auto_dispense_active) {
                    ui_ctx.card_balance_ml = state_event->balance;
                }
                ui_ctx.last_topup_ml = state_event->last_topup;
                if (ui_ctx.phone_valid) {
                    strncpy(ui_ctx.last_phone_number, state_event->customer_id, sizeof(ui_ctx.last_phone_number) - 1);
                    ui_ctx.last_phone_number[sizeof(ui_ctx.last_phone_number) - 1] = '\0';
                }
                if (!auto_dispense_active && ui_ctx.card_state == MIFARE_CARD_STATE_PRESENT && ui_ctx.card_balance_ml > 0) {
                    ui_ctx.last_card_balance_ml = ui_ctx.card_balance_ml;
                }
                ui_ctx.force_refresh_on_next_update = true;
            } else if (event->header.id == EVT_RFID_BALANCE_CONFIRMED &&
                       event->header.size >= sizeof(Event_Balance_Updated_t)) {
                Event_Balance_Updated_t* balance_event = (Event_Balance_Updated_t*)event;
                ui_ctx.card_balance_ml = balance_event->balance;
                ui_ctx.last_topup_ml = balance_event->last_topup;
                if (balance_event->balance > 0) {
                    ui_ctx.last_card_balance_ml = balance_event->balance;
                }
                ui_ctx.force_refresh_on_next_update = true;
            } else if (event->header.id == EVT_DISPENSER_STATUS_CHANGED &&
                       event->header.size >= sizeof(Event_Dispenser_Status_t)) {
                Event_Dispenser_Status_t* status_event = (Event_Dispenser_Status_t*)event;
                ui_ctx.dispense_active = (status_event->flags & EVENT_DISPENSER_FLAG_ACTIVE) != 0u;
                ui_ctx.self_clean_active = (status_event->flags & EVENT_DISPENSER_FLAG_SELF_CLEAN) != 0u;
                ui_ctx.manual_dispense_active = (status_event->flags & EVENT_DISPENSER_FLAG_NO_CARD_MODE) != 0u;
                ui_ctx.dispense_amount_ml = status_event->dispensed_ml;
                ui_ctx.dispense_remaining_ml = status_event->remaining_ml;
            }
            Event_Release(event);
        }
    }
}

/**
 * @brief Main LCD Display Driver Task
 * @param argument Task argument (unused)
 */
static void LCD_Display_Task(void *argument)
{
    (void)argument;
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Starting LCD Display Driver initialization...\r\n");
    
    /* Initialize GPIO pins */
    lcd_gpio_init();
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: GPIO initialized\r\n");
    
    /* Initialize synchronization primitives */
    lvgl_mutex = xSemaphoreCreateMutexStatic(&lvgl_mutex_buffer);
    if (lvgl_mutex == NULL) {
        LOG_ERROR_LCD_DISPLAY_DRIVER("LCD: ERROR - Failed to create LVGL mutex\r\n");
        LOG_CRITICAL_LCD_DISPLAY_DRIVER("[✗] LCD Display Task initialization FAILED\r\n");
        vTaskDelete(NULL);
        return;
    }

    lcd_event_queue = xQueueCreateStatic(LCD_EVENT_QUEUE_LENGTH,
                                         sizeof(Event_t*),
                                         lcd_event_queue_storage,
                                         &lcd_event_queue_buffer);
    if (lcd_event_queue == NULL) {
        LOG_ERROR_LCD_DISPLAY_DRIVER("LCD: ERROR - Failed to create event queue\r\n");
    } else if (!EventBroker_Subscribe(lcd_event_queue, EVT_OPERATION_START) ||
               !EventBroker_Subscribe(lcd_event_queue, EVT_OPERATION_PROGRESS) ||
               !EventBroker_Subscribe(lcd_event_queue, EVT_OPERATION_STOP) ||
               !EventBroker_Subscribe(lcd_event_queue, EVT_RFID_STATE_CHANGED) ||
               !EventBroker_Subscribe(lcd_event_queue, EVT_RFID_BALANCE_CONFIRMED) ||
               !EventBroker_Subscribe(lcd_event_queue, EVT_DISPENSER_STATUS_CHANGED)) {
        LOG_ERROR_LCD_DISPLAY_DRIVER("LCD: ERROR - Failed to subscribe to state events\r\n");
    }

    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Resetting display controller...\r\n");
    Hardware_LCD_Reset();
    vTaskDelay(pdMS_TO_TICKS(105)); /* Match standalone driver timing */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Reset complete\r\n");
    
    /* Turn on backlight */
   
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Initialize LVGL and the display driver */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Initializing LVGL library...\r\n");
    lv_init();
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: LVGL initialized\r\n");
    
    /* Set custom delay callback to use FreeRTOS vTaskDelay instead of busy-wait */
    lv_delay_set_cb(lvgl_freertos_delay);
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Custom FreeRTOS delay registered\r\n");

    /* Create the display using conditional compilation based on lv_conf.h */
#if LV_USE_ILI9488
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Creating ILI9488 display driver (%dx%d)...\r\n", DISPLAY_VERTICAL_SIZE, DISPLAY_HORIZONTAL_SIZE);
    lcd_display = lv_ili9488_create(DISPLAY_VERTICAL_SIZE, DISPLAY_HORIZONTAL_SIZE,
                                    (lv_lcd_flag_t)(LV_LCD_FLAG_RGB666 | LV_LCD_FLAG_MIRROR_X),
                                    lcd_1_send_cmd,
                                    lcd_1_send_color);
#elif LV_USE_ILI9341
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Creating ILI9341 display driver (%dx%d)...\r\n", DISPLAY_HORIZONTAL_SIZE, DISPLAY_VERTICAL_SIZE);
    lcd_display = lv_ili9341_create(DISPLAY_HORIZONTAL_SIZE, DISPLAY_VERTICAL_SIZE,
                                    LV_LCD_FLAG_MIRROR_X | LV_LCD_FLAG_MIRROR_Y,
                                    lcd_1_send_cmd,
                                    lcd_1_send_color);
#else
    #error "No display controller defined"
#endif

    if (lcd_display == NULL) {
        LOG_ERROR_LCD_DISPLAY_DRIVER("LCD: ERROR - Display creation failed\r\n");
        LOG_CRITICAL_LCD_DISPLAY_DRIVER("[✗] LCD Display Task initialization FAILED\r\n");
        // Display creation failed - cleanup and exit
        vSemaphoreDelete(lvgl_mutex);
        vTaskDelete(NULL);
        return;
    }
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Display driver created successfully\r\n");
    
    /* Set display rotation to landscape orientation */
#if LV_USE_ILI9488
    /* ILI9488: Rotate 90 degrees for landscape (480x320) */
    lv_display_set_rotation(lcd_display, LV_DISPLAY_ROTATION_90);
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Display rotation set to 90 degrees (landscape)\r\n");
#elif LV_USE_ILI9341
    /* ILI9341: Rotate 90 degrees for landscape (320x240) */
    lv_display_set_rotation(lcd_display, LV_DISPLAY_ROTATION_0);
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Display rotation set to 90 degrees (landscape)\r\n");
#endif
    
    /* For now, create a basic display buffer setup */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Configuring display buffers (%d bytes)...\r\n", DISPLAY_BUFFER_SIZE);
    lv_display_set_buffers(lcd_display, display_buffer, NULL, DISPLAY_BUFFER_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Display buffers configured\r\n");

    /* Optional: set panel gap/offsets if the glass has non-zero origin. Start with 0,0. */
   
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Initializing UI...\r\n");
    ui_init();
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: UI initialized\r\n");
    
    /* Apply visibility settings from config */
    init_ui_visibility_from_config();
    
    /* Keep backlight off initially - will fade in after full screen render */
    LCD_Driver_SetBacklight(0);
    
    /* Reset lines rendered counter before initial render */
    lcd_reset_lines_rendered();
    
    /* Get actual display height after rotation (LVGL handles rotation internally) */
    int32_t display_height = lv_display_get_vertical_resolution(lcd_display);
    
    /* Trigger LVGL render and wait until full screen is rendered.
     * Full screen height depends on rotation - use LVGL's reported height */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Rendering initial UI (waiting for %ld lines)...\r\n", (long)display_height);

    TickType_t initial_render_start = xTaskGetTickCount();
    TickType_t initial_render_last_tick = initial_render_start;
    TickType_t initial_render_last_log = initial_render_start;

    while (lcd_get_lines_rendered() < (uint32_t)display_height) {
        TickType_t tick_now = xTaskGetTickCount();
        uint32_t elapsed_ms = (uint32_t)((tick_now - initial_render_last_tick) * portTICK_PERIOD_MS);
        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
            initial_render_last_tick = tick_now;
        }

        lv_timer_handler();
        System_ReportTaskStatus(SYSTEM_TASK_ID_LCD_DISPLAY, true);

        uint32_t render_elapsed_ms = (uint32_t)((tick_now - initial_render_start) * portTICK_PERIOD_MS);
        uint32_t log_elapsed_ms = (uint32_t)((tick_now - initial_render_last_log) * portTICK_PERIOD_MS);
        if (log_elapsed_ms >= LCD_INITIAL_RENDER_LOG_MS) {
            initial_render_last_log = tick_now;
            LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Initial render progress %lu/%ld lines after %lu ms\r\n",
                                         lcd_get_lines_rendered(),
                                         (long)display_height,
                                         (unsigned long)render_elapsed_ms);
        }

        if (render_elapsed_ms >= LCD_INITIAL_RENDER_TIMEOUT_MS) {
            LOG_ERROR_LCD_DISPLAY_DRIVER("LCD: Initial render timed out at %lu/%ld lines after %lu ms\r\n",
                                         lcd_get_lines_rendered(),
                                         (long)display_height,
                                         (unsigned long)render_elapsed_ms);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(5)); /* Small delay between render passes */
    }
    
    /* Wait for SPI transfers to complete - display controller needs time
     * to receive and display all the pixel data after LVGL finishes flushing */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Initial render complete (%lu lines rendered)\r\n", lcd_get_lines_rendered());
    
    /* Now fade in backlight smoothly after UI is fully rendered to screen */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Fading in backlight...\r\n");
    for (uint8_t brightness = 0; brightness <= 100; brightness += 5) {
        LCD_Driver_SetBacklight(brightness);
        vTaskDelay(pdMS_TO_TICKS(30)); /* 30ms per step = 600ms total fade time */
    }
    LCD_Driver_SetBacklight(100); /* Ensure we end at exactly 100% */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Backlight at 100%%, initialization complete\r\n");
    
    LOG_CRITICAL_LCD_DISPLAY_DRIVER("[✓] LCD Display Task initialized successfully\r\n");
    
    /* MAIN TASK LOOP */
    TickType_t xLastWakeTime = xTaskGetTickCount();
    start_tick_ms = xTaskGetTickCount();
    last_data_poll_ms = start_tick_ms;

    for(;;) {
        /* Feed watchdog every second - MUST be first in loop */
        TASK_HEARTBEAT_EVERY_SECOND("LCD");
        System_ReportTaskStatus(SYSTEM_TASK_ID_LCD_DISPLAY, true);
        drain_lcd_events();
       
        /* Update UI based on system state at regular intervals */
        unsigned long now_tick = xTaskGetTickCount();
        uint32_t data_poll_interval = Config_Get()->ui.data_poll_interval_ms;
        if ((now_tick - last_data_poll_ms) >= data_poll_interval) {
            last_data_poll_ms = now_tick;
            
            /* Acquire LVGL protection semaphore */
            if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10)) == pdPASS) {
                update_ui_from_system_state();
                xSemaphoreGive(lvgl_mutex);
            }
        }
        
        /* Acquire LVGL protection semaphore with timeout to prevent WDT triggers */
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(500)) != pdPASS) {
            /* Failed to acquire mutex - skip this frame but keep reporting status */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Update LVGL tick counter using actual elapsed time to avoid clock drift
         * (the loop sleeps a variable amount based on lv_timer_handler) */
        static TickType_t last_tick_inc = 0;
        TickType_t tick_now = xTaskGetTickCount();
        if (last_tick_inc != 0) {
            uint32_t elapsed_ms = (uint32_t)((tick_now - last_tick_inc) * portTICK_PERIOD_MS);
            if (elapsed_ms > 0) {
                lv_tick_inc(elapsed_ms);
            }
        }
        last_tick_inc = tick_now;

        /* Handle screen switching - happens only once after defined delay */
        uint32_t screen_switch_delay_seconds = Config_Get()->ui.screen_switch_delay_ms / 1000U;
        if (!screen_switched && elapsed_seconds >= screen_switch_delay_seconds) 
        {
            /* Backlight already at full brightness from fade-in */
            screen_switched = 1;
        }

        /* Process LVGL rendering */
        unsigned long time_till_next = lv_timer_handler();

        /* Increment time counter only every 1000 ms */
        elapsed_seconds = (xTaskGetTickCount() - start_tick_ms) / ONE_SECOND_MS; /* derive seconds */

        /* Release semaphore */
        xSemaphoreGive(lvgl_mutex);

    /* Dynamic delay based on LVGL recommendation */
    if(time_till_next> 100)
    {
        time_till_next = Config_Get()->ui.display_refresh_ms;
    }

    if (time_till_next < LVGL_TASK_PERIOD_MS) 
    {
        time_till_next = LVGL_TASK_PERIOD_MS;
    }
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(time_till_next));
    }
}

/* ========================================================================== */
/*                            PUBLIC API FUNCTIONS                           */
/* ========================================================================== */

/**
 * @brief Start the LCD Display Driver Task
 */
void Task_Start_LCD_Display_Task(void)
{
    if (lcd_task_handle != NULL) {
        LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Task already running\r\n");
        return;
    }
    lcd_task_handle = xTaskCreateStatic(LCD_Display_Task, "LCD_Task", lcd_display_task_stack_size_words, NULL, LCD_DISPLAY_TASK_PRIORITY, lcd_task_stack, &lcd_task_tcb);
}

/**
 * @brief Stop the LCD Display Driver Task
 */
void Task_Stop_LCD_Display_Driver_Task(void)
{
    if (lcd_task_handle != NULL) {
        vTaskDelete(lcd_task_handle);
        lcd_task_handle = NULL;
        LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Task stopped\r\n");
    }
}

/**
 * @brief Get the LCD Display Driver Task handle
 * @return Task handle
 */
TaskHandle_t task_get_handle_LCD_Display_Driver_Task(void) { return lcd_task_handle; }

/* ========================================================================== */
/*                           LVGL INTERFACE FUNCTIONS                        */
/* ========================================================================== */


/* ========================================================================== */
/*                            UI CONTROL FUNCTIONS                           */
/* ========================================================================== */

/**
 * @brief Set visibility of a UI object
 * @param obj Pointer to the UI object
 * @param visible true to show, false to hide
 * @return true if successful, false otherwise
 */
bool ui_set_visibility(lv_obj_t * obj, bool visible)
{
    if (lvgl_mutex == NULL || obj == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdPASS) {
        if (visible) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        xSemaphoreGive(lvgl_mutex);
        return true;
    }
    return false;
}

/**
 * @brief Set text of a label object
 * @param label Pointer to the label object
 * @param text Text string to set
 * @return true if successful, false otherwise
 */
bool ui_set_label_text(lv_obj_t * label, const char * text)
{
    if (lvgl_mutex == NULL || label == NULL || text == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdPASS) {
        lv_label_set_text(label, text);
        lv_obj_invalidate(label); /* Force refresh */
        xSemaphoreGive(lvgl_mutex);
        return true;
    }
    return false;
}

/**
 * @brief Set value of a bar object
 * @param bar Pointer to the bar object
 * @param value Value to set
 * @param anim Animation enable/disable
 * @return true if successful, false otherwise
 */
bool ui_set_bar_value(lv_obj_t * bar, int32_t value, lv_anim_enable_t anim)
{
    if (lvgl_mutex == NULL || bar == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdPASS) {
        lv_bar_set_value(bar, value, anim);
        xSemaphoreGive(lvgl_mutex);
        return true;
    }
    return false;
}

/**
 * @brief Set background color style of an object
 * @param obj Pointer to the object
 * @param color Color to set
 * @param selector Style selector (e.g., LV_PART_MAIN | LV_STATE_DEFAULT)
 * @return true if successful, false otherwise
 */
bool ui_set_obj_style_bg_color(lv_obj_t * obj, lv_color_t color, lv_style_selector_t selector)
{
    if (lvgl_mutex == NULL || obj == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100)) == pdPASS) {
        lv_obj_set_style_bg_color(obj, color, selector);
        xSemaphoreGive(lvgl_mutex);
        return true;
    }
    return false;
}

/* ========================================================================== */
/*                          UI UPDATE FUNCTIONS                              */
/* ========================================================================== */

/**
 * @brief Reset UI to idle/default state
 */
static void reset_ui_to_idle(void)
{
    /* Apply IDLE state configuration (includes visibility) */
    apply_ui_state_config(UI_STATE_IDLE);
    
    // Get configured no-card user ID from SD card
    const SystemConfig_t* config = Config_Get();
    
    if (ui_customerID != NULL) {
        lv_label_set_text(ui_customerID, config->ui.no_card_customer_id);
    }
    
    /* Reset cached values */
    strncpy(ui_ctx.last_phone_number, config->ui.no_card_customer_id, sizeof(ui_ctx.last_phone_number) - 1);
    ui_ctx.last_phone_number[sizeof(ui_ctx.last_phone_number) - 1] = '\0';
    ui_ctx.last_card_balance_ml = 0;
    ui_ctx.rfid_card_present = false;
    ui_ctx.showing_persisted_data = false;
    ui_ctx.manual_no_flow_display_active = false;
    ui_ctx.manual_no_flow_display_start_time = 0;
}

/**
 * @brief Map MIFARE card state to UI state
 * @param card_state Current card state from MIFARE driver
 * @param is_dispensing Whether dispenser is actively dispensing
 * @return UI_State_t Corresponding UI state
 */
static UI_State_t map_card_state_to_ui_state(MIFARE_CardState_t card_state, bool is_dispensing)
{
    /* If dispensing, override other states */
    if (is_dispensing) {
        return UI_STATE_DISPENSING;
    }
    
    /* Map card states */
    switch (card_state) {
        case MIFARE_CARD_STATE_ABSENT:
            return UI_STATE_IDLE;
            
        case MIFARE_CARD_STATE_INITIALIZING:
        case MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE:
            return UI_STATE_CARD_INITIALIZING;
            
        case MIFARE_CARD_STATE_PRESENT:
            return UI_STATE_CARD_READY;
            
        case MIFARE_CARD_STATE_ERROR:
            return UI_STATE_ERROR;
            
        default:
            return UI_STATE_IDLE;
    }
}

static void set_obj_hidden(lv_obj_t * obj, bool hidden)
{
    if (obj == NULL) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_firmware_update_percentage(int32_t percentage)
{
    char percentage_text[16];

    if (percentage < 0) {
        percentage = 0;
    }
    if (percentage > 100) {
        percentage = 100;
    }

    snprintf(percentage_text, sizeof(percentage_text), "%ld%%", (long)percentage);

    if (ui_customerID != NULL) {
        lv_label_set_text(ui_customerID, "Updating");
        set_obj_hidden(ui_customerID, false);
        snprintf(last_customer_id, sizeof(last_customer_id), "%s", "Updating");
    }

    if (ui_cardErrorStatus != NULL) {
        lv_label_set_text(ui_cardErrorStatus, "Wait");
        set_obj_hidden(ui_cardErrorStatus, false);
    }

    if (ui_cardRemaining != NULL) {
        lv_label_set_text(ui_cardRemaining, percentage_text);
        set_obj_hidden(ui_cardRemaining, false);
    }

    if (ui_totalRemainingBar != NULL) {
        lv_obj_set_style_bg_color(ui_totalRemainingBar, lv_color_hex(UI_TOTAL_REMAINING_BAR_GREEN), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_bar_set_value(ui_totalRemainingBar, percentage, LV_ANIM_OFF);
        set_obj_hidden(ui_totalRemainingBar, false);
    }

    set_obj_hidden(ui_flowRateSensor, true);
    set_obj_hidden(ui_buttonState, true);

    last_total_remaining_percentage = percentage;
    last_dispense_timer_seconds = 0xFFFFFFFF;
    last_dispense_token_count = 0xFFFFFFFF;
}

static void apply_firmware_update_ui(uint32_t bytes_received, uint32_t expected_size)
{
    int32_t percentage = 0;

    if (expected_size > 0U) {
        percentage = (int32_t)(((uint64_t)bytes_received * 100U) / expected_size);
    }

    apply_firmware_update_percentage(percentage);
}

/**
 * @brief Apply UI configuration for a specific state
 * @param ui_state UI state to apply configuration for
 * @note Called with LVGL semaphore held
 */
static void apply_ui_state_config(UI_State_t ui_state)
{
    if (ui_state >= UI_STATE_COUNT) {
        return;  /* Invalid state */
    }
    
    const SystemConfig_t* config = Config_Get();
    const UI_State_Config_t* state_cfg = &config->ui.states[ui_state];
    
    /* Apply visibility for customer ID based on state config */
    if (ui_customerID != NULL) {
        if (state_cfg->show_customer_id) {
            lv_obj_clear_flag(ui_customerID, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_customerID, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief Apply background colors from SD card configuration
 * @note Called periodically to enable live config updates via 'set' command
 */
static void apply_background_colors_from_config(void)
{
    const SystemConfig_t* config = Config_Get();
    
    /* Check if any background colors changed */
    bool colors_changed = (last_bg_color != config->ui.bg_color) ||
                          (last_bg_grad_color != config->ui.bg_grad_color) ||
                          (last_bg_main_stop != config->ui.bg_main_stop) ||
                          (last_bg_grad_stop != config->ui.bg_grad_stop) ||
                          (last_title_bar_color != config->ui.title_bar_color);
    
    if (!colors_changed) {
        return;  /* No changes - skip update */
    }
    
    if (ui_operationalScreen != NULL) {
        /* Apply main background color */
        lv_obj_set_style_bg_color(ui_operationalScreen, lv_color_hex(config->ui.bg_color), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_operationalScreen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        /* Apply gradient color */
        lv_obj_set_style_bg_grad_color(ui_operationalScreen, lv_color_hex(config->ui.bg_grad_color), LV_PART_MAIN | LV_STATE_DEFAULT);
        
        /* Apply gradient stops */
        lv_obj_set_style_bg_main_stop(ui_operationalScreen, config->ui.bg_main_stop, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_grad_stop(ui_operationalScreen, config->ui.bg_grad_stop, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        /* Apply gradient direction (vertical) */
        lv_obj_set_style_bg_grad_dir(ui_operationalScreen, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Applied background colors from config (0x%06lX -> 0x%06lX)\r\n",
                                     config->ui.bg_color, config->ui.bg_grad_color);
    }
    
    /* Note: Title bar color (blueButton1) removed - element not in current UI */
    
    /* Update cache */
    last_bg_color = config->ui.bg_color;
    last_bg_grad_color = config->ui.bg_grad_color;
    last_bg_main_stop = config->ui.bg_main_stop;
    last_bg_grad_stop = config->ui.bg_grad_stop;
    last_title_bar_color = config->ui.title_bar_color;
}

/**
 * @brief Initialize UI element visibility from config at startup
 * @note Called once after ui_init() to apply config visibility settings
 */
static void init_ui_visibility_from_config(void)
{
    const SystemConfig_t* config = Config_Get();
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Applying UI visibility from config...\r\n");
    
    /* Apply background colors from SD card configuration */
    apply_background_colors_from_config();
    
    /* Set customer ID initial text */
    if (ui_customerID != NULL) {
        lv_label_set_text(ui_customerID, config->ui.init_customer_id);
    }
    
    /* Apply IDLE state configuration (default startup state) */
    apply_ui_state_config(UI_STATE_IDLE);
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: UI visibility configured for IDLE state\r\n");
}

/* ========================================================================== */
/*                       UI DISPLAY STATE MACHINE                            */
/* ========================================================================== */

/**
 * UI_Disp_State_t is the single "mode" that drives every widget on the
 * operational screen.  It is evaluated fresh each render tick from the
 * event-cache snapshot in ui_ctx.  Transitions are pure (no side-effects in
 * the evaluator); each widget has its own render helper that switches on the
 * current state.
 *
 * Priority (highest first, first match wins):
 *   CLEANING  →  DISPENSING  →  DISPENSING_MANUAL  →  NO_FLOW_MANUAL
 *   →  ADMIN  →  CARD_READING  →  WAITING_REMOVAL  →  NO_FLOW_CARD
 *   →  CARD_ERROR  →  NO_BALANCE  →  CARD_READY  →  COOLDOWN
 *   →  PERSISTENCE  →  IDLE
 *
 * FW_UPDATE uses an early-return path before state evaluation (preserved).
 * MODULE_FAILURE overrides cardErrorStatus and ledIndicator within any state.
 */
typedef enum {
    UI_DS_IDLE = 0,
    UI_DS_CLEANING,
    UI_DS_CARD_READING,
    UI_DS_ADMIN,
    UI_DS_CARD_READY,
    UI_DS_DISPENSING,
    UI_DS_DISPENSING_MANUAL,
    UI_DS_NO_FLOW_MANUAL,
    UI_DS_NO_FLOW_CARD,
    UI_DS_NO_BALANCE,
    UI_DS_CARD_ERROR,
    UI_DS_WAITING_REMOVAL,
    UI_DS_COOLDOWN,
    UI_DS_PERSISTENCE,
    UI_DS_COUNT
} UI_Disp_State_t;

/* Format a millilitre value as "NNN ml", "N.N L" or "NN L" */
static void ui_fmt_ml(char *buf, size_t size, uint32_t ml)
{
    if (ml < 1000u) {
        snprintf(buf, size, "%lu ml", (unsigned long)ml);
    } else if (ml < 10000u) {
        unsigned long x10 = (unsigned long)(ml / 100u);
        snprintf(buf, size, "%lu.%lu L", x10 / 10u, x10 % 10u);
    } else {
        snprintf(buf, size, "%lu L", (unsigned long)(ml / 1000u));
    }
}

/* Pure state evaluator — no LVGL calls, no side-effects */
static UI_Disp_State_t ui_disp_determine_state(
    bool self_clean, bool dispense_active, bool card_present,
    bool manual_no_flow, bool admin_card,
    MIFARE_CardState_t cst, MIFARE_TransactionState_t txn,
    bool card_no_flow, bool card_auth, uint32_t balance_ml,
    bool in_cooldown, bool persisting)
{
    if (self_clean)                               return UI_DS_CLEANING;
    if (dispense_active && card_present)          return UI_DS_DISPENSING;
    if (dispense_active)                          return UI_DS_DISPENSING_MANUAL;
    if (manual_no_flow)                           return UI_DS_NO_FLOW_MANUAL;
    if (admin_card)                               return UI_DS_ADMIN;
    if (cst == MIFARE_CARD_STATE_INITIALIZING     ||
        cst == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE ||
        txn == TRANSACTION_STATE_CARD_DETECTED    ||
        txn == TRANSACTION_STATE_AUTHENTICATING   ||
        txn == TRANSACTION_STATE_READING_DATA     ||
        txn == TRANSACTION_STATE_VALIDATING)      return UI_DS_CARD_READING;
    if (txn == TRANSACTION_STATE_WAITING_REMOVAL) return UI_DS_WAITING_REMOVAL;
    if (card_no_flow && card_present)             return UI_DS_NO_FLOW_CARD;
    if (cst == MIFARE_CARD_STATE_ERROR)           return UI_DS_CARD_ERROR;
    if (cst == MIFARE_CARD_STATE_PRESENT &&
        balance_ml == 0u)                         return UI_DS_NO_BALANCE;
    if (card_auth)                                return UI_DS_CARD_READY;
    if (in_cooldown)                              return UI_DS_COOLDOWN;
    if (persisting)                               return UI_DS_PERSISTENCE;
    return UI_DS_IDLE;
}

/* Alternate between two strings every interval_ms */
static const char *ui_alt(bool *flag, uint32_t *last_tick,
                           uint32_t interval_ms,
                           const char *primary, const char *secondary)
{
    uint32_t now = (uint32_t)xTaskGetTickCount();
    if ((now - *last_tick) >= pdMS_TO_TICKS(interval_ms)) {
        *flag = !(*flag);
        *last_tick = now;
    }
    return *flag ? secondary : primary;
}

/* ─── Per-widget render helpers ─────────────────────────────────────────── */

static void render_customer_id(UI_Disp_State_t state, const char *no_card_id)
{
    if (ui_customerID == NULL) return;

    const char *text;
    switch (state) {
        case UI_DS_CLEANING:
            text = "System";
            break;
        case UI_DS_ADMIN:
            text = "Admin";
            break;
        case UI_DS_DISPENSING_MANUAL:
        case UI_DS_NO_FLOW_MANUAL:
            text = "Manual";
            break;
        case UI_DS_CARD_READING:
        case UI_DS_CARD_READY:
        case UI_DS_DISPENSING:
        case UI_DS_NO_FLOW_CARD:
        case UI_DS_NO_BALANCE:
        case UI_DS_CARD_ERROR:
        case UI_DS_WAITING_REMOVAL:
            text = ui_ctx.phone_valid ? ui_ctx.last_phone_number : no_card_id;
            break;
        case UI_DS_COOLDOWN:
        case UI_DS_PERSISTENCE:
            /* Card gone — keep showing last customer name if we have one */
            text = (ui_ctx.last_phone_number[0] != '\0')
                   ? ui_ctx.last_phone_number : no_card_id;
            break;
        default: /* UI_DS_IDLE */
            text = no_card_id;
            break;
    }

    if (strcmp(last_customer_id, text) != 0) {
        strncpy(last_customer_id, text, sizeof(last_customer_id) - 1);
        last_customer_id[sizeof(last_customer_id) - 1] = '\0';
        lv_label_set_text(ui_customerID, last_customer_id);
    }
}

static void render_card_error_status(UI_Disp_State_t state,
                                      bool card_waiting_tap,
                                      MIFARE_OperationContext_t op_ctx)
{
    if (ui_cardErrorStatus == NULL) return;

    /* Persistent toggle state for each alternating-message state */
    static bool     alt_waiting = false;  static uint32_t alt_waiting_t  = 0u;
    static bool     alt_no_flow = false;  static uint32_t alt_no_flow_t  = 0u;
    static bool     alt_no_bal  = false;  static uint32_t alt_no_bal_t   = 0u;
    static bool     alt_c_err   = false;  static uint32_t alt_c_err_t    = 0u;
    /* Dispensed-amount cache (shared by DISPENSING and COOLDOWN) */
    static uint32_t last_disp_x10   = 0xFFFFFFFFu;
    static char     last_err_text[16] = "";

    /* Module failures override every state */
    if (ui_ctx.has_module_failures) {
        static uint32_t mod_cycle_t = 0u;
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if ((now - mod_cycle_t) >= pdMS_TO_TICKS(2000u)) {
            ui_ctx.current_failed_module_index =
                (ui_ctx.current_failed_module_index + 1u) % ui_ctx.failed_module_count;
            mod_cycle_t = now;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%s",
                 System_GetModuleName(ui_ctx.failed_modules[ui_ctx.current_failed_module_index]));
        lv_label_set_text(ui_cardErrorStatus, buf);
        set_obj_hidden(ui_cardErrorStatus, false);
        return;
    }

    /* Show dispensed volume while dispensing or in cooldown (positive flow only) */
    uint32_t dispensed_ml = 0u;
    if (state == UI_DS_DISPENSING || state == UI_DS_DISPENSING_MANUAL) {
        dispensed_ml = ui_ctx.dispense_amount_ml;
    } else if (state == UI_DS_COOLDOWN) {
        dispensed_ml = ui_ctx.dispensed_amount_ml;
    }

    if (dispensed_ml > 0u) {
        uint32_t x10 = dispensed_ml / 100u;
        if (x10 != last_disp_x10) {
            last_disp_x10 = x10;
            char buf[16];
            if (x10 < 100u) {
                snprintf(buf, sizeof(buf), "%lu.%lu L",
                         (unsigned long)(x10 / 10u), (unsigned long)(x10 % 10u));
            } else {
                snprintf(buf, sizeof(buf), "%lu L", (unsigned long)(x10 / 10u));
            }
            lv_label_set_text(ui_cardErrorStatus, buf);
            strncpy(last_err_text, buf, sizeof(last_err_text) - 1);
            last_err_text[sizeof(last_err_text) - 1] = '\0';
        }
        set_obj_hidden(ui_cardErrorStatus, false);
        return;
    }

    /* Reset dispensed cache when not in a dispensing-amount state */
    last_disp_x10 = 0xFFFFFFFFu;

    const char *text = NULL;
    bool visible     = true;

    switch (state) {
        case UI_DS_IDLE:
            text = card_waiting_tap ? "Tap Card" : "No Card";
            break;
        case UI_DS_CLEANING:
            text = "Cleaning";
            break;
        case UI_DS_CARD_READING:
            text = "In Progress";
            break;
        case UI_DS_ADMIN:
            text = "Admin";
            break;
        case UI_DS_CARD_READY:
        case UI_DS_DISPENSING:           /* zero-flow active dispense: not started yet */
            text = "Ready";
            break;
        case UI_DS_DISPENSING_MANUAL:    /* zero-flow manual dispense: nothing to show yet */
            visible = false;
            break;
        case UI_DS_COOLDOWN:             /* zero-dispensed cooldown: shouldn't normally occur */
            visible = false;
            break;
        case UI_DS_NO_FLOW_MANUAL:
            text = "No Flow";
            break;
        case UI_DS_NO_FLOW_CARD:
            text = ui_alt(&alt_no_flow, &alt_no_flow_t, 2000u, "No Flow",    "Remove Card");
            break;
        case UI_DS_NO_BALANCE:
            text = ui_alt(&alt_no_bal,  &alt_no_bal_t,  2000u, "No Balance", "Remove Card");
            break;
        case UI_DS_CARD_ERROR:
            text = ui_alt(&alt_c_err,   &alt_c_err_t,   2000u, "No Init",    "Remove Card");
            break;
        case UI_DS_WAITING_REMOVAL: {
            const char *status = (op_ctx == MIFARE_CONTEXT_TOPUP) ? "Topped Up" : "Complete";
            text = ui_alt(&alt_waiting, &alt_waiting_t, 2000u, status, "Remove Card");
            break;
        }
        case UI_DS_PERSISTENCE:
            set_obj_hidden(ui_cardErrorStatus, false); /* keep last shown text */
            return;
        default:
            visible = false;
            break;
    }

    if (!visible) {
        set_obj_hidden(ui_cardErrorStatus, true);
        return;
    }

    if (text != NULL && strcmp(last_err_text, text) != 0) {
        strncpy(last_err_text, text, sizeof(last_err_text) - 1);
        last_err_text[sizeof(last_err_text) - 1] = '\0';
        lv_label_set_text(ui_cardErrorStatus, last_err_text);
    }
    set_obj_hidden(ui_cardErrorStatus, false);
}

static void render_card_remaining(UI_Disp_State_t state, uint32_t display_balance_ml)
{
    if (ui_cardRemaining == NULL) return;

    static uint32_t last_val = 0xFFFFFFFFu;
    bool     hidden = false;
    uint32_t val    = 0u;

    switch (state) {
        case UI_DS_IDLE:
        case UI_DS_CLEANING:
        case UI_DS_CARD_READING:
        case UI_DS_ADMIN:
        case UI_DS_NO_FLOW_MANUAL:
        case UI_DS_NO_BALANCE:
        case UI_DS_CARD_ERROR:
            hidden = true;
            break;
        case UI_DS_CARD_READY:
        case UI_DS_NO_FLOW_CARD:
        case UI_DS_WAITING_REMOVAL:
        case UI_DS_COOLDOWN:
        case UI_DS_PERSISTENCE:
            val    = display_balance_ml;
            hidden = (val == 0u);
            break;
        case UI_DS_DISPENSING:
            val    = display_balance_ml;
            hidden = false;
            break;
        case UI_DS_DISPENSING_MANUAL: {
            uint32_t remaining = (ui_ctx.showing_persisted_data && ui_ctx.last_card_balance_ml > 0u)
                                 ? ui_ctx.last_card_balance_ml
                                 : ui_ctx.dispense_remaining_ml;
            val    = remaining;
            hidden = false;
            break;
        }
        default:
            hidden = true;
            break;
    }

    set_obj_hidden(ui_cardRemaining, hidden);
    if (hidden) {
        last_val = 0xFFFFFFFFu;
        return;
    }

    if (val != last_val) {
        last_val = val;
        char buf[16];
        ui_fmt_ml(buf, sizeof(buf), val);
        lv_label_set_text(ui_cardRemaining, buf);
    }
}

static void render_total_remaining_bar(UI_Disp_State_t state,
                                        uint32_t balance_ml, uint32_t last_topup_ml)
{
    if (ui_totalRemainingBar == NULL) return;

    static int32_t last_pct = -1;
    int32_t pct = last_pct; /* default: keep last value */

    switch (state) {
        case UI_DS_IDLE:
        case UI_DS_CLEANING:
        case UI_DS_CARD_READING:
        case UI_DS_ADMIN:
        case UI_DS_NO_FLOW_MANUAL:
        case UI_DS_NO_BALANCE:
        case UI_DS_CARD_ERROR:
        case UI_DS_DISPENSING_MANUAL:
            pct = 0;
            break;
        case UI_DS_CARD_READY:
        case UI_DS_DISPENSING:
        case UI_DS_NO_FLOW_CARD:
        case UI_DS_WAITING_REMOVAL:
            if (last_topup_ml > 0u) {
                pct = (int32_t)((balance_ml * 100u) / last_topup_ml);
                if (pct < 0)   pct = 0;
                if (pct > 100) pct = 100;
            }
            break;
        default: /* COOLDOWN, PERSISTENCE: keep last */
            break;
    }

    if (pct != last_pct) {
        lv_bar_set_value(ui_totalRemainingBar, pct, LV_ANIM_OFF);
        last_pct = pct;
    }
}

static void render_level_colour(UI_Disp_State_t state, uint32_t display_balance_ml)
{
    if (ui_levelColourIndicator == NULL) return;

    typedef enum { LC_HIDDEN, LC_RED, LC_AMBER, LC_GREEN } LevelColor_t;
    static LevelColor_t last_lc = LC_HIDDEN;
    LevelColor_t lc = last_lc; /* default: keep last */

    switch (state) {
        case UI_DS_IDLE:
        case UI_DS_CLEANING:
        case UI_DS_CARD_READING:
        case UI_DS_ADMIN:
        case UI_DS_NO_FLOW_MANUAL:
        case UI_DS_NO_BALANCE:
        case UI_DS_CARD_ERROR:
        case UI_DS_DISPENSING_MANUAL:
            lc = LC_HIDDEN;
            break;
        case UI_DS_CARD_READY:
        case UI_DS_DISPENSING:
        case UI_DS_NO_FLOW_CARD:
        case UI_DS_WAITING_REMOVAL:
        case UI_DS_COOLDOWN:
        case UI_DS_PERSISTENCE: {
            uint32_t v = (display_balance_ml > 0u)
                         ? display_balance_ml : ui_ctx.last_card_balance_ml;
            lc = (v < 15000u) ? LC_RED : (v < 20000u) ? LC_AMBER : LC_GREEN;
            break;
        }
        default:
            break;
    }

    if (lc == last_lc) return;
    last_lc = lc;

    switch (lc) {
        case LC_RED:
            lv_obj_set_style_bg_color(ui_levelColourIndicator,
                                      lv_color_hex(0xCC0000u), LV_PART_MAIN | LV_STATE_DEFAULT);
            set_obj_hidden(ui_levelColourIndicator, false);
            break;
        case LC_AMBER:
            lv_obj_set_style_bg_color(ui_levelColourIndicator,
                                      lv_color_hex(0xFF9900u), LV_PART_MAIN | LV_STATE_DEFAULT);
            set_obj_hidden(ui_levelColourIndicator, false);
            break;
        case LC_GREEN:
            lv_obj_set_style_bg_color(ui_levelColourIndicator,
                                      lv_color_hex(0x05820Au), LV_PART_MAIN | LV_STATE_DEFAULT);
            set_obj_hidden(ui_levelColourIndicator, false);
            break;
        case LC_HIDDEN:
        default:
            set_obj_hidden(ui_levelColourIndicator, true);
            break;
    }
}

static void render_led_indicator(UI_Disp_State_t state)
{
    if (ui_ledIndicator == NULL) return;

    typedef enum { LED_HIDDEN, LED_GREEN, LED_FLASH_RED } LED_State_t;
    static LED_State_t last_led = LED_HIDDEN;
    LED_State_t led;

    if (ui_ctx.has_module_failures) {
        led = LED_FLASH_RED;
    } else {
        switch (state) {
            case UI_DS_ADMIN:
            case UI_DS_CARD_READY:
            case UI_DS_DISPENSING:
                led = LED_GREEN;
                break;
            case UI_DS_CARD_READING:
            case UI_DS_NO_FLOW_CARD:
            case UI_DS_NO_BALANCE:
            case UI_DS_CARD_ERROR:
            case UI_DS_WAITING_REMOVAL:
                led = LED_FLASH_RED;
                break;
            default:
                led = LED_HIDDEN;
                break;
        }
    }

    /* Flashing is handled every tick regardless of state-change */
    if (led == LED_FLASH_RED) {
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if ((now - ui_ctx.last_led_toggle_time) >= pdMS_TO_TICKS(250u)) {
            ui_ctx.led_is_on           = !ui_ctx.led_is_on;
            ui_ctx.last_led_toggle_time = now;
        }
        if (ui_ctx.led_is_on) {
            lv_obj_set_style_bg_color(ui_ledIndicator,
                                      lv_color_hex(0xFF0000u), LV_PART_MAIN | LV_STATE_DEFAULT);
            set_obj_hidden(ui_ledIndicator, false);
        } else {
            set_obj_hidden(ui_ledIndicator, true);
        }
        last_led = led;
        return;
    }

    if (led == last_led) return;
    last_led = led;

    switch (led) {
        case LED_GREEN:
            lv_obj_set_style_bg_color(ui_ledIndicator,
                                      lv_color_hex(0x00FF00u), LV_PART_MAIN | LV_STATE_DEFAULT);
            set_obj_hidden(ui_ledIndicator, false);
            break;
        case LED_HIDDEN:
        default:
            set_obj_hidden(ui_ledIndicator, true);
            break;
    }
}

/* ─── Main update entry point ──────────────────────────────────────────── */

/**
 * @brief Update UI based on current system state
 * @note Called periodically with LVGL semaphore held.
 *       Evaluates display state once, then dispatches to per-widget renders.
 */
static void update_ui_from_system_state(void)
{
    apply_background_colors_from_config();

    /* ── Firmware update: early-return path ──────────────────────────────── */
    uint32_t fw_bytes_received = 0u;
    uint32_t fw_expected_size  = 0u;
    uint8_t  cch_fw_phase      = 0u;
    uint8_t  cch_fw_percent    = 0u;
    uint32_t cch_fw_completed  = 0u;
    uint32_t cch_fw_total      = 0u;
    bool cch_fw_active = RS485_Command_Adapter_GetFirmwareProgress(
                             &cch_fw_phase, &cch_fw_percent,
                             &cch_fw_completed, &cch_fw_total);

    if (RS485_FW_Update_GetProgress(&fw_bytes_received, &fw_expected_size)) {
        if (cch_fw_active && cch_fw_total > fw_expected_size && fw_expected_size > 0u) {
            uint32_t combined = (cch_fw_total - fw_expected_size) + fw_bytes_received;
            apply_firmware_update_percentage(
                (int32_t)(((uint64_t)combined * 100u) / cch_fw_total));
        } else {
            apply_firmware_update_ui(fw_bytes_received, fw_expected_size);
        }
        ui_ctx.force_refresh_on_next_update = false;
        return;
    }
    if (cch_fw_active) {
        apply_firmware_update_percentage((int32_t)cch_fw_percent);
        ui_ctx.force_refresh_on_next_update = false;
        return;
    }

    /* ── Module failure check ─────────────────────────────────────────────── */
    const SystemConfig_t *cfg = Config_Get();
    ui_ctx.failed_module_count = 0u;
    if (cfg->modules.lcd_display_enabled    && System_GetModuleState(MODULE_LCD_DISPLAY)    != MODULE_STATE_RUNNING) ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_LCD_DISPLAY;
    if (cfg->modules.mifare_polling_enabled && System_GetModuleState(MODULE_MIFARE_POLLING) != MODULE_STATE_RUNNING) ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_MIFARE_POLLING;
    if (cfg->modules.dispenser_enabled      && System_GetModuleState(MODULE_DISPENSER)      != MODULE_STATE_RUNNING) ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_DISPENSER;
    if (cfg->modules.buzzer_enabled         && System_GetModuleState(MODULE_BUZZER)         != MODULE_STATE_RUNNING) ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_BUZZER;
    if (cfg->modules.io_expander_enabled    && System_GetModuleState(MODULE_IO_EXPANDER)    != MODULE_STATE_RUNNING) ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_IO_EXPANDER;
    if (cfg->modules.rs485_enabled          && System_GetModuleState(MODULE_RS485)          != MODULE_STATE_RUNNING) ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_RS485;
    ui_ctx.has_module_failures = (ui_ctx.failed_module_count > 0u);

    /* ── Derive inputs from event cache ──────────────────────────────────── */
    uint32_t current_time             = (uint32_t)xTaskGetTickCount();
    MIFARE_CardState_t      cst       = ui_ctx.card_state;
    MIFARE_TransactionState_t txn     = ui_ctx.transaction_state;
    uint32_t card_balance_ml          = ui_ctx.card_balance_ml;
    bool admin_card_present           = ui_ctx.admin_card_present;
    bool is_dispensing                = ui_ctx.dispense_active;
    bool card_present_now             = ui_ctx.rfid_card_present ||
                                         (cst == MIFARE_CARD_STATE_PRESENT ||
                                         cst == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE);
    bool card_authenticated           = (cst == MIFARE_CARD_STATE_PRESENT &&
                                         (card_balance_ml > 0u || admin_card_present));

    /* Cache card balance while card is present */
    if (cst == MIFARE_CARD_STATE_PRESENT && card_balance_ml > 0u) {
        ui_ctx.last_card_balance_ml = card_balance_ml;
    }

    /* ── Manual no-flow banner timeout ───────────────────────────────────── */
    if (ui_ctx.manual_no_flow_display_active) {
        if (pdTICKS_TO_MS(current_time - ui_ctx.manual_no_flow_display_start_time)
                >= MANUAL_NO_FLOW_DISPLAY_MS) {
            ui_ctx.manual_no_flow_display_active     = false;
            ui_ctx.manual_no_flow_display_start_time = 0u;
            ui_ctx.force_refresh_on_next_update      = true;
        }
    }
    bool manual_no_flow = ui_ctx.manual_no_flow_display_active;
    bool card_no_flow   = (txn == TRANSACTION_STATE_ERROR_NO_FLOW);

    /* ── Cooldown tracking ────────────────────────────────────────────────── */
    if (ui_ctx.was_dispensing && !is_dispensing) {
        ui_ctx.dispense_stopped_time = current_time;
        ui_ctx.in_cooldown           = true;
        ui_ctx.dispensed_amount_ml   = ui_ctx.dispense_amount_ml;
    }
    ui_ctx.was_dispensing = is_dispensing;
    if (ui_ctx.in_cooldown && !is_dispensing) {
        if (pdTICKS_TO_MS(current_time - ui_ctx.dispense_stopped_time) >= DISPENSE_COOLDOWN_MS) {
            ui_ctx.in_cooldown = false;
        }
    }

    /* ── Card presence edge detection ────────────────────────────────────── */
    if (!ui_ctx.was_card_present && card_present_now) {
        /* Card inserted — cancel any lingering transient states */
        ui_ctx.in_cooldown                       = false;
        ui_ctx.showing_persisted_data            = false;
        ui_ctx.manual_no_flow_display_active     = false;
        ui_ctx.manual_no_flow_display_start_time = 0u;
        ui_ctx.force_refresh_on_next_update      = true;
    }
    if (ui_ctx.was_card_present && !card_present_now) {
        ui_ctx.card_removed_time      = current_time;
        ui_ctx.showing_persisted_data = true;
    }
    ui_ctx.was_card_present = card_present_now;

    /* Persistence timeout — reset to idle when expired */
    if (ui_ctx.showing_persisted_data && !card_present_now) {
        if (pdTICKS_TO_MS(current_time - ui_ctx.card_removed_time)
                >= cfg->ui.ui_hide_delay_ms) {
            reset_ui_to_idle();
            apply_ui_state_config(UI_STATE_IDLE);
            ui_ctx.force_refresh_on_next_update = false;
            return;
        }
    }

    /* Legacy tracking variable */
    card_present = card_present_now ? 1 : 0;

    /* Pending card command (e.g. CLI write) waiting for tap */
    CLI_PendingCommandState_t *pending_cmd = CLI_GetPendingCommand();
    bool card_waiting_tap = (pending_cmd != NULL && pending_cmd->active &&
                             cst == MIFARE_CARD_STATE_ABSENT);

    /* display_balance_ml: use cached value in states where live balance may be 0 */
    uint32_t display_balance_ml = card_balance_ml;
    if (card_balance_ml == 0u && ui_ctx.last_card_balance_ml > 0u) {
        if (card_no_flow || txn == TRANSACTION_STATE_WAITING_REMOVAL ||
            ui_ctx.in_cooldown || is_dispensing || ui_ctx.showing_persisted_data) {
            display_balance_ml = ui_ctx.last_card_balance_ml;
        }
    }

    /* ── Evaluate display state ───────────────────────────────────────────── */
    UI_Disp_State_t disp_state = ui_disp_determine_state(
        ui_ctx.self_clean_active,
        is_dispensing, card_present_now,
        manual_no_flow, admin_card_present,
        cst, txn,
        card_no_flow, card_authenticated, card_balance_ml,
        ui_ctx.in_cooldown, ui_ctx.showing_persisted_data);

    static UI_Disp_State_t last_disp_state = UI_DS_COUNT;
    if (disp_state != last_disp_state || ui_ctx.force_refresh_on_next_update) {
        /* Invalidate per-widget caches so renders re-draw immediately */
        last_dispense_timer_seconds     = 0xFFFFFFFFu;
        last_dispense_token_count       = 0xFFFFFFFFu;
        last_total_remaining_percentage = -1;
        last_valve_state                = (ValveState_t)0xFFu;
        last_disp_state                 = disp_state;
        LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: display state → %d\r\n", (int)disp_state);
    }

    /* Legacy UI_State_t drives customerID visibility via apply_ui_state_config */
    UI_State_t lvgl_state = map_card_state_to_ui_state(cst, is_dispensing);
    if (lvgl_state != last_applied_ui_state || ui_ctx.force_refresh_on_next_update) {
        apply_ui_state_config(lvgl_state);
        last_applied_ui_state = lvgl_state;
    }

    /* ── Widget renders ───────────────────────────────────────────────────── */
    render_customer_id(disp_state, cfg->ui.no_card_customer_id);
    render_card_error_status(disp_state, card_waiting_tap, ui_ctx.operation_context);
    render_card_remaining(disp_state, display_balance_ml);
    render_total_remaining_bar(disp_state, card_balance_ml, ui_ctx.last_topup_ml);
    render_level_colour(disp_state, display_balance_ml);
    render_led_indicator(disp_state);

    ui_ctx.force_refresh_on_next_update = false;
}
