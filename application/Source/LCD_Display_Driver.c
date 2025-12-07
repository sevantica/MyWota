/*
 * LCD Display Driver - Optimized for STM32F411 with ILI9488
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
#include "LCD_Display_Driver.h"
#include "LCD_Driver.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_Screen1.h"
#include "System.h"
#include "Hardware_Access.h" /* For SPI_MSG_DEF and centralized hardware definitions */
#include "USB_Logging.h"
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

/* ========================================================================== */
/*                         CORE TASK FUNCTION PROTOTYPES                     */
/* ========================================================================== */
static void LCD_Display_Driver_Task(void* argument);

/* ========================================================================== */
/*                         EVENT PROCESSING FUNCTION PROTOTYPES              */
/* ========================================================================== */
static void process_rfid_event(DISPLAY_MSG_Def* msg);
static void process_gpio_event(DISPLAY_MSG_Def* msg);
static void process_sensor_event(DISPLAY_MSG_Def* msg);
static void process_ui_update_event(DISPLAY_MSG_Def* msg);
static void process_system_state_event(DISPLAY_MSG_Def* msg);
static void process_user_input_event(DISPLAY_MSG_Def* msg);
static void process_flow_sensor_event(DISPLAY_MSG_Def* msg);
static void process_i2c_device_event(DISPLAY_MSG_Def* msg);
static void process_custom_event(DISPLAY_MSG_Def* msg);

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

    /* Initialize GPIO pins */
    lcd_gpio_init();
    
    /* Initialize synchronization primitives */
    lvgl_sem = xSemaphoreCreateMutex();
    if (lvgl_sem == NULL) { vTaskDelete(NULL); return; }

    Hardware_LCD_Reset();
    vTaskDelay(pdMS_TO_TICKS(105)); /* Match standalone driver timing */
    
    /* Turn on backlight */
   
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Initialize LVGL and the display driver */
    lv_init();

    /* Create the display using a generic approach for now
     * Note: ILI9488 specific driver may need LV_USE_ILI9488 to be defined
     * For now, commenting out to focus on HAL replacement */
    
    display1 = lv_ili9341_create(DISPLAY_HORIZONTAL_SIZE, DISPLAY_VERTICAL_SIZE,
                                 (lv_lcd_flag_t)0,
                                lcd_1_send_cmd,
                                lcd_1_send_color);

    if (display1 == NULL) {
        // Display creation failed - cleanup and exit
        vSemaphoreDelete(lvgl_sem);
        vTaskDelete(NULL);
        return;
    }
    

    /* Use native physical controller orientation (landscape 480x320). */
    
    /* For now, create a basic display buffer setup */
    lv_display_set_buffers(display1, display_buffer, NULL, DISPLAY_BUFFER_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Optional: set panel gap/offsets if the glass has non-zero origin. Start with 0,0. */
   
    /* One-time visual sanity check: fill screen and toggle inversion before starting LVGL UI */


    ui_init();
    
    /* Keep backlight off initially - will fade in after first render */
    lcd_backlight_on(0);
    
    /* Trigger initial LVGL render to draw the UI to screen */
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(50)); /* Allow time for display to update */
    
    /* Now fade in backlight smoothly after UI is rendered to screen */
    for (uint8_t brightness = 0; brightness <= 100; brightness += 5) {
        lcd_backlight_on(brightness);
        vTaskDelay(pdMS_TO_TICKS(30)); /* 30ms per step = 600ms total fade time */
    }
    lcd_backlight_on(100); /* Ensure we end at exactly 100% */
    
    /* MAIN TASK LOOP */
    TickType_t xLastWakeTime = xTaskGetTickCount();
    start_tick_ms = xTaskGetTickCount();
    last_fps_update_tick = start_tick_ms;

    

    //lcd_rotate_display(3); /* Rotate to portrait */=
    for(;;) {
       TASK_HEARTBEAT_EVERY_SECOND("LCD");
        /* Process display messages */
        DISPLAY_MSG_Def display_msg;
        QueueHandle_t display_queue = get_msg_queue_display();
        if(display_queue != NULL) {
            if(xQueueReceive(display_queue, &display_msg, pdMS_TO_TICKS(100)) == pdPASS) {
                /* Protect LVGL calls with semaphore */
                xSemaphoreTake(lvgl_sem, portMAX_DELAY);
                
                /* Process different event types */
                switch(display_msg.event_type) {
                    case EVENT_TYPE_RFID_PICC:
                        process_rfid_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_GPIO_PIN:
                        process_gpio_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_SENSOR:
                        process_sensor_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_UI_UPDATE:
                        process_ui_update_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_SYSTEM_STATE:
                        process_system_state_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_USER_INPUT:
                        process_user_input_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_FLOW_SENSOR:
                        process_flow_sensor_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_I2C_DEVICE:
                        process_i2c_device_event(&display_msg);
                        break;
                        
                    case EVENT_TYPE_CUSTOM:
                        process_custom_event(&display_msg);
                        break;
                        
                    default:
                        USB_Log_Printf("LCD: Unknown event type %d received\r\n", display_msg.event_type);
                        break;
                }
                
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

/* ========================================================================== */
/*                           LVGL INTERFACE FUNCTIONS                        */
/* ========================================================================== */


/* ========================================================================== */
/*                            UI CONTROL FUNCTIONS                           */
/* ========================================================================== */

/**
 * @brief Update the total remaining bar with percentage of last top-up
 * @param current_balance_ml Current balance in milliliters
 * @param last_topup_amount_ml Last top-up amount for percentage calculation
 * 
 * @details Updates the ui_totalRemainingBar with the percentage of remaining balance
 *          versus the last top-up amount. The bar shows 0-100% where 100% represents
 *          the full top-up amount and 0% represents empty.
 */
void ui_update_total_remaining_bar(uint32_t current_balance_ml, uint32_t last_topup_amount_ml)
{
    // Check if LVGL semaphore is available
    if (lvgl_sem == NULL) {

        return;
    }
    
    // Check if the UI element exists
    extern lv_obj_t * ui_totalRemainingBar;
    if (ui_totalRemainingBar == NULL) {
     
        return;
    }
    
    // Calculate percentage (0-100)
    uint8_t percentage = 0;
    if (last_topup_amount_ml > 0) {
        // Ensure we don't exceed 100% even if balance somehow exceeds topup amount
        if (current_balance_ml >= last_topup_amount_ml) {
            percentage = 100;
        } else {
            percentage = (uint8_t)((current_balance_ml * 100) / last_topup_amount_ml);
        }
    }
    
    // Acquire LVGL protection semaphore
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        
        // Update the progress bar (LVGL bars typically use 0-100 range)
        lv_bar_set_value(ui_totalRemainingBar, percentage, LV_ANIM_ON);
        
        // Update level color indicator based on percentage
        extern lv_obj_t * ui_levelColourIndicator;
        if (ui_levelColourIndicator != NULL) {
            if (percentage > 25) {
                // Green for above 25%
                lv_obj_set_style_bg_color(ui_levelColourIndicator, lv_color_hex(0x05820A), LV_PART_MAIN | LV_STATE_DEFAULT);
            } else if (percentage > 10) {
                // Amber/Orange for above 10% but 25% or less
                lv_obj_set_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT);
            } else {
                // Red for 10% or less
                lv_obj_set_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }
        
        // Release the semaphore
        xSemaphoreGive(lvgl_sem);
        
        USB_Log_Printf("UI: Updated remaining bar to %u%% (%u/%u mL)\r\n", 
                       percentage, current_balance_ml, last_topup_amount_ml);
    } else {
        USB_Log_Printf("UI: Failed to acquire LVGL semaphore for bar update\r\n");
    }
}

/**
 * @brief Update the card remaining balance display on the UI
 * @param balance_ml Current card balance in milliliters
 * 
 * @details This function safely updates the ui_cardRemaining label with the current
 *          card balance. It handles LVGL thread safety using the lvgl_sem semaphore.
 *          The balance is displayed in liters (L) with 1 decimal place for values > 9L,
 *          or in milliliters (ml) for values <= 9L.
 */
void ui_update_card_remaining_balance(uint32_t balance_ml)
{

    // Check if LVGL semaphore is available
    if (lvgl_sem == NULL) {
        USB_Log_Printf("UI: LVGL semaphore not initialized\r\n");
        return;
    }
    
    // Check if the UI element exists
    extern lv_obj_t * ui_cardRemaining;
    if (ui_cardRemaining == NULL) {
        USB_Log_Printf("UI: cardRemaining label not initialized\r\n");
        return;
    }
    
    // Acquire LVGL protection semaphore
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        
        static char balance_str[16];

        // Display in liters (L) for values > 9000 ml (9L), otherwise in milliliters (ml)
        if (balance_ml > 9000) {
            uint32_t liters = balance_ml / 1000;
            snprintf(balance_str, sizeof(balance_str), "%luL", liters);
        } else {
            snprintf(balance_str, sizeof(balance_str), "%luml", balance_ml);
        }
        
        
        // Update the UI label
        lv_label_set_text(ui_cardRemaining, balance_str);
        
        // Force label to recalculate size and refresh
        lv_obj_invalidate(ui_cardRemaining);
        
        // Release the semaphore
        xSemaphoreGive(lvgl_sem);
        
        USB_Log_Printf("UI: Updated card balance display to: %s\r\n", balance_str);
    } else {
        USB_Log_Printf("UI: Failed to acquire LVGL semaphore for balance update\r\n");
    }
}

/**
 * @brief Update the dispensed session display on the UI
 * @param dispensed_ml Amount dispensed in current session in milliliters
 * 
 * @details This function safely updates the ui_dispensedSession label with the 
 *          cumulative amount dispensed in the current session. It handles LVGL 
 *          thread safety using the lvgl_sem semaphore.
 */
void ui_update_dispensed_session(uint32_t dispensed_ml)
{
    // Check if LVGL semaphore is available
    if (lvgl_sem == NULL) {
        USB_Log_Printf("UI: LVGL semaphore not initialized\r\n");
        return;
    }
    
    // Check if the UI element exists
    extern lv_obj_t * ui_dispensedSession;
    if (ui_dispensedSession == NULL) {
        USB_Log_Printf("UI: dispensedSession label not initialized\r\n");
        return;
    }
    
    // Acquire LVGL protection semaphore
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        
        static char dispensed_str[16];
        // Convert ml to liters with one decimal place
        uint32_t liters = dispensed_ml / 1000;
        uint32_t decimal = (dispensed_ml % 1000) / 100;
        snprintf(dispensed_str, sizeof(dispensed_str), "%lu.%luL", liters, decimal);
        // Update the UI label
        lv_label_set_text(ui_dispensedSession, dispensed_str);
        
        // Release the semaphore
        xSemaphoreGive(lvgl_sem);
        
        USB_Log_Printf("UI: Updated dispensed session display to: %s\r\n", dispensed_str);
    } else {
        USB_Log_Printf("UI: Failed to acquire LVGL semaphore for dispensed session update\r\n");
    }
}

/**
 * @brief Update the customer ID display on the UI
 * @param phone_number 64-bit phone number from card account data
 * 
 * @details This function safely updates the ui_customerID label with the phone number
 *          read from the MIFARE card's account data. It handles LVGL thread safety
 *          using the lvgl_sem semaphore. Phone number is displayed as an 11-digit string.
 *          The element is made visible when displaying a phone number and hidden when
 *          phone_str is NULL (card removed).
 */
void ui_update_customer_id(const char* phone_str)
{
    // Check if LVGL semaphore is available
    if (lvgl_sem == NULL) {
        USB_Log_Printf("UI: LVGL semaphore not initialized\r\n");
        return;
    }
    
    // Check if the UI element exists
    extern lv_obj_t * ui_customerID;
    if (ui_customerID == NULL) {
        USB_Log_Printf("UI: customerID label not initialized\r\n");
        return;
    }
    
    // Acquire LVGL protection semaphore
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        
        // Check if phone string is NULL or empty (card removed)
        if (phone_str == NULL || phone_str[0] == '\0') {
            // Hide the label when card is removed
            lv_obj_add_flag(ui_customerID, LV_OBJ_FLAG_HIDDEN);
            USB_Log_Printf("UI: Customer ID hidden (card removed)\r\n");
        } else {
            // Update the UI label with phone string
            lv_label_set_text(ui_customerID, phone_str);
            
            // Make the label visible
            lv_obj_clear_flag(ui_customerID, LV_OBJ_FLAG_HIDDEN);
            
            USB_Log_Printf("UI: Customer ID visible with number: %s\r\n", phone_str);
            
            // Force label to recalculate size and refresh
            lv_obj_invalidate(ui_customerID);
        }
        
        // Release the semaphore
        xSemaphoreGive(lvgl_sem);
    } else {
        USB_Log_Printf("UI: Failed to acquire LVGL semaphore for customer ID update\r\n");
    }
}

/**
 * @brief Update the button state display on the UI
 * @param button_state Current button state (0=LOW/Released, 1=HIGH/Pressed)
 * 
 * @details This function safely updates the ui_buttonState label with the 
 *          current button state. It handles LVGL thread safety using the 
 *          lvgl_sem semaphore.
 */
void ui_update_button_state(uint8_t button_state)
{
    // Check if LVGL semaphore is available
    if (lvgl_sem == NULL) {
        USB_Log_Printf("UI: LVGL semaphore not initialized\r\n");
        return;
    }
    
    // Check if the UI element exists
    extern lv_obj_t * ui_buttonState;
    if (ui_buttonState == NULL) {
        USB_Log_Printf("UI: buttonState label not initialized\r\n");
        return;
    }
    
    // Acquire LVGL protection semaphore
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        
        // Update the UI label with button state text
        if (button_state == 1) {
            lv_label_set_text(ui_buttonState, "HIGH");
        } else {
            lv_label_set_text(ui_buttonState, "LOW");
        }
        
        // Release the semaphore
        xSemaphoreGive(lvgl_sem);
        
        USB_Log_Printf("UI: Updated button state display to: %s\r\n", button_state ? "HIGH" : "LOW");
    } else {
        USB_Log_Printf("UI: Failed to acquire LVGL semaphore for button state update\r\n");
    }
}

/**
 * @brief Update the flow rate label on the UI
 * @param flow_rate_lpm Flow rate in liters per minute
 * 
 * @details This function safely updates the ui_flowRate label with the current
 *          flow rate. It handles LVGL thread safety using the lvgl_sem semaphore.
 */
void ui_update_flow_rate_label(float flow_rate_lpm)
{
    // Check if LVGL semaphore is available
    if (lvgl_sem == NULL) {
        return;
    }
    
    // Check if the UI element exists
    extern lv_obj_t * ui_flowRateSensor;
    // Note: ui_flowRateSensor might not be defined in ui.h if not created in SquareLine Studio
    // We should check if it exists, but for now we assume it's declared extern
    // If it's not in ui.h/c, this will cause linker error.
    // But the user code calls this function, so presumably the UI element exists or is intended.
    
    if (ui_flowRateSensor == NULL) {
        return;
    }
    
    // Acquire LVGL protection semaphore
    if (xSemaphoreTake(lvgl_sem, pdMS_TO_TICKS(100)) == pdPASS) {
        
        static char flow_str[16];
        snprintf(flow_str, sizeof(flow_str), "%.1f L/min", flow_rate_lpm);
        
        // Update the UI label
        lv_label_set_text(ui_flowRateSensor, flow_str);
        
        // Release the semaphore
        xSemaphoreGive(lvgl_sem);
    }
}

/* ========================================================================== */
/*                          EVENT PROCESSING FUNCTIONS                       */
/* ========================================================================== */

/**
 * @brief Process RFID/PICC events (backwards compatibility)
 * @param msg Pointer to the event message
 */
static void process_rfid_event(DISPLAY_MSG_Def* msg) {
    if (msg->event_data.rfid_data.picc_position == LOCAL_PICC_POS_0) {
        if (msg->event_data.rfid_data.picc_state == LOCAL_PICC_STATE_ACTIVE) {
            card_present = 1;
            USB_Log_Printf("LCD: RFID card active (UID: 0x%08lX)\r\n", msg->event_data.rfid_data.card_uid);
        } else {
            card_present = 0;
            bar_value = 0;
            USB_Log_Printf("LCD: RFID card inactive/auth failed\r\n");
        }
    }
}

/**
 * @brief Process GPIO pin state change events
 * @param msg Pointer to the event message
 */
static void process_gpio_event(DISPLAY_MSG_Def* msg) {
    uint8_t pin_id = msg->event_data.gpio_data.pin_id;
    uint8_t pin_state = msg->event_data.gpio_data.pin_state;
    
    USB_Log_Printf("LCD: GPIO Pin %d changed to %s\r\n", pin_id, pin_state ? "HIGH" : "LOW");
    
    /* Handle specific GPIO pins */
    switch (pin_id) {
        case 12: /* CAT9555 Pin 12 - Update bar color based on pin state */
            if (ui_totalRemainingBar != NULL) {
                if (pin_state == 1) {
                    /* Pin HIGH - Green bar */
                    lv_obj_set_style_bg_color(ui_totalRemainingBar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
                    lv_bar_set_value(ui_totalRemainingBar, 100, LV_ANIM_ON);
                } else {
                    /* Pin LOW - Red bar */
                    lv_obj_set_style_bg_color(ui_totalRemainingBar, lv_color_hex(0xFF0000), LV_PART_INDICATOR);
                    lv_bar_set_value(ui_totalRemainingBar, 0, LV_ANIM_ON);
                }
            }
            break;
            
        default:
            USB_Log_Printf("LCD: Unhandled GPIO pin %d event\r\n", pin_id);
            break;
    }
}

/**
 * @brief Process sensor events
 * @param msg Pointer to the event message
 */
static void process_sensor_event(DISPLAY_MSG_Def* msg) {
    uint8_t sensor_id = msg->event_data.sensor_data.sensor_id;
    uint32_t sensor_value = msg->event_data.sensor_data.sensor_value;
    
    USB_Log_Printf("LCD: Sensor %d value: %lu\r\n", sensor_id, sensor_value);
    
    /* Handle specific sensors */
    /* Add sensor-specific UI updates here */
}

/**
 * @brief Process direct UI update events
 * @param msg Pointer to the event message
 */
static void process_ui_update_event(DISPLAY_MSG_Def* msg) {
    uint8_t element_id = msg->event_data.ui_data.ui_element_id;
    uint8_t action = msg->event_data.ui_data.ui_action;
    uint32_t value = msg->event_data.ui_data.ui_value;
    uint32_t color = msg->event_data.ui_data.ui_color;
    
    USB_Log_Printf("LCD: UI update - Element: %d, Action: %d, Value: %lu\r\n", element_id, action, value);
    
    switch (element_id) {
        case UI_ELEMENT_TOTAL_REMAINING_BAR:
            if (ui_totalRemainingBar != NULL) {
                switch (action) {
                    case UI_ACTION_SET_VALUE:
                        lv_bar_set_value(ui_totalRemainingBar, (int32_t)value, LV_ANIM_ON);
                        break;
                    case UI_ACTION_SET_COLOR:
                        lv_obj_set_style_bg_color(ui_totalRemainingBar, lv_color_hex(color), LV_PART_INDICATOR);
                        break;
                }
            }
            break;
            
        case UI_ELEMENT_CARD_REMAINING_LABEL:
            if (ui_cardRemaining != NULL && action == UI_ACTION_SET_VALUE) {
                /* Update card balance display */
                ui_update_card_remaining_balance(value);
            }
            break;
            
        default:
            USB_Log_Printf("LCD: Unhandled UI element %d\r\n", element_id);
            break;
    }
}

/**
 * @brief Process system state change events
 * @param msg Pointer to the event message
 */
static void process_system_state_event(DISPLAY_MSG_Def* msg) {
    uint8_t component = msg->event_data.system_data.system_component;
    uint8_t state = msg->event_data.system_data.system_state;
    uint16_t error_code = msg->event_data.system_data.error_code;
    
    USB_Log_Printf("LCD: System component %d state changed to %d (error: %d)\r\n", component, state, error_code);
    
    /* Handle system state changes for UI feedback */
    /* Add system-specific UI updates here */
}

/**
 * @brief Process user input events
 * @param msg Pointer to the event message
 */
static void process_user_input_event(DISPLAY_MSG_Def* msg) {
    uint8_t input_id = msg->event_data.user_input_data.input_id;
    uint8_t input_type = msg->event_data.user_input_data.input_type;
    uint8_t input_action = msg->event_data.user_input_data.input_action;
    
    USB_Log_Printf("LCD: User input - ID: %d, Type: %d, Action: %d\r\n", input_id, input_type, input_action);
    
    /* Handle user input for UI interactions */
    /* Add input-specific UI updates here */
}

/**
 * @brief Process flow sensor events
 * @param msg Pointer to the event message
 */
static void process_flow_sensor_event(DISPLAY_MSG_Def* msg) {
    uint32_t flow_value = msg->event_data.sensor_data.sensor_value;
    uint8_t sensor_status = msg->event_data.sensor_data.sensor_status;
    
    USB_Log_Printf("LCD: Flow sensor value: %lu, Status: %d\r\n", flow_value, sensor_status);
    
    /* Update UI based on flow sensor data */
    /* Add flow-specific UI updates here */
}

/**
 * @brief Process I2C device events
 * @param msg Pointer to the event message
 */
static void process_i2c_device_event(DISPLAY_MSG_Def* msg) {
    uint8_t device_id = msg->event_data.system_data.system_component;
    uint8_t device_state = msg->event_data.system_data.system_state;
    
    USB_Log_Printf("LCD: I2C device %d state: %d\r\n", device_id, device_state);
    
    /* Handle I2C device state changes for UI feedback */
    /* Add I2C device-specific UI updates here */
}

/**
 * @brief Process custom application events
 * @param msg Pointer to the event message
 */
static void process_custom_event(DISPLAY_MSG_Def* msg) {
    USB_Log_Printf("LCD: Custom event from source %d\r\n", msg->event_source);
    
    /* Handle custom application-specific events */
    /* Add custom event processing here */
}
