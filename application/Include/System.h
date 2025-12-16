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

#ifndef APPLICATION_INCLUDE_SYSTEM_H_
#define APPLICATION_INCLUDE_SYSTEM_H_

/*Includes ----------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "Hardware_Access.h"
#include "Dispenser_Control.h"
#include "RFID_RC522_Driver.h"
#include "mywota_ui_driver.h"




/*Typedefs -----------------------------------------------------------*/
typedef enum{

EXP1_I2C_A0_POS	=0,
EXP1_I2C_A1_POS,
EXP1_I2C_A2_POS,

MUX_ADD_SO_POS,
MUX_ADD_S1_POS,
MUX_ADD_S2_POS,
MUX_COM_POS,

RELAY_CONTROL_0_POS,
RELAY_CONTROL_1_POS,
RELAY_CONTROL_2_POS,
RELAY_CONTROL_3_POS,
RELAY_CONTROL_4_POS,
RELAY_CONTROL_5_POS,
RELAY_CONTROL_6_POS,
RELAY_CONTROL_7_POS,

LED_TEST_ONLY_SINK_POS,
UI_BUTTON_DISPENSER_0_POS,
UI_BUTTON_DISPENSER_4_POS,

SPI_LCD_CHIP_SELECT_POS,
SPI_LCD_DATA_COMMAND_POS,
SPI_LCD_RESET_POS,


}GPIO_PIN_POSTIONS;

/*Event message types for generalized event system - DEPRECATED (UI now polls data) */
typedef enum {
    EVENT_TYPE_RFID_PICC = 0,      // RFID/PICC card events
    EVENT_TYPE_GPIO_PIN = 1,       // GPIO pin state changes
    EVENT_TYPE_SENSOR = 2,         // Sensor readings and states
    EVENT_TYPE_SYSTEM_STATE = 4,   // System status changes
    EVENT_TYPE_USER_INPUT = 5,     // Button presses, touch events
    EVENT_TYPE_FLOW_SENSOR = 6,    // Water flow sensor events
    EVENT_TYPE_I2C_DEVICE = 7,     // I2C device status events
    EVENT_TYPE_CUSTOM = 255        // Custom application-specific events
} EVENT_TYPE_Enum;

/*Event source identifiers*/
typedef enum {
    EVENT_SOURCE_RFID_RC522 = 0,
    EVENT_SOURCE_CAT9555_PIN = 1,
    EVENT_SOURCE_PICO_GPIO = 2,
    EVENT_SOURCE_YS_S201_FLOW = 3,
    EVENT_SOURCE_UI_BUTTON = 4,
    EVENT_SOURCE_SYSTEM_TASK = 5,
    EVENT_SOURCE_LCD_TASK = 6,
    EVENT_SOURCE_CUSTOM = 255
} EVENT_SOURCE_Enum;

/*Generic event data structure using union for efficient memory usage*/
typedef union {
    /*RFID/PICC event data*/
    struct {
        uint8_t picc_position;     // PICC reader position (0-7)
        uint8_t picc_state;        // PICC state (INACTIVE/AUTH/ACTIVE)
        uint32_t card_uid;         // Card UID (first 4 bytes)
    } rfid_data;
    
    /*GPIO pin event data*/
    struct {
        uint8_t pin_id;            // Pin identifier
        uint8_t pin_state;         // Pin state (0=LOW, 1=HIGH)
        uint8_t bank_id;           // Bank ID for I2C expanders
        uint32_t debounce_time_ms; // Debounce time in milliseconds
    } gpio_data;
    
    /*Sensor event data*/
    struct {
        uint8_t sensor_id;         // Sensor identifier
        uint8_t sensor_type;       // Sensor type (flow, temperature, etc.)
        uint32_t sensor_value;     // Sensor reading value
        uint8_t sensor_status;     // Sensor status flags
    } sensor_data;
    
    /*System state event data*/
    struct {
        uint8_t system_component;  // System component ID
        uint8_t system_state;      // New system state
        uint16_t error_code;       // Error code if applicable
        uint32_t additional_info;  // Additional context information
    } system_data;
    
    /*User input event data*/
    struct {
        uint8_t input_id;          // Input source identifier
        uint8_t input_type;        // Input type (button, touch, etc.)
        uint8_t input_action;      // Action (press, release, hold)
        uint32_t input_duration;   // Duration of input in milliseconds
    } user_input_data;
    
    /*Raw data for custom events*/
    struct {
        uint8_t data[16];          // Raw data bytes for custom events
    } raw_data;
} EVENT_DATA_Union;

/*Generalized event message structure*/
typedef struct {
    uint64_t message_id;           // Unique message identifier
    EVENT_TYPE_Enum event_type;    // Type of event
    EVENT_SOURCE_Enum event_source; // Source that generated the event
    uint8_t priority;              // Event priority (0=low, 255=critical)
    EVENT_DATA_Union event_data;   // Event-specific data
    uint64_t timestamp;            // Event timestamp
} DISPLAY_MSG_Def;

/*Legacy typedef for backwards compatibility*/
typedef DISPLAY_MSG_Def EVENT_MSG_Def;




/*Defines ------------------------------------------------------------*/
#define 	TOUCH_BUTTON_ID		0
#define 	BUTTON_TYPE_GPIO 	0

#define 	GPIO_MOMENTARY_TOUCH_SENSITIVE_BUTTON		0
#define 	GPIO_LATCHING_TOUCH_SENSITIVE_BUTTON		1
#define 	I2C_MOMENTARY_TOUCH_SENSITIVE_BUTTON		2
#define 	I2C_LATCHING_TOUCH_SENSITIVE_BUTTON			3
#define 	I2C_MOMENTARY_PUSH_BUTTON					4
#define 	I2C_LATCHING_PUSH_BUTTON					5
#define 	I2C_MULTI_PIN_MOMENTARY_PUSH_BUTTON			5

#define		I2C_MAX_WAIT_TIME							20

/*Assigned IO */
#define		EXP1_I2C_A0									USER_GPIO_ID_x(A,4)
#define		EXP1_I2C_A1									USER_GPIO_ID_x(A,15)
#define		EXP1_I2C_A2									USER_GPIO_ID_x(B,2)

#define		MUX_ADD_SO									USER_GPIO_ID_x(C,13)
#define		MUX_ADD_S1									USER_GPIO_ID_x(C,14)
#define		MUX_ADD_S2									USER_GPIO_ID_x(C,15)
#define		MUX_COM										USER_GPIO_ID_x(A,1)

#define		RELAY_CONTROL_0								GPIO_EXP1_EXT_0
#define		RELAY_CONTROL_1								GPIO_EXP1_EXT_1
#define		RELAY_CONTROL_2								GPIO_EXP1_EXT_2
#define		RELAY_CONTROL_3								GPIO_EXP1_EXT_3
#define		RELAY_CONTROL_4								GPIO_EXP1_EXT_4
#define		RELAY_CONTROL_5								GPIO_EXP1_EXT_5
#define		RELAY_CONTROL_6								GPIO_EXP1_EXT_6
#define		RELAY_CONTROL_7								GPIO_EXP1_EXT_7

#define		UI_BUTTON_DISPENSER_0						GPIO_EXP2_EXT_0
#define		UI_BUTTON_DISPENSER_1						GPIO_EXP2_EXT_1
#define		UI_BUTTON_DISPENSER_2						GPIO_EXP2_EXT_2
#define		UI_BUTTON_DISPENSER_3						GPIO_EXP2_EXT_3
#define		UI_BUTTON_DISPENSER_4						GPIO_EXP2_EXT_4
#define		UI_BUTTON_DISPENSER_5						GPIO_EXP2_EXT_5

#define     SPI_LCD_CHIP_SELECT							GPIO_EXP2_EXT_1
#define     SPI_LCD_DATA_COMMAND						GPIO_EXP2_EXT_2
#define     SPI_LCD_RESET								GPIO_EXP2_EXT_3

#define 	LED_TEST_ONLY_SINK							USER_GPIO_ID_x(C,13)

/*Array Position definitions, DO NOT MODIFY EXISTING POSITIONS*/

/*Macros -------------------------------------------------------------*/


#define IO_ASSIGN_FUNCTION(pin_name, pin_type)			system_io_collection[pin_name##_POS].IO_ID = pin_name; \
		system_io_collection[pin_name##_POS].IO_TYPE = pin_type

#define GET_IO(pin_name)								system_io_collection[pin_name##_POS]


/* NOTE: IO_READ_STATE macro retained conceptually but adapted to FreeRTOS primitives. */
#define IO_READ_STATE(pin_name) do { \
	if(gpio_semaphore && xSemaphoreTake(gpio_semaphore, portMAX_DELAY)==pdTRUE){ \
		system_io_collection[pin_name##_POS].requestTime = xTaskGetTickCount(); \
		system_io_collection[pin_name##_POS].rw = IO_read; \
		QueueHandle_t q = get_msg_queue_io(); \
		if(q) xQueueSend(q, &system_io_collection[pin_name##_POS], pdMS_TO_TICKS(100)); \
		xSemaphoreGive(gpio_semaphore); \
		vTaskDelay(pdMS_TO_TICKS(2)); \
	} \
} while(0)

/*Extern Variables ---------------------------------------------------*/
extern SemaphoreHandle_t gpio_semaphore;
extern SemaphoreHandle_t i2c_semaphore;
extern SemaphoreHandle_t i2c_1_Semaphore;
extern SemaphoreHandle_t mux_semaphore;

void Task_Start_System_Task();

void* getUIComponent(uint8_t type);

QueueHandle_t get_msg_queue_io();
QueueHandle_t get_msg_queue_spi_tx();
QueueHandle_t get_msg_queue_picc();
// Removed undefined get_msg_queue_display declaration

/* ========================================================================== */
/*                         EVENT UTILITY FUNCTIONS - DEPRECATED             */
/* ========================================================================== */
/* NOTE: UI now uses getter functions to poll data instead of events */

/**
 * @brief Send a sensor reading event
 * @param sensor_id Sensor identifier
 * @param sensor_type Sensor type
 * @param sensor_value Sensor reading value
 * @param sensor_status Sensor status flags
 * @param source Event source identifier
 * @return pdTRUE if event sent successfully, pdFALSE otherwise
 */
BaseType_t send_event_sensor(uint8_t sensor_id, uint8_t sensor_type, uint32_t sensor_value, uint8_t sensor_status, EVENT_SOURCE_Enum source);

/**
 * @brief Send a system state change event
 * @param component System component identifier
 * @param state New system state
 * @param error_code Error code if applicable
 * @param additional_info Additional context information
 * @param source Event source identifier
 * @return pdTRUE if event sent successfully, pdFALSE otherwise
 */
BaseType_t send_event_system_state(uint8_t component, uint8_t state, uint16_t error_code, uint32_t additional_info, EVENT_SOURCE_Enum source);

/**
 * @brief Send a user input event
 * @param input_id Input source identifier
 * @param input_type Input type (button, touch, etc.)
 * @param input_action Action (press, release, hold)
 * @param input_duration Duration of input in milliseconds
 * @param source Event source identifier
 * @return pdTRUE if event sent successfully, pdFALSE otherwise
 */
BaseType_t send_event_user_input(uint8_t input_id, uint8_t input_type, uint8_t input_action, uint32_t input_duration, EVENT_SOURCE_Enum source);


#endif /* APPLICATION_INCLUDE_SYSTEM_H_ */
