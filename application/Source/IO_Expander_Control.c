/*
 * @attention
 * Copyright (c) Sevantica 2025.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file IO_Expander_Control.c
 * @brief FreeRTOS task for polling and controlling CAT9555 I/O Expander
 * @details Polls all I/O pins at fixed rate, caches states, and provides control functions
 */

/* Includes ------------------------------------------------------------------*/
#include "IO_Expander_Control.h"
#include "Hardware_Access.h"
#include "CAT9555_Driver.h"
#include "USB_Logging.h"
#include "System.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

/*Private defines ---------------------------------------------------*/
/* Polling configuration */
#define IO_EXP_POLL_RATE_MS            (50u)   // Poll every 50ms (20Hz)
#define IO_EXP_DEBOUNCE_COUNT          (3u)    // Require 3 consistent reads for button

/* Logging Configuration */
#define LOG_DEBUG_IO_EXP_CTRL_EN      0
#define LOG_CRITICAL_IO_EXP_CTRL_EN   1
#define LOG_ERROR_IO_EXP_CTRL_EN      1

#if LOG_DEBUG_IO_EXP_CTRL_EN
    #define IO_EXP_CTRL_DEBUG(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define IO_EXP_CTRL_DEBUG(...)
#endif

#if LOG_CRITICAL_IO_EXP_CTRL_EN
    #define IO_EXP_CTRL_CRITICAL(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define IO_EXP_CTRL_CRITICAL(...)
#endif

#if LOG_ERROR_IO_EXP_CTRL_EN
    #define IO_EXP_CTRL_ERROR(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define IO_EXP_CTRL_ERROR(...)
#endif

#define IO_EXP_LOG(fmt, ...) IO_EXP_CTRL_DEBUG("IO_EXP_CTRL: " fmt "\r\n", ##__VA_ARGS__)

/*Private variables -------------------------------------------------*/
static CAT9555_Handle_t *s_io_expander_handle = NULL;
static bool s_initialized = false;
static TaskHandle_t s_task_handle = NULL;

/* Cached pin states (read from hardware) - thread-safe via FreeRTOS */
static IO_Expander_State_t s_pin_states[IO_EXP_NUM_PINS];

/* Debounce state for button */
static uint8_t s_button_debounce_count = 0;
static bool s_button_pressed = false;

/*Private function prototypes ---------------------------------------*/
static void IO_Expander_Control_Task(void* argument);
static IO_Expander_Control_Status_t convert_cat9555_status(CAT9555_Status_t cat_status);
static void poll_all_inputs(void);
static void handle_button_debounce(IO_Expander_State_t raw_button_state);

/*Public Functions ---------------------------------------------------*/

/**
 * @brief Initialize I/O Expander Control module
 */
IO_Expander_Control_Status_t IO_Expander_Control_Init(void)
{
    if (s_initialized) {
        IO_EXP_LOG("Already initialized");
        return IO_EXP_CTRL_OK;
    }

    // Initialize cached states to low
    memset(s_pin_states, IO_EXP_STATE_LOW, sizeof(s_pin_states));

    // Get I/O expander handle from driver (driver owns the memory)
    s_io_expander_handle = CAT9555_GetHandle(0);
    if (s_io_expander_handle == NULL) {
        IO_EXP_CTRL_ERROR("Failed to get CAT9555 handle");
        return IO_EXP_CTRL_ERROR_HARDWARE;
    }

    // Initialize hardware (idempotent - safe to call multiple times)
    init_io_expander_hw();

    // Get pin configuration
    IO_Expander_Pins_t io_pins = Get_IO_Expander_Pins();

    // Initialize CAT9555 driver
    CAT9555_Status_t status = CAT9555_Init(s_io_expander_handle, io_pins.i2c_address);
    if (status != CAT9555_OK) {
        IO_EXP_CTRL_ERROR("CAT9555_Init failed: %s", CAT9555_GetStatusString(status));
        return convert_cat9555_status(status);
    }

    // Mark as initialized before configuring pins (allows wrapper functions to work)
    s_initialized = true;

    // Configure output pins
    // Note: Pin 0, 1, 2 reserved for wash option buttons (inputs)
    // Main relay moved to pin 3 (was keypad row 2)
    IO_Expander_SetPinOutput(IO_EXP_PIN_KEYPAD_ROW_2);  // Pin 3 - repurposed as main relay
    IO_Expander_WritePin(IO_EXP_PIN_KEYPAD_ROW_2, IO_EXP_STATE_LOW);  // Relay off
    
    IO_Expander_SetPinOutput(IO_EXP_PIN_STATUS_LED);
    IO_Expander_WritePin(IO_EXP_PIN_STATUS_LED, IO_EXP_STATE_LOW);  // LED off
    
    IO_Expander_SetPinOutput(IO_EXP_PIN_BUZZER);
    IO_Expander_WritePin(IO_EXP_PIN_BUZZER, IO_EXP_STATE_LOW);      // Buzzer off (managed by Buzzer_Driver)

    // Configure input pins
    // Pins 0, 1, 2 configured by Car_Wash_Controller as button inputs
    IO_Expander_SetPinInput(IO_EXP_PIN_USER_BUTTON);
    IO_Expander_SetPinInput(IO_EXP_PIN_RELAY_SENSE_1);
    IO_Expander_SetPinInput(IO_EXP_PIN_RELAY_SENSE_2);
    // IO_Expander_SetPinInput(IO_EXP_PIN_SPARE_INPUT_1); // Repurposed for Pressure Washer Output
    IO_Expander_SetPinInput(IO_EXP_PIN_SPARE_INPUT_2);

    // Keypad DISABLED - pins 0,1,2 used for wash buttons
    // Keypad driver configuration removed

    IO_EXP_CTRL_CRITICAL("[✓] I/O Expander Control initialized");

    return IO_EXP_CTRL_OK;
}

/**
 * @brief Start I/O Expander Control polling task
 */
void Task_Start_IO_Expander_Control_Task(void)
{
    if (s_task_handle != NULL) {
        IO_EXP_LOG("Task already running");
        return;
    }

    BaseType_t result = xTaskCreate(
        IO_Expander_Control_Task,
        "IO_Expander_Task",
        IO_EXPANDER_TASK_STACK_WORDS,
        NULL,
        IO_EXPANDER_TASK_PRIORITY,
        &s_task_handle
    );

    if (result == pdPASS) {
        IO_EXP_CTRL_CRITICAL("[✓] I/O Expander Control task started");
    } else {
        IO_EXP_CTRL_ERROR("[✗] Failed to create I/O Expander Control task");
    }
}

/**
 * @brief Get cached state of an input pin
 */
IO_Expander_Control_Status_t IO_Expander_GetPinState(uint8_t pin, IO_Expander_State_t *state)
{
    if (!s_initialized) {
        return IO_EXP_CTRL_ERROR_NOT_INITIALIZED;
    }

    if (pin >= IO_EXP_NUM_PINS || state == NULL) {
        return IO_EXP_CTRL_ERROR_INVALID_PIN;
    }

    // Return cached state from polling task (thread-safe)
    *state = s_pin_states[pin];
    return IO_EXP_CTRL_OK;
}

/**
 * @brief Check if user button is currently pressed
 */
bool IO_Expander_IsUserButtonPressed(void)
{
    return s_button_pressed;
}

/**
 * @brief Control main relay state
 */
IO_Expander_Control_Status_t IO_Expander_SetMainRelay(bool enable)
{
    IO_Expander_State_t state = enable ? IO_EXP_STATE_HIGH : IO_EXP_STATE_LOW;
    // Main relay now on pin 3 (was pin 0, conflicted with wash button)
    return IO_Expander_WritePin(IO_EXP_PIN_KEYPAD_ROW_2, state);
}

/**
 * @brief Control status LED state
 */
IO_Expander_Control_Status_t IO_Expander_SetStatusLED(bool enable)
{
    IO_Expander_State_t state = enable ? IO_EXP_STATE_HIGH : IO_EXP_STATE_LOW;
    return IO_Expander_WritePin(IO_EXP_PIN_STATUS_LED, state);
}

/**
 * @brief Configure a pin as output
 */
IO_Expander_Control_Status_t IO_Expander_SetPinOutput(uint8_t pin)
{
    if (!s_initialized) {
        return IO_EXP_CTRL_ERROR_NOT_INITIALIZED;
    }

    if (pin > CAT9555_PIN_15) {
        return IO_EXP_CTRL_ERROR_INVALID_PIN;
    }

    CAT9555_PinConfig_t config = {
        .pin = pin,
        .direction = CAT9555_PIN_OUTPUT
    };

    CAT9555_Status_t status = CAT9555_ConfigurePin(s_io_expander_handle, &config);
    return convert_cat9555_status(status);
}

/**
 * @brief Configure a pin as input
 */
IO_Expander_Control_Status_t IO_Expander_SetPinInput(uint8_t pin)
{
    if (!s_initialized) {
        return IO_EXP_CTRL_ERROR_NOT_INITIALIZED;
    }

    if (pin > CAT9555_PIN_15) {
        return IO_EXP_CTRL_ERROR_INVALID_PIN;
    }

    CAT9555_PinConfig_t config = {
        .pin = pin,
        .direction = CAT9555_PIN_INPUT
    };

    CAT9555_Status_t status = CAT9555_ConfigurePin(s_io_expander_handle, &config);
    return convert_cat9555_status(status);
}

/**
 * @brief Set output pin state
 */
IO_Expander_Control_Status_t IO_Expander_WritePin(uint8_t pin, IO_Expander_State_t state)
{
    if (!s_initialized) {
        return IO_EXP_CTRL_ERROR_NOT_INITIALIZED;
    }

    if (pin > CAT9555_PIN_15) {
        return IO_EXP_CTRL_ERROR_INVALID_PIN;
    }

    uint8_t cat_state = (state == IO_EXP_STATE_HIGH) ? 
                        CAT9555_PIN_HIGH : CAT9555_PIN_LOW;

    CAT9555_Status_t status = CAT9555_WritePin(s_io_expander_handle, pin, cat_state);
    return convert_cat9555_status(status);
}

/**
 * @brief Read input pin state
 */
IO_Expander_Control_Status_t IO_Expander_ReadPin(uint8_t pin, IO_Expander_State_t *state)
{
    if (!s_initialized) {
        return IO_EXP_CTRL_ERROR_NOT_INITIALIZED;
    }

    if (pin > CAT9555_PIN_15 || state == NULL) {
        return IO_EXP_CTRL_ERROR_INVALID_PIN;
    }

    uint8_t cat_state;
    CAT9555_Status_t status = CAT9555_ReadPin(s_io_expander_handle, pin, &cat_state);
    
    if (status == CAT9555_OK) {
        *state = (cat_state == CAT9555_PIN_HIGH) ? IO_EXP_STATE_HIGH : IO_EXP_STATE_LOW;
    }

    return convert_cat9555_status(status);
}

/**
 * @brief Toggle output pin state
 */
IO_Expander_Control_Status_t IO_Expander_TogglePin(uint8_t pin)
{
    if (!s_initialized) {
        return IO_EXP_CTRL_ERROR_NOT_INITIALIZED;
    }

    if (pin > CAT9555_PIN_15) {
        return IO_EXP_CTRL_ERROR_INVALID_PIN;
    }

    // Read current state
    IO_Expander_State_t current_state;
    IO_Expander_Control_Status_t result = IO_Expander_ReadPin(pin, &current_state);
    if (result != IO_EXP_CTRL_OK) {
        return result;
    }

    // Toggle state
    IO_Expander_State_t new_state = (current_state == IO_EXP_STATE_HIGH) ? 
                                      IO_EXP_STATE_LOW : IO_EXP_STATE_HIGH;

    return IO_Expander_WritePin(pin, new_state);
}

/**
 * @brief Get status string for logging
 */
const char* IO_Expander_Control_GetStatusString(IO_Expander_Control_Status_t status)
{
    switch (status) {
        case IO_EXP_CTRL_OK:                      return "OK";
        case IO_EXP_CTRL_ERROR:                   return "Error";
        case IO_EXP_CTRL_ERROR_NOT_INITIALIZED:   return "Not Initialized";
        case IO_EXP_CTRL_ERROR_INVALID_PIN:       return "Invalid Pin";
        case IO_EXP_CTRL_ERROR_HARDWARE:          return "Hardware Error";
        default:                                  return "Unknown";
    }
}

/*Private Functions --------------------------------------------------*/

/**
 * @brief I/O Expander Control FreeRTOS task
 * @details Polls all input pins at fixed rate, handles debouncing, caches states
 */
static void IO_Expander_Control_Task(void* argument)
{
    (void)argument;

    IO_EXP_LOG("[→] I/O Expander Control task running");

    while (1) {
        // Feed watchdog every second - MUST be first in loop
        TASK_HEARTBEAT_EVERY_SECOND("IO_Expander_Task");
        System_ReportTaskStatus(SYSTEM_TASK_ID_IO_EXPANDER, true);

        // Poll all input pins
        poll_all_inputs();

        // Delay for next poll cycle
        vTaskDelay(pdMS_TO_TICKS(IO_EXP_POLL_RATE_MS));
    }
}

/**
 * @brief Poll all input pins and update cached states
 */
static void poll_all_inputs(void)
{
    // Read all input pins and cache their states
    for (uint8_t pin = 0; pin < IO_EXP_NUM_PINS; pin++) {
        uint8_t cat_state;
        CAT9555_Status_t status = CAT9555_ReadPin(s_io_expander_handle, pin, &cat_state);
        
        if (status == CAT9555_OK) {
            s_pin_states[pin] = (cat_state == CAT9555_PIN_HIGH) ? IO_EXP_STATE_HIGH : IO_EXP_STATE_LOW;
        }
    }

    // Handle button debouncing
    IO_Expander_State_t raw_button_state = s_pin_states[IO_EXP_PIN_USER_BUTTON];
    handle_button_debounce(raw_button_state);
}

/**
 * @brief Handle button debouncing logic
 */
static void handle_button_debounce(IO_Expander_State_t raw_button_state)
{
    // Button is active low (pressed = LOW)
    bool raw_pressed = (raw_button_state == IO_EXP_STATE_LOW);
    
    if (raw_pressed == s_button_pressed) {
        // State stable - reset debounce counter
        s_button_debounce_count = 0;
    } else {
        // State changed - increment counter
        s_button_debounce_count++;
        
        if (s_button_debounce_count >= IO_EXP_DEBOUNCE_COUNT) {
            // State confirmed - update and log
            s_button_pressed = raw_pressed;
            s_button_debounce_count = 0;
            
            if (s_button_pressed) {
                IO_EXP_LOG("User button PRESSED");
            } else {
                IO_EXP_LOG("User button RELEASED");
            }
        }
    }
}

/**
 * @brief Convert CAT9555 status to IO_Expander_Control status
 */
static IO_Expander_Control_Status_t convert_cat9555_status(CAT9555_Status_t cat_status)
{
    switch (cat_status) {
        case CAT9555_OK:
            return IO_EXP_CTRL_OK;
        case CAT9555_ERROR_NOT_INITIALIZED:
            return IO_EXP_CTRL_ERROR_NOT_INITIALIZED;
        case CAT9555_ERROR_INVALID_PARAM:
            return IO_EXP_CTRL_ERROR_INVALID_PIN;
        default:
            return IO_EXP_CTRL_ERROR_HARDWARE;
    }
}
