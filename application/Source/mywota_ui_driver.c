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
#include "mywota_ui_driver.h"
#include "LCD_Driver.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_Screen1.h"
#include "System.h"
#include "Hardware_Access.h" /* For SPI_MSG_DEF and centralized hardware definitions */
#include "USB_Logging.h"
#include "MIFARE_Transaction_Manager.h"  /* For getter functions */
#include "Dispenser_Control.h"           /* For getter functions */
#ifdef LV_USE_ILI9341
#include "display/ili9341/lv_ili9341.h"
#endif

#ifdef LV_USE_ILI9488
#include "display/ili9488/lv_ili9488.h"
#endif

#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include "USB_Logging.h" /* For USB logging and diagnostics */
#include "LCD_Driver.h"

/* ========================================================================== */
/*                           PRIVATE DEFINITIONS                             */
/* ========================================================================== */

/* Timing Configuration */
#define DISPLAY_REFRESH_MS              5U
#define LVGL_TASK_PERIOD_MS             5U
#define UI_DATA_POLL_INTERVAL_MS        100U   /* Poll data every 100ms */

/* UI Configuration */
#define SCREEN_SWITCH_DELAY_MS          4000U   /* 4 seconds delay before switching */
#define SCREEN_SWITCH_DELAY_SECONDS     (SCREEN_SWITCH_DELAY_MS/1000U)

/* Progress Bar Configuration */
#define BAR_MAX_VALUE                   100U
#define BAR_MIN_VALUE                   0U
#define BAR_INCREMENT                   20U
#define BAR_UPDATE_INTERVAL_MS          50U

/* Timing Constants */
#define ONE_SECOND_MS                   1000U
#define FPS_UPDATE_INTERVAL_MS          ONE_SECOND_MS
#define LCD_RESET_DELAY_MS              500U

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

/* Local event constants for LCD processing */
enum { LOCAL_PICC_POS_0 = 0 }; /* Position index 0 */
enum { LOCAL_PICC_STATE_INACTIVE = 0, LOCAL_PICC_STATE_AUTH = 1, LOCAL_PICC_STATE_ACTIVE = 2 };

/* UI element identifiers for generalized UI updates */
typedef enum {
    UI_ELEMENT_TOTAL_REMAINING_BAR = 0,
    UI_ELEMENT_CARD_REMAINING_LABEL = 1,
    UI_ELEMENT_DISPENSED_SESSION_LABEL = 2,
    UI_ELEMENT_STATUS_INDICATOR = 3,
    UI_ELEMENT_CUSTOM = 255
} UI_ELEMENT_ID_Enum;

/* UI actions for generalized UI updates */
typedef enum {
    UI_ACTION_SET_VALUE = 0,
    UI_ACTION_SET_COLOR = 1,
    UI_ACTION_SET_TEXT = 2,
    UI_ACTION_SET_VISIBILITY = 3,
    UI_ACTION_ANIMATE = 4,
    UI_ACTION_CUSTOM = 255
} UI_ACTION_Enum;

/* ========================================================================== */
/*                           PRIVATE VARIABLES                               */
/* ========================================================================== */


/* Synchronization */
static SemaphoreHandle_t lvgl_sem = NULL;

/* Task Handle */
static TaskHandle_t LCD_Display_Driver_TaskHandle;

/* Performance Monitoring */
static unsigned long frame_count = 0;

/* UI State Variables */
static unsigned long time_lapsed = 0;            /* Elapsed seconds */
static uint8_t  screen_switched = 0;
static uint8_t  bar_value = 0;
static uint8_t  card_present = 0;
static unsigned long start_tick_ms = 0;          /* Start tick */
static unsigned long last_bar_update_tick = 0;   /* Last bar update tick */
static unsigned long last_timer_update_tick = 0; /* Last timer label tick */
static unsigned long last_fps_update_tick = 0;   /* Last FPS tick */
static unsigned long last_data_poll_tick = 0;    /* Last data poll tick */

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
static inline unsigned long lcd_next_delay_ms(unsigned long lvgl_next);
static uint8_t lcd_read_register_proper(uint8_t reg_addr, uint8_t *data, uint8_t length);
static void update_ui_from_polled_data(void);

/* ========================================================================== */
/*                         CORE TASK FUNCTION PROTOTYPES                     */
/* ========================================================================== */
static void LCD_Display_Driver_Task(void* argument);



/* ========================================================================== */
/*                         PUBLIC API FUNCTION PROTOTYPES                    */
/* ========================================================================== */
void Task_Start_LCD_Display_Driver_Task(void);
TaskHandle_t task_get_handle_LCD_Display_Driver_Task(void);

/* ========================================================================== */
/*                         LCD READ FUNCTION PROTOTYPES                      */
/* ========================================================================== */
uint8_t lcd_1_read_cmd(uint8_t cmd);
void lcd_1_read_data(uint8_t *buffer, size_t length);

/* ========================================================================== */
/*                         LCD INITIALIZATION FUNCTION PROTOTYPES            */
/* ========================================================================== */

void lcd_test_init(void);
void lcd_fill_test(void);
void lcd_fill_test(void);


/* ========================================================================== */
/*                           MAIN TASK FUNCTION                              */
/* ========================================================================== */


lv_display_t *display1 = NULL;

/**
 * @brief Main LCD Display Driver Task
 * @param argument Task argument (unused)
 */
static void LCD_Display_Driver_Task(void* argument)
{
    /* Wait for task notification to start */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Starting LCD Display Driver initialization...\r\n");
    
    /* Initialize GPIO pins */
    lcd_gpio_init();
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: GPIO initialized\r\n");
    
    /* Initialize synchronization primitives */
    lvgl_sem = xSemaphoreCreateMutex();
    if (lvgl_sem == NULL) {
        LOG_ERROR_LCD_DISPLAY_DRIVER("LCD: ERROR - Failed to create LVGL mutex\r\n");
        vTaskDelete(NULL);
        return;
    }
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Mutex created\r\n");

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

    /* Create the display using a generic approach for now
     * Note: ILI9488 specific driver may need LV_USE_ILI9488 to be defined
     * For now, commenting out to focus on HAL replacement */
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Creating ILI9341 display driver (%dx%d)...\r\n", DISPLAY_HORIZONTAL_SIZE, DISPLAY_VERTICAL_SIZE);
    display1 = lv_ili9341_create(DISPLAY_HORIZONTAL_SIZE, DISPLAY_VERTICAL_SIZE,
                                 (lv_lcd_flag_t)0,
                                lcd_1_send_cmd,
                                lcd_1_send_color);

    if (display1 == NULL) {
        LOG_ERROR_LCD_DISPLAY_DRIVER("LCD: ERROR - Display creation failed\r\n");
        // Display creation failed - cleanup and exit
        vSemaphoreDelete(lvgl_sem);
        vTaskDelete(NULL);
        return;
    }
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Display driver created successfully\r\n");
    

    /* Use native physical controller orientation (landscape 480x320). */
    
    /* For now, create a basic display buffer setup */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Configuring display buffers (%d bytes)...\r\n", DISPLAY_BUFFER_SIZE);
    lv_display_set_buffers(display1, display_buffer, NULL, DISPLAY_BUFFER_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Display buffers configured\r\n");

    /* Optional: set panel gap/offsets if the glass has non-zero origin. Start with 0,0. */
   
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Initializing UI...\r\n");
    ui_init();
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: UI initialized\r\n");
    
    /* Keep backlight off initially - will fade in after first render */
    lcd_backlight_on(0);
    
    /* Trigger initial LVGL render to draw the UI to screen */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Rendering initial UI frame...\r\n");
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(50)); /* Allow time for display to update */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Initial render complete\r\n");
    
    /* Now fade in backlight smoothly after UI is rendered to screen */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Fading in backlight...\r\n");
    for (uint8_t brightness = 0; brightness <= 100; brightness += 5) {
        lcd_backlight_on(brightness);
        vTaskDelay(pdMS_TO_TICKS(30)); /* 30ms per step = 600ms total fade time */
    }
    lcd_backlight_on(100); /* Ensure we end at exactly 100% */
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Backlight at 100%%, initialization complete\r\n");
    
    /* MAIN TASK LOOP */
    TickType_t xLastWakeTime = xTaskGetTickCount();
    start_tick_ms = xTaskGetTickCount();
    last_fps_update_tick = start_tick_ms;
    last_data_poll_tick = start_tick_ms;

    for(;;) {
       TASK_HEARTBEAT_EVERY_SECOND("LCD");
       
        /* Poll data from modules at regular intervals */
        unsigned long now_tick = xTaskGetTickCount();
        if ((now_tick - last_data_poll_tick) >= UI_DATA_POLL_INTERVAL_MS) {
            last_data_poll_tick = now_tick;
            
            /* Acquire LVGL protection semaphore */
            if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(10)) == pdPASS) {
                update_ui_from_polled_data();
                xSemaphoreGive(lvgl_sem);
            }
        }
        
        /* Acquire LVGL protection semaphore (wait forever, expect success) */
    xSemaphoreTake(lvgl_sem, portMAX_DELAY);

        /* Update progress bar every 1000 ms when card present */
        if (card_present) {
            unsigned long now_tick = xTaskGetTickCount();
            if ((now_tick - last_bar_update_tick) >= BAR_UPDATE_INTERVAL_MS) {
                last_bar_update_tick = now_tick;
                bar_value = (uint8_t)(bar_value + BAR_INCREMENT);
                if (bar_value >= BAR_MAX_VALUE) {
                    bar_value = BAR_MAX_VALUE;
                    
                    bar_value = BAR_MIN_VALUE; /* restart */
                } else {
                    
                }
            }
        }

        /* Handle screen switching - happens only once after defined delay */
        if (!screen_switched && time_lapsed >= SCREEN_SWITCH_DELAY_SECONDS) 
        {
            /* Backlight already at full brightness from fade-in */
            screen_switched = 1;
            last_timer_update_tick = xTaskGetTickCount();

        }

        /* Update timer display */
        if (screen_switched) 
        {
            unsigned long now_tick = xTaskGetTickCount();
            if ((now_tick - last_timer_update_tick) >= ONE_SECOND_MS) 
            {
                last_timer_update_tick = now_tick;
     
            }
        }

        /* Process LVGL rendering */
        unsigned long time_till_next = lv_timer_handler();

        /* Performance monitoring */
        frame_count++;
    unsigned long current_tick = xTaskGetTickCount();
    if ((current_tick - last_fps_update_tick) >= FPS_UPDATE_INTERVAL_MS) 
    {
            frame_count = 0;
            last_fps_update_tick = current_tick;
            
 
        }

        /* Increment time counter only every 1000 ms */
    time_lapsed = (xTaskGetTickCount() - start_tick_ms) / ONE_SECOND_MS; /* derive seconds */

        /* Release semaphore */
    xSemaphoreGive(lvgl_sem);

    /* Dynamic delay based on LVGL recommendation */
    if(time_till_next> 100)
    {
        time_till_next = DISPLAY_REFRESH_MS;
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
    (void)xTaskCreate(LCD_Display_Driver_Task, "LCD_Display_Driver Task", lcd_display_task_stack_size_words, NULL, LCD_DISPLAY_TASK_PRIORITY, &LCD_Display_Driver_TaskHandle);
}

/**
 * @brief Get the LCD Display Driver Task handle
 * @return Task handle
 */
TaskHandle_t task_get_handle_LCD_Display_Driver_Task() { return LCD_Display_Driver_TaskHandle; }

/**
 * @brief Fill screen with test colors to verify LCD hardware
 */
void lcd_fill_test(void)
{
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Testing screen fill...\\r\\n");
    
    // Create a simple test screen with red color
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), 0);  // Red
    lv_obj_invalidate(scr);
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Screen set to RED, calling lv_timer_handler()...\\r\\n");
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Changing to GREEN...\\r\\n");
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x00FF00), 0);  // Green
    lv_obj_invalidate(scr);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Changing to BLUE...\\r\\n");
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0000FF), 0);  // Blue
    lv_obj_invalidate(scr);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    LOG_DEBUG_LCD_DISPLAY_DRIVER("LCD: Fill test complete\\r\\n");
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
    if (lvgl_sem == NULL || obj == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        if (visible) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        xSemaphoreGive(lvgl_sem);
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
    if (lvgl_sem == NULL || label == NULL || text == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        lv_label_set_text(label, text);
        lv_obj_invalidate(label); // Force refresh
        xSemaphoreGive(lvgl_sem);
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
    if (lvgl_sem == NULL || bar == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        lv_bar_set_value(bar, value, anim);
        xSemaphoreGive(lvgl_sem);
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
    if (lvgl_sem == NULL || obj == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        lv_obj_set_style_bg_color(obj, color, selector);
        xSemaphoreGive(lvgl_sem);
        return true;
    }
    return false;
}

/* ========================================================================== */
/*                          EVENT PROCESSING FUNCTIONS                       */
/* ========================================================================== */

/**
 * @brief Update UI by polling data from modules (replaces event-based updates)
 * @note Called periodically by main task loop with LVGL semaphore held
 */
static void update_ui_from_polled_data(void)
{
    /* Poll data from MIFARE Transaction Manager */
    uint32_t card_balance_ml = MIFARE_GetBalanceML();
    bool card_status = MIFARE_GetCardStatus();
    
    /* Poll data from Dispenser Control */
    uint32_t dispensed_ml = Dispenser_GetDispensedSessionML();
    bool is_dispensing = Dispenser_IsDispensing();
    
    /* Update card remaining label */
    if (ui_cardRemaining != NULL) {
        static char balance_str[16];
        if (card_balance_ml > 9000) {
            uint32_t liters = card_balance_ml / 1000;
            snprintf(balance_str, sizeof(balance_str), "%luL", liters);
        } else {
            snprintf(balance_str, sizeof(balance_str), "%lumL", card_balance_ml);
        }
        lv_label_set_text(ui_cardRemaining, balance_str);
    }
    
    /* Update total remaining bar (assume max 1000L capacity) */
    if (ui_totalRemainingBar != NULL) {
        uint32_t max_capacity = 1000000; // 1000L in mL
        uint8_t percentage = 0;
        if (card_balance_ml >= max_capacity) {
            percentage = 100;
        } else {
            percentage = (uint8_t)((card_balance_ml * 100) / max_capacity);
        }
        lv_bar_set_value(ui_totalRemainingBar, percentage, LV_ANIM_ON);
    }
    
    /* Update dispensed session label */
    if (ui_dispensedSession != NULL) {
        static char dispensed_str[16];
        if (dispensed_ml > 9000) {
            uint32_t liters = dispensed_ml / 1000;
            snprintf(dispensed_str, sizeof(dispensed_str), "%luL", liters);
        } else {
            snprintf(dispensed_str, sizeof(dispensed_str), "%lumL", dispensed_ml);
        }
        lv_label_set_text(ui_dispensedSession, dispensed_str);
    }
    
    /* Update card present state */
    card_present = card_status ? 1 : 0;
    
    /* Optional: Add visual feedback for dispensing state */
    (void)is_dispensing;  // Can be used to show/hide dispensing indicator
}
