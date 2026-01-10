/*
 * BigYellow UI Driver - Optimized for STM32F411 with ILI9488
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
#include "System.h"
#include "Hardware_Access.h" /* For SPI_MSG_DEF and centralized hardware definitions */
#include "USB_Logging.h"
#include "MIFARE_Transaction_Manager.h"  /* For getter functions */
#include "Dispenser_Controller.h"        /* For dispenser functions */
#include "System_Config.h"                /* For SD card configuration */
#ifdef LV_USE_ILI9341
#include "display/ili9341/lv_ili9341.h"
#endif

#ifdef LV_USE_ILI9488
#include "display/ili9488/lv_ili9488.h"
#endif

#include "Task_Heartbeat.h"
#include "task_stack_config.h"

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
    
    /* Cached display values (shown during persistence timeout) */
    char last_phone_number[12];
    uint32_t last_card_balance_ml;
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

/* Task Handle */
static TaskHandle_t lcd_task_handle;

/* Performance Monitoring - removed unused frame_count variable */

/* UI State Variables */
static unsigned long elapsed_seconds = 0;            /* Elapsed seconds */
static uint8_t  screen_switched = 0;
static uint8_t  card_present = 0;
static unsigned long start_tick_ms = 0;              /* Start tick */
static unsigned long last_fps_update_ms = 0;         /* Last FPS tick */
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

/* Stack size provided by centralized task_stack_config.h */
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
static const char* get_card_error_status_text(MIFARE_CardState_t card_state, bool card_present);

/* ========================================================================== */
/*                         CORE TASK FUNCTION PROTOTYPES                     */
/* ========================================================================== */
static void lcd_display_task(void *argument);



/* lcd_test_init() and lcd_fill_test() declared in header */

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
static void lcd_display_task(void *argument)
{
    (void)argument;
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Starting LCD Display Driver initialization...\r\n");
    
    /* Initialize GPIO pins */
    lcd_gpio_init();
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: GPIO initialized\r\n");
    
    /* Initialize synchronization primitives */
    lvgl_mutex = xSemaphoreCreateMutex();
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
    lcd_backlight_on(0);
    
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
        lcd_backlight_on(brightness);
        vTaskDelay(pdMS_TO_TICKS(30)); /* 30ms per step = 600ms total fade time */
    }
    lcd_backlight_on(100); /* Ensure we end at exactly 100% */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Backlight at 100%%, initialization complete\r\n");
    
    LOG_CRITICAL_LCD_DISPLAY_DRIVER("[✓] LCD Display Task initialized successfully\r\n");
    
    /* MAIN TASK LOOP */
    TickType_t xLastWakeTime = xTaskGetTickCount();
    start_tick_ms = xTaskGetTickCount();
    last_fps_update_ms = start_tick_ms;
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

        /* Update LVGL tick counter - must be called regularly for LVGL timing */
        lv_tick_inc(LVGL_TASK_PERIOD_MS);

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
void Task_Start_LCD_Display_Driver_Task()
{
    if (lcd_task_handle != NULL) {
        LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Task already running\r\n");
        return;
    }
    (void)xTaskCreate(lcd_display_task, "LCD_Task", lcd_display_task_stack_size_words, NULL, LCD_DISPLAY_TASK_PRIORITY, &lcd_task_handle);
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

/**
 * @brief Fill screen with test colors to verify LCD hardware
 */
void lcd_fill_test(void)
{
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Testing screen fill...\r\n");
    
    // Create a simple test screen with red color
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), 0);  // Red
    lv_obj_invalidate(scr);
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Screen set to RED, calling lv_timer_handler()...\r\n");
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Changing to GREEN...\r\n");
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x00FF00), 0);  // Green
    lv_obj_invalidate(scr);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Changing to BLUE...\r\n");
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0000FF), 0);  // Blue
    lv_obj_invalidate(scr);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Fill test complete\r\n");
}

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
    
    /* Ensure cardErrorStatus label is on top of other UI elements */
    if (ui_cardErrorStatus != NULL) {
        lv_obj_move_foreground(ui_cardErrorStatus);
    }
    
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
    
    /* ===== Poll current system state from modules ===== */
    uint32_t card_balance_ml = MIFARE_GetBalanceMl();
    MIFARE_CardState_t card_state = MIFARE_GetCardState();
    bool card_present_now = (card_state == MIFARE_CARD_STATE_PRESENT || 
                             card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE);
    bool card_authenticated = (card_state == MIFARE_CARD_STATE_PRESENT && card_balance_ml > 0);
    bool is_dispensing = Dispenser_IsDispenseActive();
    uint32_t current_time = xTaskGetTickCount();
    
    /* Update legacy variable */
    card_present = card_present_now ? 1 : 0;
    
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
    
    /* ===== Update card error status label ===== */
    if (ui_cardErrorStatus != NULL) {
        static char error_status_text[32] = "";
        
        if (card_authenticated && !ui_ctx.in_cooldown) {
            /* Card authenticated correctly - hide error status */
            if (!lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            /* Show error status or dispensed amount during cooldown */
            if (ui_ctx.in_cooldown) {
                /* During cooldown - show dispensed amount */
                uint32_t dispensed_liters_x10 = ui_ctx.dispensed_amount_ml / 100;  /* Convert ml to 0.1L units */
                snprintf(error_status_text, sizeof(error_status_text), 
                         "Dispensed: %lu.%lu L", 
                         dispensed_liters_x10 / 10, dispensed_liters_x10 % 10);
            } else {
                /* Show card status */
                const char* status = get_card_error_status_text(card_state, card_present_now);
                strncpy(error_status_text, status, sizeof(error_status_text) - 1);
                error_status_text[sizeof(error_status_text) - 1] = '\0';
            }
            
            lv_label_set_text(ui_cardErrorStatus, error_status_text);
            
            /* Make sure it's visible */
            if (lv_obj_has_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_cardErrorStatus, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    
    /* ===== Determine current UI state ===== */
    UI_State_t current_ui_state = map_card_state_to_ui_state(card_state, is_dispensing);
    
    /* ===== Apply state-specific UI configuration (only if changed) ===== */
    if (current_ui_state != last_applied_ui_state) {
        apply_ui_state_config(current_ui_state);
        last_applied_ui_state = current_ui_state;
        /* Force valve state colors to update when UI state changes */
        last_valve_state = (ValveState_t)0xFF;  /* Invalid value triggers update */
    }
    
    /* ===== Update customer ID (only when changed) ===== */
    const SystemConfig_t* config = Config_Get();
    if (ui_customerID != NULL) {
        const char* new_customer_id = NULL;
        
        if (card_state == MIFARE_CARD_STATE_PRESENT) {
            /* Card present - get and cache phone number */
            char phone_str[12];
            if (MIFARE_GetCustomerPhoneNumber(phone_str, sizeof(phone_str))) {
                strncpy(ui_ctx.last_phone_number, phone_str, sizeof(ui_ctx.last_phone_number) - 1);
                ui_ctx.last_phone_number[sizeof(ui_ctx.last_phone_number) - 1] = '\0';
                new_customer_id = ui_ctx.last_phone_number;
            }
        } else if (!ui_ctx.showing_persisted_data) {
            /* No card and not persisting - show configured default */
            new_customer_id = config->ui.no_card_customer_id;
        }
        
        /* Only update label if text changed */
        if (new_customer_id != NULL && strcmp(last_customer_id, new_customer_id) != 0) {
            strncpy(last_customer_id, new_customer_id, sizeof(last_customer_id) - 1);
            last_customer_id[sizeof(last_customer_id) - 1] = '\0';
            lv_label_set_text(ui_customerID, last_customer_id);
        }
        /* When persisting after card removal, label keeps cached value */
    }
    
    /* ===== Update dispensedSession label (show during dispense and cooldown) ===== */
    if (ui_dispensedSession != NULL) {
        static char dispensed_str[16];
        
        if (is_dispensing || ui_ctx.in_cooldown) {
            /* Show dispensed amount */
            uint32_t dispensed_ml = is_dispensing ? Dispenser_GetDispensedAmountML() : ui_ctx.dispensed_amount_ml;
            uint32_t dispensed_liters_x10 = dispensed_ml / 100;  /* Convert ml to 0.1L units */
            
            snprintf(dispensed_str, sizeof(dispensed_str), "%lu.%lu L", 
                     dispensed_liters_x10 / 10, dispensed_liters_x10 % 10);
            lv_label_set_text(ui_dispensedSession, dispensed_str);
            
            /* Make visible */
            if (lv_obj_has_flag(ui_dispensedSession, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(ui_dispensedSession, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            /* Hide when not dispensing or in cooldown */
            if (!lv_obj_has_flag(ui_dispensedSession, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(ui_dispensedSession, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    
    /* ===== Update card remaining display (only when changed) ===== */
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
        
        /* Force refresh if mode changed */
        if (current_mode != last_timer_mode) {
            last_dispense_timer_seconds = 0xFFFFFFFF;  /* Reset all caches */
            last_dispense_token_count = 0xFFFFFFFF;
            last_timer_mode = current_mode;
        }
        
        /* Format balance as single string for ui_cardRemaining label */
        static char balance_str[16];
        
        if (dispense_active && card_present_now) {
            /* Dispense active AND card present - show balance as liters */
            uint32_t balance_liters_x10 = card_balance_ml / 100;  /* Convert ml to 0.1L units */
            
            if (balance_liters_x10 != last_dispense_timer_seconds) {
                last_dispense_timer_seconds = balance_liters_x10;
                snprintf(balance_str, sizeof(balance_str), "%lu.%lu L", 
                         balance_liters_x10 / 10, balance_liters_x10 % 10);
                lv_label_set_text(ui_cardRemaining, balance_str);
            }
            
        } else if (dispense_active) {
            /* Dispense active without card - show remaining volume */
            uint32_t remaining_ml = Dispenser_GetDispenseVolumeRemainingMl();
            uint32_t remaining_liters_x10 = remaining_ml / 100;
            
            if (remaining_liters_x10 != last_dispense_timer_seconds) {
                last_dispense_timer_seconds = remaining_liters_x10;
                snprintf(balance_str, sizeof(balance_str), "%lu.%lu L", 
                         remaining_liters_x10 / 10, remaining_liters_x10 % 10);
                lv_label_set_text(ui_cardRemaining, balance_str);
            }
            
        } else if (card_present_now) {
            /* Card present but no active dispense - show balance */
            uint32_t balance_liters_x10 = card_balance_ml / 100;
            
            if (balance_liters_x10 != last_dispense_timer_seconds) {
                last_dispense_timer_seconds = balance_liters_x10;
                last_dispense_token_count = 0xFFFFFFFF;
                snprintf(balance_str, sizeof(balance_str), "%lu.%lu L", 
                         balance_liters_x10 / 10, balance_liters_x10 % 10);
                lv_label_set_text(ui_cardRemaining, balance_str);
            }
        } else {
            /* No card - show default */
            if (last_dispense_timer_seconds != 0xFFFFFFFE) {
                last_dispense_timer_seconds = 0xFFFFFFFE;
                last_dispense_token_count = 0xFFFFFFFF;
                lv_label_set_text(ui_cardRemaining, "--.- L");
            }
        }
    }
    
/* Note: Dispense option symbols (vacSymbol, pressWasherSymbol, washBrushSymbol) 
     * and colored buttons (redButton, blueButton, greedButton) removed - 
     * these elements don't exist in the current UI design */
}


