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
#include "ui_Screen1.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "System_Events.h"
#include "Hardware_Access.h" /* For SPI_MSG_DEF and centralized hardware definitions */
#include "USB_Logging.h"
#include "MIFARE_Transaction_Core.h"  /* For getter functions */
#include "Dispenser_Controller.h"        /* For dispenser functions */
#include "System_Config.h"                /* For SD card configuration */
#include "USB_Command_Handler.h"
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
    
    /* Force UI refresh on next update (e.g., when new card inserted) */
    bool force_refresh_on_next_update;
} UI_Display_Context_t;

/* Timing Constants */
#define ONE_SECOND_MS                   1000U
#define FPS_UPDATE_INTERVAL_MS          ONE_SECOND_MS
#define LCD_RESET_DELAY_MS              500U
#define DISPENSE_COOLDOWN_MS            5000U  /* 5 second cooldown after dispense stops */

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
    .last_card_balance_ml = 0
};

/* Performance Optimization - Cache previous state to avoid redundant updates */
static UI_State_t last_applied_ui_state = UI_STATE_COUNT;  /* Invalid state forces first update */
static ValveState_t last_valve_state = (ValveState_t)0xFF;  /* Invalid value forces first update */
static uint32_t last_dispense_timer_seconds = 0xFFFFFFFF;  /* Cache for dispenser timer */
static uint32_t last_dispense_token_count = 0xFFFFFFFF;  /* Cache for token count display */
static char last_customer_id[32] = "";  /* Cache for customer ID */

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
    
    while (lcd_get_lines_rendered() < (uint32_t)display_height) {
        lv_timer_handler();
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
    ui_ctx.showing_persisted_data = false;
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
    
    if (ui_Screen1 != NULL) {
        /* Apply main background color */
        lv_obj_set_style_bg_color(ui_Screen1, lv_color_hex(config->ui.bg_color), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_Screen1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        /* Apply gradient color */
        lv_obj_set_style_bg_grad_color(ui_Screen1, lv_color_hex(config->ui.bg_grad_color), LV_PART_MAIN | LV_STATE_DEFAULT);
        
        /* Apply gradient stops */
        lv_obj_set_style_bg_main_stop(ui_Screen1, config->ui.bg_main_stop, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_grad_stop(ui_Screen1, config->ui.bg_grad_stop, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        /* Apply gradient direction (vertical) */
        lv_obj_set_style_bg_grad_dir(ui_Screen1, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
        
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

/**
 * @brief Update UI based on current system state
 * @note Called periodically with LVGL semaphore held. 
 *       UI reacts to system state - it does not drive it.
 */
static void update_ui_from_system_state(void)
{
    /* ===== Check for live config changes (background colors, etc.) ===== */
    apply_background_colors_from_config();
    
    /* ===== Check for module startup failures ===== */
    const SystemConfig_t* cfg = Config_Get();
    ui_ctx.failed_module_count = 0;
    
    /* Check each enabled module's state - flag as failed if enabled but not running */
    if (cfg->modules.lcd_display_enabled && System_GetModuleState(MODULE_LCD_DISPLAY) != MODULE_STATE_RUNNING) {
        ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_LCD_DISPLAY;
    }
    if (cfg->modules.mifare_polling_enabled && System_GetModuleState(MODULE_MIFARE_POLLING) != MODULE_STATE_RUNNING) {
        ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_MIFARE_POLLING;
    }
    if (cfg->modules.dispenser_enabled && System_GetModuleState(MODULE_DISPENSER) != MODULE_STATE_RUNNING) {
        ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_DISPENSER;
    }
    if (cfg->modules.buzzer_enabled && System_GetModuleState(MODULE_BUZZER) != MODULE_STATE_RUNNING) {
        ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_BUZZER;
    }
    if (cfg->modules.io_expander_enabled && System_GetModuleState(MODULE_IO_EXPANDER) != MODULE_STATE_RUNNING) {
        ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_IO_EXPANDER;
    }
    if (cfg->modules.rs485_enabled && System_GetModuleState(MODULE_RS485) != MODULE_STATE_RUNNING) {
        ui_ctx.failed_modules[ui_ctx.failed_module_count++] = MODULE_RS485;
    }
    
    ui_ctx.has_module_failures = (ui_ctx.failed_module_count > 0);
    
    /* ===== Poll current system state from modules ===== */
    uint32_t card_balance_tokens = MIFARE_GetBalance();
    MIFARE_CardState_t card_state = MIFARE_GetCardState();
    MIFARE_TransactionState_t txn_state = MIFARE_GetTransactionState();
    bool admin_card_present = MIFARE_IsAdminCard();
    USB_PendingCommandState_t* pending_card_command = USB_Command_GetPendingCommand();
    bool card_process_waiting_for_tap = (pending_card_command != NULL &&
                                         pending_card_command->active &&
                                         card_state == MIFARE_CARD_STATE_ABSENT);
    bool card_present_now = (card_state == MIFARE_CARD_STATE_PRESENT || 
                             card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE);
    bool card_authenticated = (card_state == MIFARE_CARD_STATE_PRESENT && (card_balance_tokens > 0 || admin_card_present));
    
    /* Cache balance when card is ready (before error states can clear it) */
    if (card_state == MIFARE_CARD_STATE_PRESENT && card_balance_tokens > 0) {
        ui_ctx.last_card_balance_ml = card_balance_tokens; // Assuming tokens are equivalent to ml for display purposes
    }
    bool is_dispensing = Dispenser_IsDispenseActive();
    uint32_t current_time = xTaskGetTickCount();
    
    /* Update legacy variable */
    card_present = card_present_now ? 1 : 0;
    
    /* DEBUG LOGGING */
    static uint32_t last_debug_log_time = 0;
    if ((current_time - last_debug_log_time) > 1000) {
        last_debug_log_time = current_time;
        if (is_dispensing) {
            USB_Log_Printf("UI_DEBUG: Dispensing=1, Card=%d, RemLabel=%p\r\n", 
                          card_present_now, ui_cardRemaining);
            if (ui_cardRemaining) {
                 USB_Log_Printf("UI_DEBUG: Label Hidden=%d\r\n", 
                               lv_obj_has_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN));
            }
        }
    }
    
    /* ===== Track dispense stop for cooldown period ===== */
    if (ui_ctx.was_dispensing && !is_dispensing) {
        /* Dispense just stopped - start cooldown period */
        ui_ctx.dispense_stopped_time = current_time;
        ui_ctx.in_cooldown = true;
        ui_ctx.dispensed_amount_ml = Dispenser_GetDispensedAmountML();
    }
    ui_ctx.was_dispensing = is_dispensing;
    
    /* ===== Check if cooldown period expired ===== */
    if (ui_ctx.in_cooldown && !is_dispensing) {
        uint32_t time_since_stop = pdTICKS_TO_MS(current_time - ui_ctx.dispense_stopped_time);
        if (time_since_stop >= DISPENSE_COOLDOWN_MS) {
            ui_ctx.in_cooldown = false;
        }
    }
    
    /* ===== Detect card insertion edge - overrides cooldown ===== */
    if (!ui_ctx.was_card_present && card_present_now) {
        /* Card just inserted - cancel cooldown and persistence immediately */
        ui_ctx.in_cooldown = false;
        ui_ctx.showing_persisted_data = false;
        ui_ctx.force_refresh_on_next_update = true;  /* Force all cached values to refresh */
    }
    
    /* ===== Detect card removal edge ===== */
    if (ui_ctx.was_card_present && !card_present_now) {
        /* Card just removed - start persistence timer */
        ui_ctx.card_removed_time = current_time;
        ui_ctx.showing_persisted_data = true;
    }
    ui_ctx.was_card_present = card_present_now;
    
    /* ===== Check if persistence timeout expired ===== */
    if (ui_ctx.showing_persisted_data && !card_present_now) {
        uint32_t ui_hide_delay = Config_Get()->ui.ui_hide_delay_ms;
        if ((current_time - ui_ctx.card_removed_time) >= ui_hide_delay) {
            /* Timeout expired - reset to idle */
            reset_ui_to_idle();
            apply_ui_state_config(UI_STATE_IDLE);
            return;
        }
    }
    
    /* ===== Determine current UI state ===== */
    UI_State_t current_ui_state = map_card_state_to_ui_state(card_state, is_dispensing);
    
    /* ===== Apply state-specific UI configuration (only if changed) ===== */
    /* Force refresh on new card insertion OR if state changed */
    if (current_ui_state != last_applied_ui_state || ui_ctx.force_refresh_on_next_update) {
        apply_ui_state_config(current_ui_state);
        last_applied_ui_state = current_ui_state;
        /* Force valve state colors to update when UI state changes */
        last_valve_state = (ValveState_t)0xFF;  /* Invalid value triggers update */
    }
    
    /* ===== Update customer ID (only when changed) ===== */
    const SystemConfig_t* config = Config_Get();
    if (ui_customerID != NULL) {
        const char* new_customer_id = NULL;
        
        if (admin_card_present) {
            new_customer_id = "Admin";
        } else if (card_state == MIFARE_CARD_STATE_PRESENT) {
            /* Card present - get and cache phone number */
            char phone_str[12];
            if (MIFARE_GetCustomerPhoneNumber(phone_str, sizeof(phone_str))) {
                strncpy(ui_ctx.last_phone_number, phone_str, sizeof(ui_ctx.last_phone_number) - 1);
                ui_ctx.last_phone_number[sizeof(ui_ctx.last_phone_number) - 1] = '\0';
                new_customer_id = ui_ctx.last_phone_number;
            }
        } else if (!ui_ctx.showing_persisted_data) {
            /* No card and not persisting */
            if (is_dispensing) {
                /* Manual dispense active - show "Manual" */
                new_customer_id = "Manual";
                /* Force visibility for manual mode */
                if (lv_obj_has_flag(ui_customerID, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_clear_flag(ui_customerID, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                /* Show configured default */
                new_customer_id = config->ui.no_card_customer_id;
            }
        }
        
        /* Only update label if text changed */
        if (new_customer_id != NULL && strcmp(last_customer_id, new_customer_id) != 0) {
            strncpy(last_customer_id, new_customer_id, sizeof(last_customer_id) - 1);
            last_customer_id[sizeof(last_customer_id) - 1] = '\0';
            lv_label_set_text(ui_customerID, last_customer_id);
        }
        /* When persisting after card removal, label keeps cached value */
    }
    
    /* ===== Determine error status first (needed for dispensedSession visibility) ===== */
    const char* error_text = NULL;
    bool should_show_error = false;
    
    /* Check all error conditions - prioritize specific errors over generic balance check */
    if (txn_state == TRANSACTION_STATE_ERROR_NO_FLOW) {
        /* No flow error - show specific message */
        error_text = "No Flow";
        should_show_error = true;
    } else if (card_state == MIFARE_CARD_STATE_INITIALIZING ||
               card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE ||
               txn_state == TRANSACTION_STATE_CARD_DETECTED ||
               txn_state == TRANSACTION_STATE_AUTHENTICATING ||
               txn_state == TRANSACTION_STATE_READING_DATA ||
               txn_state == TRANSACTION_STATE_VALIDATING) {
        /* Card operation in progress - show working status */
        error_text = "In Progress";
        should_show_error = true;
    } else if (txn_state == TRANSACTION_STATE_WAITING_REMOVAL) {
        /* Operation complete - show appropriate status based on context */
        MIFARE_OperationContext_t ctx = MIFARE_GetOperationContext();
        if (ctx == MIFARE_CONTEXT_INIT) {
            error_text = "Complete";
        } else {
            error_text = "Topped Up";
        }
        should_show_error = true;
    } else if (card_state == MIFARE_CARD_STATE_ABSENT && !ui_ctx.in_cooldown) {
        /* No card present (only show after cooldown expires) */
        error_text = card_process_waiting_for_tap ? "Tap Card" : "No Card";
        should_show_error = true;
    } else if (card_state == MIFARE_CARD_STATE_PRESENT && 
               txn_state == TRANSACTION_STATE_WAITING_REMOVAL) {
        /* Operation complete - keep showing balance but display status message */
        should_show_error = true;
    } else if (card_state == MIFARE_CARD_STATE_ERROR) {
        /* Card failed to initialize */
        should_show_error = true;
    } else if (admin_card_present) {
        should_show_error = false;
    } else if (card_state == MIFARE_CARD_STATE_PRESENT && card_balance_tokens == 0 && 
               txn_state != TRANSACTION_STATE_ERROR_NO_FLOW) {
        /* Card authenticated but no balance (not a flow error) */
        should_show_error = true;
    } else if (card_authenticated) {
        /* Hide when card authenticated with balance */
        should_show_error = false;
    } else if (ui_ctx.in_cooldown) {
        /* Show error after cooldown expires */
        should_show_error = true;
    }
    
    /* ===== Update cardErrorStatus label ===== */
    if (ui_cardErrorStatus != NULL) {
        static uint32_t last_dispensed_error_liters_x10 = 0xFFFFFFFF;
        static char last_error_text[16] = "";  // Moved here to allow reset
        
        /* Reset cache when transitioning out of dispense/cooldown to force update */
        static bool was_in_dispense_or_cooldown = false;
        bool is_in_dispense_or_cooldown = (is_dispensing || ui_ctx.in_cooldown);
        
        /* Force refresh on new card insertion OR when exiting dispense/cooldown */
        if (ui_ctx.force_refresh_on_next_update || 
            (was_in_dispense_or_cooldown && !is_in_dispense_or_cooldown)) {
            /* Reset all caches to force immediate update */
            last_dispensed_error_liters_x10 = 0xFFFFFFFF;
            last_error_text[0] = '\0';  // Clear error text cache
        }
        was_in_dispense_or_cooldown = is_in_dispense_or_cooldown;
        
        /* Check if a new card is being processed (takes priority over cooldown display) */
        bool card_processing_in_progress = (
            txn_state == TRANSACTION_STATE_CARD_DETECTED ||
            txn_state == TRANSACTION_STATE_AUTHENTICATING ||
            txn_state == TRANSACTION_STATE_READING_DATA ||
            txn_state == TRANSACTION_STATE_VALIDATING ||
            card_state == MIFARE_CARD_STATE_INITIALIZING ||
            card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE
        );
        
        /* Get current dispensed amount for display logic */
        uint32_t current_dispensed_ml = is_dispensing ? Dispenser_GetDispensedAmountML() : ui_ctx.dispensed_amount_ml;
        bool has_positive_flow = (current_dispensed_ml > 0);
        
        /* Dispensing/cooldown shows dispensed amount - ONLY when there's positive flow */
        /* If no water was dispensed, skip to normal error handling (shows "No Flow" or "Ready") */
        if (((is_dispensing && has_positive_flow) || (ui_ctx.in_cooldown && has_positive_flow)) && !card_processing_in_progress) {
            /* Show dispensed amount on cardErrorStatus (only when flow is positive) */
            static char dispensed_error_str[16];
            
            uint32_t dispensed_ml = is_dispensing ? Dispenser_GetDispensedAmountML() : ui_ctx.dispensed_amount_ml;
            uint32_t dispensed_liters_x10 = dispensed_ml / 100;  /* Convert ml to 0.1L units */
            
            /* Only update if value changed */
            if (dispensed_liters_x10 != last_dispensed_error_liters_x10) {
                last_dispensed_error_liters_x10 = dispensed_liters_x10;
                
                if (dispensed_liters_x10 < 100) {
                    /* Under 10L - show decimal */
                    snprintf(dispensed_error_str, sizeof(dispensed_error_str), "%lu.%lu L", 
                             dispensed_liters_x10 / 10, dispensed_liters_x10 % 10);
                } else {
                    /* 10L or more - show whole number only */
                    snprintf(dispensed_error_str, sizeof(dispensed_error_str), "%lu L", 
                             dispensed_liters_x10 / 10);
                }
                lv_label_set_text(ui_cardErrorStatus, dispensed_error_str);
            }
            
            /* Make visible */
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (card_processing_in_progress) {
            /* New card being processed - show "In Progress" */
            if (strcmp(last_error_text, "In Progress") != 0) {
                lv_label_set_text(ui_cardErrorStatus, "In Progress");
                strncpy(last_error_text, "In Progress", sizeof(last_error_text) - 1);
                last_error_text[sizeof(last_error_text) - 1] = '\0';
            }
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (txn_state == TRANSACTION_STATE_WAITING_REMOVAL) {
            /* Operation complete - alternate between status and "Remove Card" based on context */
            static uint32_t last_toggle_time_waiting = 0;
            static bool show_remove_waiting = false;
            uint32_t now = xTaskGetTickCount();
            
            if ((now - last_toggle_time_waiting) >= pdMS_TO_TICKS(2000)) {
                show_remove_waiting = !show_remove_waiting;
                last_toggle_time_waiting = now;
            }
            
            MIFARE_OperationContext_t ctx = MIFARE_GetOperationContext();
            const char *status_msg = (ctx == MIFARE_CONTEXT_INIT) ? "Complete" : "Topped Up";
            const char *msg = show_remove_waiting ? "Remove Card" : status_msg;
            if (strcmp(last_error_text, msg) != 0) {
                lv_label_set_text(ui_cardErrorStatus, msg);
                strncpy(last_error_text, msg, sizeof(last_error_text) - 1);
                last_error_text[sizeof(last_error_text) - 1] = '\0';
            }
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (admin_card_present && !is_dispensing) {
            /* Admin card ready - no dispensing balance shown */
            if (strcmp(last_error_text, "Admin") != 0) {
                lv_label_set_text(ui_cardErrorStatus, "Admin");
                strncpy(last_error_text, "Admin", sizeof(last_error_text) - 1);
                last_error_text[sizeof(last_error_text) - 1] = '\0';
            }
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (card_authenticated && card_balance_tokens > 0 && !is_dispensing) {
            /* Card ready with balance, not yet dispensing - show "Ready" */
            if (strcmp(last_error_text, "Ready") != 0) {
                lv_label_set_text(ui_cardErrorStatus, "Ready");
                strncpy(last_error_text, "Ready", sizeof(last_error_text) - 1);
                last_error_text[sizeof(last_error_text) - 1] = '\0';
            }
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (ui_ctx.has_module_failures) {
            /* Module failures override normal error displays */
            /* Cycle through failed module names every 2 seconds */
            uint32_t now = xTaskGetTickCount();
            if ((now - ui_ctx.last_failed_module_cycle_time) >= pdMS_TO_TICKS(2000)) {
                ui_ctx.current_failed_module_index++;
                if (ui_ctx.current_failed_module_index >= ui_ctx.failed_module_count) {
                    ui_ctx.current_failed_module_index = 0;
                }
                ui_ctx.last_failed_module_cycle_time = now;
            }
            
            /* Build error text: just module name */
            char module_error_text[32];
            System_Module_t failed_module = ui_ctx.failed_modules[ui_ctx.current_failed_module_index];
            const char* module_name = System_GetModuleName(failed_module);
            snprintf(module_error_text, sizeof(module_error_text), "%s", module_name);
            
            /* Always update and show module failure errors */
            lv_label_set_text(ui_cardErrorStatus, module_error_text);
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            /* Normal error handling (when no module failures and not dispensing) */
            bool should_show = should_show_error;  /* Use pre-calculated error state */
            
            /* Check for "In Progress" states first - takes priority */
            static bool logged_in_progress = false;
            if (txn_state == TRANSACTION_STATE_CARD_DETECTED ||
                txn_state == TRANSACTION_STATE_AUTHENTICATING ||
                txn_state == TRANSACTION_STATE_READING_DATA ||
                txn_state == TRANSACTION_STATE_VALIDATING ||
                card_state == MIFARE_CARD_STATE_INITIALIZING ||
                card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
                error_text = "In Progress";
                should_show = true;
                /* Debug: Log when In Progress is detected (once per card) */
                if (!logged_in_progress) {
                    USB_Log_Printf("[UI] In Progress detected: txn=%d, card=%d\r\n", txn_state, card_state);
                    logged_in_progress = true;
                }
            } else {
                /* Reset flag when NOT in progress so next card logs again */
                logged_in_progress = false;
            }
            
            if (txn_state == TRANSACTION_STATE_ERROR_NO_FLOW) {
                /* No flow error - alternate between "No Flow" and "Remove Card" */
                static uint32_t last_toggle_time_no_flow = 0;
                static bool show_remove_message_no_flow = false;
                uint32_t now = xTaskGetTickCount();
                
                if ((now - last_toggle_time_no_flow) >= pdMS_TO_TICKS(2000)) {
                    show_remove_message_no_flow = !show_remove_message_no_flow;
                    last_toggle_time_no_flow = now;
                }
                
                error_text = show_remove_message_no_flow ? "Remove Card" : "No Flow";
                should_show = true;
            } else if (txn_state == TRANSACTION_STATE_WAITING_REMOVAL) {
                /* Operation complete - alternate between status and "Remove Card" based on context */
                static uint32_t last_toggle_time_waiting_lvl = 0;
                static bool show_remove_message_waiting_lvl = false;
                uint32_t now = xTaskGetTickCount();
                
                if ((now - last_toggle_time_waiting_lvl) >= pdMS_TO_TICKS(2000)) {
                    show_remove_message_waiting_lvl = !show_remove_message_waiting_lvl;
                    last_toggle_time_waiting_lvl = now;
                }
                
                MIFARE_OperationContext_t ctx = MIFARE_GetOperationContext();
                const char *status_msg = (ctx == MIFARE_CONTEXT_INIT) ? "Complete" : "Topped Up";
                error_text = show_remove_message_waiting_lvl ? "Remove Card" : status_msg;
                should_show = true;
            } else if (card_state == MIFARE_CARD_STATE_ABSENT && !ui_ctx.in_cooldown) {
                /* No card present (only show after cooldown expires) */
                error_text = card_process_waiting_for_tap ? "Tap Card" : "No Card";
                should_show = true;
            } else if (card_state == MIFARE_CARD_STATE_PRESENT && 
                       txn_state == TRANSACTION_STATE_WAITING_REMOVAL) {
            /* Operation complete - alternate between status and "Remove Card" based on context */
            static uint32_t last_toggle_time_flow = 0;
            static bool show_remove_message_flow = false;
            uint32_t now = xTaskGetTickCount();
            
            /* Toggle message every 2 seconds */
            if ((now - last_toggle_time_flow) >= pdMS_TO_TICKS(2000)) {
                show_remove_message_flow = !show_remove_message_flow;
                last_toggle_time_flow = now;
            }
            
            MIFARE_OperationContext_t ctx = MIFARE_GetOperationContext();
            const char *status_msg = (ctx == MIFARE_CONTEXT_INIT) ? "Complete" : "Topped Up";
            error_text = show_remove_message_flow ? "Remove Card" : status_msg;
            should_show = true;
        } else if (card_state == MIFARE_CARD_STATE_ERROR) {
            /* Card failed to initialize - alternate between "No Init" and "Remove Card" every 2 seconds */
            static uint32_t last_toggle_time = 0;
            static bool show_remove_message = false;
            uint32_t now = xTaskGetTickCount();
            
            /* Toggle message every 2 seconds */
            if ((now - last_toggle_time) >= pdMS_TO_TICKS(2000)) {
                show_remove_message = !show_remove_message;
                last_toggle_time = now;
            }
            
            error_text = show_remove_message ? "Remove Card" : "No Init";
            should_show = true;
        } else if (admin_card_present) {
            error_text = "Admin";
            should_show = true;
        } else if (card_state == MIFARE_CARD_STATE_PRESENT && card_balance_tokens == 0) {
            /* Card authenticated but no balance - alternate between "No Balance" and "Remove Card" every 2 seconds */
            static uint32_t last_toggle_time_balance = 0;
            static bool show_remove_message_balance = false;
            uint32_t now = xTaskGetTickCount();
            
            /* Toggle message every 2 seconds */
            if ((now - last_toggle_time_balance) >= pdMS_TO_TICKS(2000)) {
                show_remove_message_balance = !show_remove_message_balance;
                last_toggle_time_balance = now;
            }
            
            error_text = show_remove_message_balance ? "Remove Card" : "No Balance";
            should_show = true;
        } else if (card_authenticated) {
            /* Card authenticated with balance - show "Ready" */
            error_text = "Ready";
            should_show = true;
        } else if (ui_ctx.in_cooldown) {
            /* Show error after cooldown expires */
            should_show = true;
            /* Keep whatever error state was active */
            if (txn_state == TRANSACTION_STATE_ERROR_NO_FLOW) {
                /* No flow error during cooldown - alternate messages */
                static uint32_t last_toggle_time_no_flow_cooldown = 0;
                static bool show_remove_message_no_flow_cooldown = false;
                uint32_t now = xTaskGetTickCount();
                
                if ((now - last_toggle_time_no_flow_cooldown) >= pdMS_TO_TICKS(2000)) {
                    show_remove_message_no_flow_cooldown = !show_remove_message_no_flow_cooldown;
                    last_toggle_time_no_flow_cooldown = now;
                }
                
                error_text = show_remove_message_no_flow_cooldown ? "Remove Card" : "No Flow";
            } else if (txn_state == TRANSACTION_STATE_WAITING_REMOVAL) {
                /* Operation complete during cooldown - alternate based on context */
                static uint32_t last_toggle_time_waiting_cooldown = 0;
                static bool show_remove_message_waiting_cooldown = false;
                uint32_t now = xTaskGetTickCount();
                
                if ((now - last_toggle_time_waiting_cooldown) >= pdMS_TO_TICKS(2000)) {
                    show_remove_message_waiting_cooldown = !show_remove_message_waiting_cooldown;
                    last_toggle_time_waiting_cooldown = now;
                }
                
                MIFARE_OperationContext_t ctx = MIFARE_GetOperationContext();
                const char *status_msg = (ctx == MIFARE_CONTEXT_INIT) ? "Complete" : "Topped Up";
                error_text = show_remove_message_waiting_cooldown ? "Remove Card" : status_msg;
            } else if (card_state == MIFARE_CARD_STATE_ABSENT) {
                error_text = card_process_waiting_for_tap ? "Tap Card" : "No Card";
            } else if (card_state == MIFARE_CARD_STATE_ERROR) {
                /* Use same alternating pattern as above */
                static uint32_t last_toggle_time_cooldown = 0;
                static bool show_remove_message_cooldown = false;
                uint32_t now = xTaskGetTickCount();
                
                if ((now - last_toggle_time_cooldown) >= pdMS_TO_TICKS(2000)) {
                    show_remove_message_cooldown = !show_remove_message_cooldown;
                    last_toggle_time_cooldown = now;
                }
                
                error_text = show_remove_message_cooldown ? "Remove Card" : "No Init";
            } else if (admin_card_present) {
                error_text = "Admin";
            } else if (card_state == MIFARE_CARD_STATE_PRESENT && card_balance_tokens == 0) {
                /* Use same alternating pattern for no balance */
                static uint32_t last_toggle_time_balance_cooldown = 0;
                static bool show_remove_message_balance_cooldown = false;
                uint32_t now = xTaskGetTickCount();
                
                if ((now - last_toggle_time_balance_cooldown) >= pdMS_TO_TICKS(2000)) {
                    show_remove_message_balance_cooldown = !show_remove_message_balance_cooldown;
                    last_toggle_time_balance_cooldown = now;
                }
                
                error_text = show_remove_message_balance_cooldown ? "Remove Card" : "No Balance";
            }
        }
        
        /* Update label text if needed */
        if (should_show && error_text != NULL) {
            if (strcmp(last_error_text, error_text) != 0) {
                lv_label_set_text(ui_cardErrorStatus, error_text);
                strncpy(last_error_text, error_text, sizeof(last_error_text) - 1);
                last_error_text[sizeof(last_error_text) - 1] = '\0';
            }
            /* Make visible */
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            /* Hide error status */
            if (!lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        }
        }  /* End of else (normal error handling) */
    }  /* End of if (ui_cardErrorStatus != NULL) */
    
    /* ===== Update card remaining display (only when changed) ===== */
    /* Use cached balance if current balance is 0 due to error state, post-init/topup, or cooldown */
    uint32_t display_balance_ml = card_balance_tokens;
    if (card_balance_tokens == 0 && ui_ctx.last_card_balance_ml > 0) {
        /* Balance is 0 but we have cached value - use it for these states */
        if (txn_state == TRANSACTION_STATE_ERROR_NO_FLOW ||
            txn_state == TRANSACTION_STATE_WAITING_REMOVAL ||
            ui_ctx.in_cooldown ||
            is_dispensing ||
            ui_ctx.showing_persisted_data) {
            display_balance_ml = ui_ctx.last_card_balance_ml;
        }
    }
    
    /* Uses ui_cardRemaining label to show balance in liters */
    if (ui_cardRemaining != NULL) {
        bool dispense_active = is_dispensing;
        
        /* Track display mode changes to force refresh when switching formats */
        typedef enum {
            TIMER_MODE_NONE,
            TIMER_MODE_TOKEN_RATIO,  /* xx/yy format */
            TIMER_MODE_TIME,         /* MM:SS format */
            TIMER_MODE_EMPTY         /* --:-- format */
        } TimerDisplayMode_t;
        
        static TimerDisplayMode_t last_timer_mode = TIMER_MODE_NONE;
        TimerDisplayMode_t current_mode;
        
        /* Determine current display mode - always show value when card present or dispensing */
        if (dispense_active || card_present_now) {
            current_mode = TIMER_MODE_TIME;
        } else {
            current_mode = TIMER_MODE_EMPTY;
        }
        
        /* Force refresh if mode changed OR manual dispense active (to ensure visibility) */
        if (current_mode != last_timer_mode || ui_ctx.force_refresh_on_next_update ||
            (is_dispensing && !card_present_now && last_timer_mode != TIMER_MODE_TIME)) {  /* Manual dispense start */
            last_dispense_timer_seconds = 0xFFFFFFFF;  /* Reset all caches */
            last_dispense_token_count = 0xFFFFFFFF;
            last_timer_mode = current_mode;
        }

        /* Update visibility to match levelColourIndicator logic */
        bool show_remaining = false;
        if ((card_authenticated && card_balance_tokens > 0) || dispense_active) {
            show_remaining = true;
        } else if (card_state == MIFARE_CARD_STATE_ABSENT && !ui_ctx.in_cooldown && !ui_ctx.showing_persisted_data) {
            show_remaining = false;
        } else {
            /* Keep previous visibility if it was already visible (e.g. during errors or persistence) */
            show_remaining = !lv_obj_has_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN);
        }

        if (show_remaining) {
            if (lv_obj_has_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (!lv_obj_has_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN);
            }
        }
        
        /* Format balance as single string for ui_cardRemaining label */
        static char balance_str[16];
        
        if (dispense_active && card_present_now) {
            /* Dispense active AND card present - show balance */
            if (display_balance_ml != last_dispense_timer_seconds) {
                last_dispense_timer_seconds = display_balance_ml;
                if (display_balance_ml < 1000) {
                    /* Under 1L - show in ml */
                    snprintf(balance_str, sizeof(balance_str), "%lu ml", display_balance_ml);
                } else if (display_balance_ml < 10000) {
                    /* 1L to under 10L - show decimal */
                    uint32_t liters_x10 = display_balance_ml / 100;
                    snprintf(balance_str, sizeof(balance_str), "%lu.%lu L", 
                             liters_x10 / 10, liters_x10 % 10);
                } else {
                    /* 10L or more - show whole number only */
                    snprintf(balance_str, sizeof(balance_str), "%lu L", 
                             display_balance_ml / 1000);
                }
                lv_label_set_text(ui_cardRemaining, balance_str);
            }
            
        } else if (dispense_active) {
            /* Dispense active without card - show remaining volume */
            uint32_t remaining_ml;
            
            /* Force visibility for manual dispense */
            if (lv_obj_has_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardRemaining, LV_OBJ_FLAG_HIDDEN);
                LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Manual dispense - Unhiding ui_cardRemaining\r\n");
            }
            
            /* If card was just removed during dispense (persisting), use cached balance */
            if (ui_ctx.showing_persisted_data && ui_ctx.last_card_balance_ml > 0) {
                remaining_ml = ui_ctx.last_card_balance_ml;
            } else {
                remaining_ml = Dispenser_GetDispenseVolumeRemainingMl();
            }
            
            if (remaining_ml != last_dispense_timer_seconds) {
                last_dispense_timer_seconds = remaining_ml;
                if (remaining_ml < 1000) {
                    /* Under 1L - show in ml */
                    snprintf(balance_str, sizeof(balance_str), "%lu ml", remaining_ml);
                } else if (remaining_ml < 10000) {
                    /* 1L to under 10L - show decimal */
                    uint32_t liters_x10 = remaining_ml / 100;
                    snprintf(balance_str, sizeof(balance_str), "%lu.%lu L", 
                             liters_x10 / 10, liters_x10 % 10);
                } else {
                    /* 10L or more - show whole number only */
                    snprintf(balance_str, sizeof(balance_str), "%lu L", 
                             remaining_ml / 1000);
                }
                LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Manual dispense - Updating text to '%s' (ml=%lu)\r\n", balance_str, remaining_ml);
                lv_label_set_text(ui_cardRemaining, balance_str);
            }
            
        } else if ((card_authenticated || 
                    txn_state == TRANSACTION_STATE_WAITING_REMOVAL || 
                    txn_state == TRANSACTION_STATE_ERROR_NO_FLOW ||
                    ui_ctx.in_cooldown ||
                    ui_ctx.showing_persisted_data) && display_balance_ml > 0) {
            /* Card authenticated (or in post-operation/error/cooldown/persisted states) - show balance
             * Keeps visible during no-flow errors to match levelColourIndicator */
            if (display_balance_ml != last_dispense_timer_seconds) {
                last_dispense_timer_seconds = display_balance_ml;
                last_dispense_token_count = 0xFFFFFFFF;
                if (display_balance_ml < 1000) {
                    /* Under 1L - show in ml */
                    snprintf(balance_str, sizeof(balance_str), "%lu ml", display_balance_ml);
                } else if (display_balance_ml < 10000) {
                    /* 1L to under 10L - show decimal */
                    uint32_t liters_x10 = display_balance_ml / 100;
                    snprintf(balance_str, sizeof(balance_str), "%lu.%lu L", 
                             liters_x10 / 10, liters_x10 % 10);
                } else {
                    /* 10L or more - show whole number only */
                    snprintf(balance_str, sizeof(balance_str), "%lu L", 
                             display_balance_ml / 1000);
                }
                lv_label_set_text(ui_cardRemaining, balance_str);
            }
        } else if (card_state == MIFARE_CARD_STATE_ABSENT && !ui_ctx.in_cooldown && !ui_ctx.showing_persisted_data) {
            /* No card AND not in cooldown AND not persisting - reset to default text (hidden anyway) */
            /* During cooldown/persistence, keep showing previous balance */
            if (last_dispense_timer_seconds != 0xFFFFFFFE) {
                last_dispense_timer_seconds = 0xFFFFFFFE;
                last_dispense_token_count = 0xFFFFFFFF;
                lv_label_set_text(ui_cardRemaining, "--.- L");
            }
        }
        /* During card processing (In Progress) or cooldown/persistence - don't update if condition above not met, keep previous value */
    }
    
    /* ===== Update totalRemainingBar (percentage based on balance/last_topup) ===== */
    if (ui_totalRemainingBar != NULL) {
        static int32_t last_bar_percentage = -1;
        int32_t bar_percentage = last_bar_percentage;  /* Keep last value by default */
        
        /* Force refresh on new card insertion */
        if (ui_ctx.force_refresh_on_next_update) {
            last_bar_percentage = -1;
        }
        
        if (card_present_now) {
            /* Card present - calculate percentage */
            uint32_t last_topup_tokens = MIFARE_GetLastTopup();
            
            if (last_topup_tokens > 0) {
                /* Calculate percentage: (balance / last_topup) * 100 */
                bar_percentage = (int32_t)((card_balance_tokens * 100) / last_topup_tokens);
                
                /* Clamp to 0-100 range */
                if (bar_percentage < 0) bar_percentage = 0;
                if (bar_percentage > 100) bar_percentage = 100;
            }
            /* If no last topup recorded but card present - keep current percentage */
        } else if (card_state == MIFARE_CARD_STATE_ABSENT && !ui_ctx.in_cooldown && !ui_ctx.showing_persisted_data) {
            /* No card AND not in cooldown AND not persisting - reset to 0% */
            /* During cooldown or persistence, keep last percentage */
            bar_percentage = 0;
        } else {
            /* Keep last_bar_percentage by default */
        }
        
        /* Update bar value if changed */
        if (bar_percentage != last_bar_percentage) {
            lv_bar_set_value(ui_totalRemainingBar, bar_percentage, LV_ANIM_OFF);
            last_bar_percentage = bar_percentage;
        }
    }
    
    /* ===== Update levelColourIndicator (balance level color) ===== */
    /* Color based on absolute balance remaining (not percentage):
     * < 15L (15000ml) = Red
     * < 20L (20000ml) = Amber
     * >= 20L = Green
     */
    if (ui_levelColourIndicator != NULL) {
        typedef enum {
            LEVEL_COLOR_NONE,
            LEVEL_COLOR_RED,
            LEVEL_COLOR_AMBER,
            LEVEL_COLOR_GREEN,
            LEVEL_COLOR_HIDDEN
        } LevelColor_t;
        
        static LevelColor_t last_level_color = LEVEL_COLOR_NONE;
        LevelColor_t current_level_color;
        
        if (ui_ctx.force_refresh_on_next_update) {
            last_level_color = LEVEL_COLOR_NONE;
        }
        
        /* Only show after card is fully authenticated (not during In Progress) */
        /* Keep visible during dispensing, no-flow errors, and cooldown */
        if ((card_authenticated && card_balance_tokens > 0) || is_dispensing) {
            /* Determine color by ml thresholds */
            /* During dispense without card, we use display_balance_ml (which might be 0 but we use cached value) */
            uint32_t current_val = (card_balance_tokens > 0) ? card_balance_tokens : ui_ctx.last_card_balance_ml;
            
            if (current_val < 15000) {
                /* Under 15L - Red (critical) */
                current_level_color = LEVEL_COLOR_RED;
            } else if (current_val < 20000) {
                /* 15L to under 20L - Amber (warning) */
                current_level_color = LEVEL_COLOR_AMBER;
            } else {
                /* 20L or more - Green (good) */
                current_level_color = LEVEL_COLOR_GREEN;
            }
        } else if (card_state == MIFARE_CARD_STATE_ABSENT && !ui_ctx.in_cooldown && !ui_ctx.showing_persisted_data) {
            /* No card AND not in cooldown AND not persisting - hide indicator */
            current_level_color = LEVEL_COLOR_HIDDEN;
        } else {
            /* Card processing or no balance - keep previous color if any (handles no-flow error / cooldown / persistence) */
            if (last_level_color != LEVEL_COLOR_NONE && last_level_color != LEVEL_COLOR_HIDDEN) {
                current_level_color = last_level_color;  /* Keep previous color */
            } else {
                current_level_color = LEVEL_COLOR_HIDDEN;
            }
        }
        
        /* Update only if color changed */
        if (current_level_color != last_level_color) {
            switch (current_level_color) {
                case LEVEL_COLOR_RED:
                    lv_obj_set_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xCC0000), LV_PART_MAIN | LV_STATE_DEFAULT);
                    if (lv_obj_has_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_clear_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                    break;
                    
                case LEVEL_COLOR_AMBER:
                    lv_obj_set_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFF9900), LV_PART_MAIN | LV_STATE_DEFAULT);
                    if (lv_obj_has_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_clear_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                    break;
                    
                case LEVEL_COLOR_GREEN:
                    lv_obj_set_style_bg_color(ui_levelColourIndicator, lv_color_hex(0x05820A), LV_PART_MAIN | LV_STATE_DEFAULT);
                    if (lv_obj_has_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_clear_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                    break;
                    
                case LEVEL_COLOR_HIDDEN:
                case LEVEL_COLOR_NONE:
                    if (!lv_obj_has_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(ui_levelColourIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                    break;
            }
            last_level_color = current_level_color;
        }
    }
    
    /* ===== Update ledIndicator (card state indicator) ===== */
    if (ui_ledIndicator != NULL) {
        typedef enum {
            LED_STATE_HIDDEN,
            LED_STATE_GREEN,
            LED_STATE_FLASH_RED
        } LED_State_t;
        
        static LED_State_t last_led_state = LED_STATE_HIDDEN;
        LED_State_t current_led_state;
        
        /* Force refresh on new card insertion */
        if (ui_ctx.force_refresh_on_next_update) {
            last_led_state = LED_STATE_HIDDEN;
        }
        
        /* Determine LED state based on card state and module failures */
        if (ui_ctx.has_module_failures) {
            /* Module failures - flash red to alert operator */
            current_led_state = LED_STATE_FLASH_RED;
        } else if (card_state == MIFARE_CARD_STATE_ABSENT) {
            /* No card - hide LED */
            current_led_state = LED_STATE_HIDDEN;
        } else if (card_state == MIFARE_CARD_STATE_PRESENT && card_authenticated) {
            /* Card validated with balance - solid green */
            current_led_state = LED_STATE_GREEN;
        } else if (card_state == MIFARE_CARD_STATE_INITIALIZING || 
                   card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE ||
                   card_state == MIFARE_CARD_STATE_ERROR ||
                   (card_state == MIFARE_CARD_STATE_PRESENT && card_balance_tokens == 0) ||
                   (card_state == MIFARE_CARD_STATE_PRESENT && txn_state == TRANSACTION_STATE_WAITING_REMOVAL)) {
            /* Card validating, error state, no balance, or no flow - flash red at 2Hz */
            current_led_state = LED_STATE_FLASH_RED;
        } else {
            /* Default - keep last state when card present, hide otherwise */
            current_led_state = card_present_now ? last_led_state : LED_STATE_HIDDEN;
        }
        
        /* Handle flashing state - 2Hz = 250ms on, 250ms off */
        if (current_led_state == LED_STATE_FLASH_RED) {
            const uint32_t flash_interval_ms = 250;  /* 2Hz = 250ms on/off */
            uint32_t now = xTaskGetTickCount();
            
            if ((now - ui_ctx.last_led_toggle_time) >= pdMS_TO_TICKS(flash_interval_ms)) {
                ui_ctx.led_is_on = !ui_ctx.led_is_on;
                ui_ctx.last_led_toggle_time = now;
                
                if (ui_ctx.led_is_on) {
                    lv_obj_set_style_bg_color(ui_ledIndicator, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
                    if (lv_obj_has_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_clear_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                } else {
                    /* Flash off - hide during off phase */
                    if (!lv_obj_has_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
        } else if (current_led_state != last_led_state) {
            /* State changed - apply new solid color or visibility */
            switch (current_led_state) {
                case LED_STATE_GREEN:
                    lv_obj_set_style_bg_color(ui_ledIndicator, lv_color_hex(0x00FF00), LV_PART_MAIN | LV_STATE_DEFAULT);
                    if (lv_obj_has_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_clear_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                    break;
                    
                case LED_STATE_HIDDEN:
                    if (!lv_obj_has_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(ui_ledIndicator, LV_OBJ_FLAG_HIDDEN);
                    }
                    break;
                    
                case LED_STATE_FLASH_RED:
                    /* Handled above */
                    break;
            }
        }
        
        last_led_state = current_led_state;
    }
    
    /* Clear force refresh flag after all updates complete */
    ui_ctx.force_refresh_on_next_update = false;

/* Note: Dispense option symbols (vacSymbol, pressWasherSymbol, washBrushSymbol) 
     * and colored buttons (redButton, blueButton, greedButton) removed - 
     * these elements don't exist in the current UI design */
}


