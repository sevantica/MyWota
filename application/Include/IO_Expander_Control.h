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

#ifndef APPLICATION_INCLUDE_IO_EXPANDER_CONTROL_H_
#define APPLICATION_INCLUDE_IO_EXPANDER_CONTROL_H_

/**
 * @file IO_Expander_Control.h
 * @brief High-level control interface for CAT9555 I/O Expander
 * @details Provides application-level abstraction for I/O expander operations.
 *          Maps functional pin names to physical CAT9555 pins.
 */

/*Includes ----------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>
#include "CAT9555_Driver.h"

/*Defines ------------------------------------------------------------*/

/* Number of I/O expander pins */
#define IO_EXP_NUM_PINS  (16u)

/* Functional Pin Assignments - MyWota Hardware v2.0 */
#define IO_EXP_PIN_RELAY_CONTROL_0         CAT9555_PIN_0   // IO0_0 - Main relay control
#define IO_EXP_PIN_KEYPAD_ROW_0            CAT9555_PIN_1   // IO0_1 - Keypad Row A
#define IO_EXP_PIN_KEYPAD_ROW_1            CAT9555_PIN_2   // IO0_2 - Keypad Row B  
#define IO_EXP_PIN_KEYPAD_ROW_2            CAT9555_PIN_3   // IO0_3 - Keypad Row C
#define IO_EXP_PIN_KEYPAD_ROW_3            CAT9555_PIN_4   // IO0_4 - Keypad Row D
#define IO_EXP_PIN_KEYPAD_COL_0            CAT9555_PIN_5   // IO0_5 - Keypad Col A
#define IO_EXP_PIN_KEYPAD_COL_1            CAT9555_PIN_6   // IO0_6 - Keypad Col B
#define IO_EXP_PIN_KEYPAD_COL_2            CAT9555_PIN_7   // IO0_7 - Keypad Col C
#define IO_EXP_PIN_KEYPAD_COL_3            CAT9555_PIN_8   // IO1_0 - Keypad Col D
#define IO_EXP_PIN_RELAY_SENSE_1           CAT9555_PIN_9   // IO1_1 - Relay feedback 1
#define IO_EXP_PIN_RELAY_SENSE_2           CAT9555_PIN_10  // IO1_2 - Relay feedback 2
#define IO_EXP_PIN_BUZZER                  CAT9555_PIN_11  // IO1_3 - Piezo buzzer
#define IO_EXP_PIN_USER_BUTTON             CAT9555_PIN_12  // IO1_4 - User button
#define IO_EXP_PIN_SYSTEM_STATUS_LED       CAT9555_PIN_13  // IO1_5 - Status LED
#define IO_EXP_PIN_SPARE_INPUT_1           CAT9555_PIN_14  // IO1_6 - Spare input 1
#define IO_EXP_PIN_SPARE_INPUT_2           CAT9555_PIN_15  // IO1_7 - Spare input 2

/* Application Aliases */
#define IO_EXP_PIN_MAIN_RELAY              IO_EXP_PIN_RELAY_CONTROL_0
#define IO_EXP_PIN_STATUS_LED              IO_EXP_PIN_SYSTEM_STATUS_LED

/*Typedefs -----------------------------------------------------------*/

/**
 * @brief I/O Expander Control status codes
 */
typedef enum {
    IO_EXP_CTRL_OK = 0,
    IO_EXP_CTRL_ERROR,
    IO_EXP_CTRL_ERROR_NOT_INITIALIZED,
    IO_EXP_CTRL_ERROR_INVALID_PIN,
    IO_EXP_CTRL_ERROR_HARDWARE
} IO_Expander_Control_Status_t;

/**
 * @brief Pin direction configuration
 */
typedef enum {
    IO_EXP_DIR_OUTPUT = 0,
    IO_EXP_DIR_INPUT = 1
} IO_Expander_Direction_t;

/**
 * @brief Pin state (for digital I/O)
 */
typedef enum {
    IO_EXP_STATE_LOW = 0,
    IO_EXP_STATE_HIGH = 1
} IO_Expander_State_t;

/*Function Prototypes ------------------------------------------------*/

/**
 * @brief Initialize I/O Expander Control module
 * @return IO_Expander_Control_Status_t Initialization result
 */
IO_Expander_Control_Status_t IO_Expander_Control_Init(void);

/**
 * @brief Configure a pin as output
 * @param pin Pin number (use IO_EXP_PIN_* defines)
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_SetPinOutput(uint8_t pin);

/**
 * @brief Configure a pin as input
 * @param pin Pin number (use IO_EXP_PIN_* defines)
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_SetPinInput(uint8_t pin);

/**
 * @brief Set output pin state (HIGH or LOW)
 * @param pin Pin number (use IO_EXP_PIN_* defines)
 * @param state IO_EXP_STATE_HIGH or IO_EXP_STATE_LOW
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_WritePin(uint8_t pin, IO_Expander_State_t state);

/**
 * @brief Read input pin state
 * @param pin Pin number (use IO_EXP_PIN_* defines)
 * @param state Pointer to store pin state
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_ReadPin(uint8_t pin, IO_Expander_State_t *state);

/**
 * @brief Toggle output pin state
 * @param pin Pin number (use IO_EXP_PIN_* defines)
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_TogglePin(uint8_t pin);

/**
 * @brief Get status string for logging
 * @param status Status code
 * @return const char* Status string
 */
const char* IO_Expander_Control_GetStatusString(IO_Expander_Control_Status_t status);

/**
 * @brief Start I/O Expander Control polling task
 * @details Creates FreeRTOS task that polls all I/O pins at fixed rate
 */
void Task_Start_IO_Expander_Control_Task(void);

/**
 * @brief Stop I/O Expander Control polling task
 */
void Task_Stop_IO_Expander_Control_Task(void);

/**
 * @brief Get cached state of an input pin
 * @details Returns last read state from polling task (thread-safe)
 * @param pin Pin number (use IO_EXP_PIN_* defines)
 * @param state Pointer to store pin state
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_GetPinState(uint8_t pin, IO_Expander_State_t *state);

/**
 * @brief Check if user button is currently pressed
 * @return bool True if button pressed, false otherwise
 */
bool IO_Expander_IsUserButtonPressed(void);

/**
 * @brief Control main relay state
 * @param enable True to turn relay on, false to turn off
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_SetMainRelay(bool enable);

/**
 * @brief Control status LED state
 * @param enable True to turn LED on, false to turn off
 * @return IO_Expander_Control_Status_t Operation result
 */
IO_Expander_Control_Status_t IO_Expander_SetStatusLED(bool enable);

#endif /* APPLICATION_INCLUDE_IO_EXPANDER_CONTROL_H_ */
