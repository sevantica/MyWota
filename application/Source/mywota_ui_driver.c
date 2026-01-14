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
#include "MIFARE_Transaction_Core.h"  /* For getter functions */
#include "Car_Wash_Controller.h"         /* For car wash functions */
#include "System_Config.h"                /* For SD card configuration */
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

/* UI Display Context - tracks UI-specific state for display persistence */
typedef struct {
    /* Card removal persistence - keep UI visible for a timeout after card removed */
    bool was_card_present;              /* Previous card state for edge detection */
    uint32_t card_removed_time;         /* Timestamp when card was removed */
    bool showing_persisted_data;        /* True if showing data after card removal */
    
    /* LED flashing state - reserved for future use */
    uint32_t last_led_toggle_time;
    bool led_is_on;
    
    /* Cached display values (shown during persistence timeout) */
    char last_phone_number[12];
    uint32_t last_card_balance_tokens;
} UI_Display_Context_t;

/* Timing Constants */
#define ONE_SECOND_MS                   1000U
#define FPS_UPDATE_INTERVAL_MS          ONE_SECOND_MS
#define LCD_RESET_DELAY_MS              500U

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_LCD_DISPLAY_DRIVER_EN      0
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

/* Performance Monitoring */
static unsigned long frame_count = 0;

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
    .last_led_toggle_time = 0,
    .led_is_on = false,
    .last_phone_number = "",  /* Will be set to config value on first update */
    .last_card_balance_tokens = 0
};

/* Performance Optimization - Cache previous state to avoid redundant updates */
static UI_State_t last_applied_ui_state = UI_STATE_COUNT;  /* Invalid state forces first update */
static WashOption_t last_wash_option = (WashOption_t)0xFF;  /* Invalid value forces first update */
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
    lv_display_set_rotation(lcd_display, LV_DISPLAY_ROTATION_270);
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Display rotation set to 90 degrees (landscape)\r\n");
#elif LV_USE_ILI9341
    /* ILI9341: Rotate 90 degrees for landscape (320x240) */
    lv_display_set_rotation(lcd_display, LV_DISPLAY_ROTATION_90);
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
    
    /* Trigger LVGL render and wait until full screen is rendered.
     * Full screen = DISPLAY_VERTICAL_SIZE lines (ILI9488: 480 lines, ILI9341: 320 lines) */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Rendering initial UI (waiting for %d lines)...\r\n", DISPLAY_VERTICAL_SIZE);
    
    TickType_t render_wait_start = xTaskGetTickCount();
    while (lcd_get_lines_rendered() < DISPLAY_VERTICAL_SIZE) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5)); /* Small delay between render passes */
        
        /* Timeout after 3 seconds to prevent WDT trigger if rendering stalls */
        if ((xTaskGetTickCount() - render_wait_start) > pdMS_TO_TICKS(3000)) {
            LOG_CRITICAL_LCD_DISPLAY_DRIVER("[!] LCD initial render timed out - proceeding anyway\r\n");
            break;
        }
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
        uint32_t lvgl_period = Config_Get()->ui.lvgl_task_period_ms;
        lv_tick_inc(lvgl_period);

        /* Handle screen switching - happens only once after defined delay */
        uint32_t screen_switch_delay_seconds = Config_Get()->ui.screen_switch_delay_ms / 1000U;
        if (!screen_switched && elapsed_seconds >= screen_switch_delay_seconds) 
        {
            /* Backlight already at full brightness from fade-in */
            screen_switched = 1;
        }

        /* Process LVGL rendering */
        unsigned long time_till_next = lv_timer_handler();

        /* Performance monitoring */
        frame_count++;
    unsigned long current_tick = xTaskGetTickCount();
    if ((current_tick - last_fps_update_ms) >= FPS_UPDATE_INTERVAL_MS) 
    {
            frame_count = 0;
            last_fps_update_ms = current_tick;
            
 
        }

        /* Increment time counter only every 1000 ms */
    elapsed_seconds = (xTaskGetTickCount() - start_tick_ms) / ONE_SECOND_MS; /* derive seconds */

        /* Release semaphore */
    xSemaphoreGive(lvgl_mutex);

    /* Dynamic delay based on LVGL recommendation */
    if(time_till_next> 100)
    {
        time_till_next = Config_Get()->ui.display_refresh_ms;
    }
    if (time_till_next < lvgl_period) 
    {
        time_till_next = lvgl_period;
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
    (void)xTaskCreate(lcd_display_task, "LCD_Task", lcd_display_task_stack_size_words, NULL, LCD_DISPLAY_TASK_PRIORITY, &lcd_task_handle);
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
    ui_ctx.last_card_balance_tokens = 0;
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
    
    /* ===== Poll current system state from modules ===== */
    MIFARE_CardState_t card_state = MIFARE_GetCardState();
    bool card_present_now = (card_state == MIFARE_CARD_STATE_PRESENT || 
                             card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE);
    uint32_t current_time = xTaskGetTickCount();
    
    /* Update legacy variable */
    card_present = card_present_now ? 1 : 0;
    
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
    bool is_dispensing = CarWash_IsWashActive();
    UI_State_t current_ui_state = map_card_state_to_ui_state(card_state, is_dispensing);
    
    /* ===== Apply state-specific UI configuration (only if changed) ===== */
    if (current_ui_state != last_applied_ui_state) {
        apply_ui_state_config(current_ui_state);
        last_applied_ui_state = current_ui_state;
        /* Force wash option colors to update when UI state changes */
        last_wash_option = (WashOption_t)0xFF;  /* Invalid value triggers update */
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
    
    /* ===== Update car wash timer and options removed (not available in current UI) ===== */

    
    /* Note: Explicit invalidations removed - LVGL auto-invalidates on property changes */
}


