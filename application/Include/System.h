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
#include "System_Core.h"
#include "MIFARE_Transaction_Core.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "Hardware_Access.h"
#include "RFID_RC522_Driver.h"
#include "MyWota_ui_driver.h"
#include "Buzzer_Driver.h"





/*Typedefs -----------------------------------------------------------*/

/* Watchdog Task IDs - Application-specific tasks (System Core uses 0-9) */
/* System Core task IDs (from System_Core.h):
 * SYS_TASK_ID_SD_LOGGER = 0
 * SYS_TASK_ID_USB_CDC = 1
 * SYS_TASK_ID_USB_COMMAND = 2
 * SYS_TASK_ID_RTC = 3
 * SYS_TASK_ID_RS485 = 4
 */

#define SYSTEM_TASK_ID_LCD_DISPLAY          (SYS_TASK_ID_APP_START + 0)
#define SYSTEM_TASK_ID_DISPENSER            (SYS_TASK_ID_APP_START + 1)
#define SYSTEM_TASK_ID_BUZZER_POLLING       (SYS_TASK_ID_APP_START + 2)
#define SYSTEM_TASK_ID_MIFARE_POLLING       (SYS_TASK_ID_APP_START + 3)
#define SYSTEM_TASK_ID_IO_EXPANDER          (SYS_TASK_ID_APP_START + 4)
#define SYSTEM_TASK_ID_RS485                SYS_TASK_ID_RS485  /* Use shared core ID */

/* Backward compatibility for shared drivers that use old names */
#define SYSTEM_TASK_ID_SD_LOGGER            SYS_TASK_ID_SD_LOGGER
#define SYSTEM_TASK_ID_USB_CDC              SYS_TASK_ID_USB_CDC
#define SYSTEM_TASK_ID_USB_COMMAND_HANDLER  SYS_TASK_ID_USB_COMMAND
#define SYSTEM_TASK_ID_RTC                  SYS_TASK_ID_RTC

/* WDT Log types and functions are provided by System_Core.h */

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
	SemaphoreHandle_t _gpio_sem = System_GetGpioSemaphore(); \
	if(_gpio_sem && xSemaphoreTake(_gpio_sem, portMAX_DELAY)==pdTRUE){ \
		system_io_collection[pin_name##_POS].requestTime = xTaskGetTickCount(); \
		system_io_collection[pin_name##_POS].rw = IO_read; \
		QueueHandle_t q = get_msg_queue_io(); \
		if(q) xQueueSend(q, &system_io_collection[pin_name##_POS], pdMS_TO_TICKS(100)); \
		xSemaphoreGive(_gpio_sem); \
		vTaskDelay(pdMS_TO_TICKS(2)); \
	} \
} while(0)

/*Function Prototypes ------------------------------------------------*/

/* Module Runtime Control - Start/Stop modules dynamically */
typedef enum {
    MODULE_LCD_DISPLAY = 0,
    MODULE_MIFARE_POLLING,
    MODULE_DISPENSER,
    MODULE_BUZZER,
    MODULE_IO_EXPANDER,
    MODULE_RS485,
    MODULE_COUNT
} System_Module_t;

typedef enum {
    MODULE_STATE_STOPPED = 0,
    MODULE_STATE_RUNNING,
    MODULE_STATE_ERROR
} Module_State_t;

/**
 * @brief Start a module at runtime
 * @param module Module to start
 * @return true if module started successfully
 */
bool System_StartModule(System_Module_t module);

/**
 * @brief Stop a module at runtime
 * @param module Module to stop
 * @return true if module stopped successfully
 */
bool System_StopModule(System_Module_t module);

/**
 * @brief Get current state of a module
 * @param module Module to query
 * @return Current module state
 */
Module_State_t System_GetModuleState(System_Module_t module);

/**
 * @brief Get module name string
 * @param module Module ID
 * @return Module name string
 */
const char* System_GetModuleName(System_Module_t module);

/**
 * @brief Print status of all modules
 */
void System_PrintModuleStatus(void);

/**
 * @brief Debug: Print raw flash sector headers for WDT log
 */
void System_DebugFlashSector(void);

/* WDT log functions are provided by System_Core.h:
 * - System_SaveWDTLogToFlash()
 * - System_LoadWDTLogFromFlash()
 * - System_PrintWDTLog()
 * - System_GetBootCount()
 */

void* getUIComponent(uint8_t type);

QueueHandle_t get_msg_queue_io();
QueueHandle_t get_msg_queue_spi_tx();
QueueHandle_t get_msg_queue_picc();
// Removed undefined get_msg_queue_display declaration



#endif /* APPLICATION_INCLUDE_SYSTEM_H_ */
