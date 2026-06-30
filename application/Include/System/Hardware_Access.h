/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 * ******************************************************************************
 */

#ifndef APPLICATION_INCLUDE_HARDWARE_ACCESS_H_
#define APPLICATION_INCLUDE_HARDWARE_ACCESS_H_

#include <stdint.h>
#include <stdbool.h>
#include "CAT9555_Driver.h"

/* I/O Expander Functional Pin Definitions - Hardware-abstracted assignments */
#define IO_PIN_RELAY_CONTROL_0         CAT9555_PIN_0   // IO0_0 - Main dispensing relay
#define IO_PIN_KEYPAD_ROW_0            CAT9555_PIN_1   // IO0_1 - Keypad Row A
#define IO_PIN_KEYPAD_ROW_1            CAT9555_PIN_2   // IO0_2 - Keypad Row B  
#define IO_PIN_KEYPAD_ROW_2            CAT9555_PIN_3   // IO0_3 - Keypad Row C
#define IO_PIN_KEYPAD_ROW_3            CAT9555_PIN_4   // IO0_4 - Keypad Row D
#define IO_PIN_KEYPAD_COL_0            CAT9555_PIN_5   // IO0_5 - Keypad Col A
#define IO_PIN_KEYPAD_COL_1            CAT9555_PIN_6   // IO0_6 - Keypad Col B
#define IO_PIN_KEYPAD_COL_2            CAT9555_PIN_7   // IO0_7 - Keypad Col C
#define IO_PIN_KEYPAD_COL_3            CAT9555_PIN_8   // IO1_0 - Keypad Col D
#define IO_PIN_RELAY_SENSE_1           CAT9555_PIN_9   // IO1_1 - Relay feedback 1
#define IO_PIN_RELAY_SENSE_2           CAT9555_PIN_10  // IO1_2 - Relay feedback 2
#define IO_PIN_RELAY_SENSE_3           CAT9555_PIN_11  // IO1_3 - Relay feedback 3
#define IO_PIN_USER_BUTTON             CAT9555_PIN_12  // IO1_4 - User interface button
#define IO_PIN_SYSTEM_STATUS_LED       CAT9555_PIN_13  // IO1_5 - System status LED
#define IO_PIN_SPARE_INPUT_1           CAT9555_PIN_14  // IO1_6 - Spare input 1
#define IO_PIN_SPARE_INPUT_2           CAT9555_PIN_15  // IO1_7 - Spare input 2

/* Direct GPIO Pin Definitions (for source files that haven't migrated to Adapter yet) */
#define I2C_0_SDA_PIN                   20
#define I2C_0_SCL_PIN                   21
#define I2C_1_SDA_PIN                   26
#define I2C_1_SCL_PIN                   27
#define PCD_RST_PIN                    14
#define RS485_DATA_EN_PIN              9
#define SYSTEM_COMM_LED_PIN            6
#define EXP_INTR_PIN                   8
#define LIGHT_SENSOR_PIN               28
#define FLOW_SENSOR_PIN                22
#define VALVE_CONTROL_PIN              15

/* I2C Addresses */
#define I2C_ADDR_PN532_NFC              0x24
#define I2C_ADDR_CAT9555_IO_EXPANDER    0x27

/* Application GPIO Pin Configuration Structure */
typedef struct {
    uint32_t system_comm_led_pin;
    uint32_t exp_intr_pin;
    uint32_t rs485_data_en_pin;
    uint32_t light_sensor_pin;
    uint32_t flow_sensor_pin;
    uint32_t valve_control_pin;
} App_GPIO_Pins_t;

/* Public API */
App_GPIO_Pins_t Get_App_GPIO_Pins(void);
void Hardware_LED_On(void);
void Hardware_LED_Off(void);

#endif /* APPLICATION_INCLUDE_HARDWARE_ACCESS_H_ */
