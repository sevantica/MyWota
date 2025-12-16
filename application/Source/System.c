/*
* @attention
*
*Copyright (c) Sevantica 2025.
* All rights reserved.
* This software is licensed under terms that can be found in the LICENSE file
* in the root directory of this software component.
* If no LICENSE file comes with this software, it is provided AS-IS.
*
******************************************************************************
*/


/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include "USB_CDC_Task.h"
#include "USB_Logging.h"
#include "lvgl.h" 
#include "ui.h"
#include "ui_Screen1.h"
#include "mywota_ui_driver.h"

/* Private includes ----------------------------------------------------------*/
#include "System.h"
#include "PN532_Driver.h"
#include "MIFARE_Transaction_Manager.h"
#include "Hardware_Access.h"
#include "YS_S201_Driver.h"
#include "CAT9555_Driver.h"
#include "SD_Logger_Task.h"
#include <stdio.h>

/* Platform-specific hardware includes */
#if defined(PICO_BUILD) || defined(PICO_BOARD)
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#endif

/* Private defines ------------------------------------------------------------*/
#define SYSTEM_CARD_REMOVAL_TIMEOUT_MS  500            // Card must be absent for 500ms before removal confirmed

/* 
 * DISPENSE MODE CONFIGURATION
 * Uncomment to require button press for dispensing.
 * Comment out to enable automatic dispensing upon card detection.
 */

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_SYSTEM_EN      1
#define LOG_CRITICAL_SYSTEM_EN   1
#define LOG_ERROR_SYSTEM_EN      1

#if LOG_DEBUG_SYSTEM_EN
    #define LOG_DEBUG_SYSTEM(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_SYSTEM(...)
#endif

#if LOG_CRITICAL_SYSTEM_EN
    #define LOG_CRITICAL_SYSTEM(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_CRITICAL_SYSTEM(...)
#endif

#if LOG_ERROR_SYSTEM_EN
    #define LOG_ERROR_SYSTEM(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_SYSTEM(...)
#endif

/* Private typedefs -----------------------------------------------------------*/

static TaskHandle_t System_Task_TaskHandle;

SemaphoreHandle_t gpio_semaphore;
SemaphoreHandle_t i2c_semaphore;
SemaphoreHandle_t i2c_1_Semaphore;  /* Platform-specific I2C1 bus semaphore */
SemaphoreHandle_t mux_semaphore;

SemaphoreHandle_t spi_1_Semaphore;
SemaphoreHandle_t spi_2_Semaphore; /* New semaphore for SPI2 bus serialization */

/* Private macros -------------------------------------------------------------*/


/*Static variables ---------------------------------------------------------*/

/* CAT9555 I/O Expander handle for pin polling */
static CAT9555_Handle_t cat9555_handle;

/* CAT9555 interrupt tracking */
static volatile uint32_t cat9555_interrupt_count = 0;

/* PN532 initialization status tracking */
static bool pn532_initialized = false;

/* NOTE: YS_S201_Handle_t flow_sensor removed - now managed by Dispenser_Control task */
/* Declaring a second handle here caused data mismatch issues */

/*Extern variables ---------------------------------------------------------*/

/* External task starter functions */
extern void Task_Start_Dispenser_Control_Task(void);
extern void Task_Start_SD_Logger_Task(void);

/*Global variables ---------------------------------------------------------*/

/* Function prototypes */
static void cat9555_interrupt_callback(uint gpio, uint32_t events);

static void System_Task(void* argument);

static void systemInitialisations();

static void poll_CAT9555_UserButton(void);

static void send_gpio_event(uint8_t pin_id, uint8_t pin_state, uint8_t bank_id, EVENT_SOURCE_Enum source);

/* Task notification index for CAT9555 interrupt */
#define CAT9555_INTERRUPT_NOTIFICATION_INDEX 0

/* System State Machine States */
typedef enum {
	SYSTEM_STATE_STARTUP,           // Initial power-on state
	SYSTEM_STATE_INITIALIZING,      // Performing hardware initialization
	SYSTEM_STATE_IDLE,              // Normal operation, waiting for events
	SYSTEM_STATE_PROCESSING_INPUT,  // Processing user input (button press)
	SYSTEM_STATE_DIAGNOSTICS,       // Running periodic diagnostics
	SYSTEM_STATE_ERROR,             // Error state
	SYSTEM_STATE_SHUTDOWN           // Graceful shutdown
} SystemState_t;

/* System State Machine Context */
typedef struct {
	SystemState_t current_state;
	SystemState_t previous_state;
	uint32_t state_entry_time;
	uint32_t time_in_state;
	uint32_t last_interrupt_count;
	uint32_t diagnostic_counter;
	bool interrupt_pending;
	TickType_t card_detection_first_failed_tick;  // Track when card detection first failed
} SystemContext_t;

static SystemContext_t system_context = {
	.current_state = SYSTEM_STATE_STARTUP,
	.previous_state = SYSTEM_STATE_STARTUP,
	.state_entry_time = 0,
	.time_in_state = 0,
	.last_interrupt_count = 0,
	.diagnostic_counter = 0,
	.interrupt_pending = false,
	.card_detection_first_failed_tick = 0
};

/* State machine function prototypes */
static void state_startup(void);
static void state_initializing(void);
static void state_idle(void);
static void state_processing_input(void);
static void state_diagnostics(void);
static void state_error(void);
static void state_shutdown(void);
static void change_state(SystemState_t new_state);
static void process_dispensing_logic(void);

static void System_Task(void* argument)
{
	TickType_t xLastWakeTime = xTaskGetTickCount();
	
	// Initialize state machine
	system_context.current_state = SYSTEM_STATE_STARTUP;
	system_context.state_entry_time = xTaskGetTickCount();
	
	for(;;)
	{
		TASK_HEARTBEAT_EVERY_SECOND("System");
		
		// Update LVGL ticker
		lv_tick_inc(10);
		
		// Update time in current state
		system_context.time_in_state = xTaskGetTickCount() - system_context.state_entry_time;
		
		// Check for CAT9555 interrupt (sets flag for state machine)
		uint32_t notification_value = 0;
		if (xTaskNotifyWaitIndexed(CAT9555_INTERRUPT_NOTIFICATION_INDEX, 
		                            0x00, 0xFFFFFFFF, 
		                            &notification_value, 
		                            0) == pdTRUE) {
			system_context.interrupt_pending = true;
		}
		
		// Execute current state
		switch (system_context.current_state) {
			case SYSTEM_STATE_STARTUP:
				state_startup();
				break;
			
			case SYSTEM_STATE_INITIALIZING:
				state_initializing();
				break;
			
			case SYSTEM_STATE_IDLE:
				state_idle();
				break;
			
			case SYSTEM_STATE_PROCESSING_INPUT:
				state_processing_input();
				break;
			
			case SYSTEM_STATE_DIAGNOSTICS:
				state_diagnostics();
				break;
			
			case SYSTEM_STATE_ERROR:
				state_error();
				break;
			
			case SYSTEM_STATE_SHUTDOWN:
				state_shutdown();
				break;
			
			default:
				LOG_ERROR_SYSTEM(": Unknown state %d\r\n", system_context.current_state);
				change_state(SYSTEM_STATE_ERROR);
				break;
		}
		
		// Run every 10ms
		vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
	}
}


static void systemInitialisations()
{

	Init_Hardware_Layer();
	

	USB_CDC_Task_Start(); 
	
	// Check if semaphore creation succeeded
	
	/* This MUST be done before any device enables GPIO interrupts */
	Hardware_Init_GPIO_Interrupts();
	
	/* Initialize PN532 Driver */
	if (PN532_Init() != PN532_STATUS_OK) {
		LOG_ERROR_SYSTEM(": PN532 Init failed, continuing without NFC\r\n");
		pn532_initialized = false;
	} else {
		LOG_CRITICAL_SYSTEM("PN532: Initialized successfully\r\n");
		pn532_initialized = true;
	}

	/* Initialize MIFARE Transaction Manager */
	MIFARE_TransactionManager_Init();
	
	/* Start MIFARE card polling task */
	MIFARE_StartPollingTask();
	
	/* Start tasks */
	Task_Start_LCD_Display_Driver_Task();
	
	/* Initialize dispenser (includes YS-S201 flow sensor initialization) */
	/* Safe to initialize now that global GPIO interrupt system is ready */
	Task_Start_Dispenser_Control_Task();
	
	/* NOTE: YS-S201 flow sensor is initialized by Dispenser_Control task */
	/* Do not initialize it here - it would create a second handle and cause data mismatch */
	
	/* Start SD Logger task for data logging */
	Task_Start_SD_Logger_Task();

	/* Tasks will handle notifications when ready */
	
	TaskHandle_t dispenser_handle = task_get_handle_Dispenser_Control_Task();
	if (dispenser_handle != NULL)
	{
		xTaskNotifyGive(dispenser_handle);
	}
	
	TaskHandle_t lcd_handle = task_get_handle_LCD_Display_Driver_Task();
	if (lcd_handle != NULL)
	{
		xTaskNotifyGive(lcd_handle); /* Start LCD task */
	}
}

/* ========================================================================== */
/*                         STATE MACHINE IMPLEMENTATION                      */
/* ========================================================================== */

/**
 * @brief Change to a new state
 * @param new_state The state to transition to
 */
static void change_state(SystemState_t new_state)
{
	if (system_context.current_state != new_state) {
		system_context.previous_state = system_context.current_state;
		system_context.current_state = new_state;
		system_context.state_entry_time = xTaskGetTickCount();
		system_context.time_in_state = 0;
	}
}

/**
 * @brief STARTUP state - Wait for system to stabilize
 */
static void state_startup(void)
{
	// Wait 2 seconds for USB CDC to initialize
	if (system_context.time_in_state >= pdMS_TO_TICKS(2000)) {
		LOG_CRITICAL_SYSTEM("[STATE] Startup complete, initializing hardware...\r\n");
		change_state(SYSTEM_STATE_INITIALIZING);
	}
}

/**
 * @brief INITIALIZING state - Perform all hardware initialization
 */
static void state_initializing(void)
{
	systemInitialisations();
	LOG_CRITICAL_SYSTEM("[STATE] Initialization complete, entering idle state\r\n");
	change_state(SYSTEM_STATE_IDLE);
}

/**
 * @brief IDLE state - Normal operation, waiting for events
 */
/**
 * @brief IDLE state - Normal operation, waiting for events
 */
static void state_idle(void)
{
	// Check if interrupt is pending
	if (system_context.interrupt_pending) {
		system_context.interrupt_pending = false;
		change_state(SYSTEM_STATE_PROCESSING_INPUT);
		return;
	}
	
	// MIFARE card polling now handled by dedicated MIFARE_Polling_Task
	// (Moved out of System task to avoid priority/starvation issues)
	
	// Increment diagnostic counter
	system_context.diagnostic_counter++;
	
	// Every 5 seconds, run diagnostics
	if (system_context.diagnostic_counter >= 500) {  // 500 * 10ms = 5 seconds
		change_state(SYSTEM_STATE_DIAGNOSTICS);
	}
}

/**
 * @brief PROCESSING_INPUT state - Handle CAT9555 interrupt
 */
static void state_processing_input(void)
{
	LOG_DEBUG_SYSTEM("CAT9555 interrupt notification received! Total count: %lu\r\n", cat9555_interrupt_count);
	
	// Refresh input cache with 5 reads with 10ms delays to debounce (50ms total)
	for (int i = 0; i < 5; i++) {
		CAT9555_RefreshInputCache(&cat9555_handle);  // I2C read updates cache
		poll_CAT9555_UserButton();  // Read from cache (no I2C)
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	
	// Return to idle state
	change_state(SYSTEM_STATE_IDLE);
}

/**
 * @brief DIAGNOSTICS state - Run periodic system diagnostics
 */
static void state_diagnostics(void)
{
	system_context.diagnostic_counter = 0;
	
	uint32_t current_count = cat9555_interrupt_count;
	if (current_count != system_context.last_interrupt_count) {
		LOG_DEBUG_SYSTEM("[CAT9555] Interrupt count: %lu (delta: %lu)\r\n", 
		               current_count, current_count - system_context.last_interrupt_count);
		system_context.last_interrupt_count = current_count;
	}
	
#if defined(PICO_BUILD) || defined(PICO_BOARD)
	/* Return to idle state */
#endif
	
	change_state(SYSTEM_STATE_IDLE);
}

/**
 * @brief ERROR state - Handle system errors
 */
static void state_error(void)
{
	LOG_DEBUG_SYSTEM("[ERROR] System in error state\r\n");
	
	if (system_context.time_in_state >= pdMS_TO_TICKS(1000)) {
		LOG_DEBUG_SYSTEM("[ERROR] Attempting recovery...\r\n");
		change_state(SYSTEM_STATE_IDLE);
	}
}

/**
 * @brief SHUTDOWN state - Graceful system shutdown
 */
static void state_shutdown(void)
{
	LOG_DEBUG_SYSTEM("[STATE] System shutdown requested\r\n");
	
	// Perform cleanup operations
	// ...
	
	// Suspend task
	vTaskSuspend(NULL);
}

/**
* @brief Send a GPIO event message to the display queue (local wrapper)
* @param pin_id GPIO pin identifier
* @param pin_state Pin state (0=LOW, 1=HIGH)
* @param bank_id Bank ID for I2C expanders (0 for direct GPIO)
* @param source Event source identifier
*/
static void send_gpio_event(uint8_t pin_id, uint8_t pin_state, uint8_t bank_id, EVENT_SOURCE_Enum source)
{
	/* Stub for future implementation */
	(void)pin_id;
	(void)pin_state;
	(void)bank_id;
	(void)source;
}

/**
 * @brief Process dispensing logic when user button is pressed
 */
static void process_dispensing_logic(void)
{
	static uint32_t button_press_count = 0;
	uint32_t amount_to_dispense = 100; // Dispense 100mL per trigger
	
	button_press_count++;
	
	// Get actual balance from the card
	uint32_t current_balance = MIFARE_GetBalanceML();
	
	LOG_DEBUG_SYSTEM("Dispense Trigger #%u - Card Balance: %u mL\r\n", button_press_count, current_balance);
	
	if (current_balance < amount_to_dispense) {
		LOG_DEBUG_SYSTEM("Insufficient balance to dispense %u mL\r\n", amount_to_dispense);
		return;
	}
	
	if (MIFARE_BeginTransaction(amount_to_dispense) == MIFARE_RESULT_OK) {
		MIFARE_UpdateTransactionProgress(amount_to_dispense, 10.0f);
		
		// Commit the transaction to the card
		if (MIFARE_CommitTransaction() == MIFARE_RESULT_OK) {
			// Get updated balance after transaction
			current_balance = MIFARE_GetBalanceML();
			LOG_DEBUG_SYSTEM("Dispense successful. New Balance: %u mL\r\n", current_balance);
			
			/* UI will automatically poll and update display - no direct UI calls needed */
		} else {
			LOG_DEBUG_SYSTEM("Transaction commit failed!\r\n");
		}
	} else {
		LOG_DEBUG_SYSTEM("Failed to begin transaction (Busy or Error)\r\n");
	}
}

/**
* @brief Poll the state of CAT9555 user button pin (interrupt-driven mode)
* @details Reads from cache (NO I2C) - cache must be refreshed via CAT9555_RefreshInputCache() first
*          Uses CAT9555_PIN_USER_BUTTON definition from Hardware_Access.h
*          Compares against cached state in the handle's current_pin_states field
*          Implements debouncing by requiring 3 consecutive stable reads
*          INTERRUPT-DRIVEN: No I2C access in this function - reads from cache only
*/
static void poll_CAT9555_UserButton(void)
{
	static uint32_t poll_counter = 0;
	static uint32_t error_counter = 0;
	static uint8_t debounce_counter = 0;
	static uint8_t debounce_state = 0xFF; /* State being debounced */
	static uint8_t last_stable_state = 0xFF; /* Last accepted stable state */
	uint8_t current_raw_state;
	uint8_t previous_state;
	
	#define DEBOUNCE_COUNT 3  /* Require 3 consecutive stable reads (30ms at 10ms intervals) */
	
	/* Extract previous state from handle's cached pin states BEFORE reading new state */
	previous_state = (cat9555_handle.current_pin_states >> IO_PIN_USER_BUTTON) & 0x01;
	
	/* Initialize last_stable_state on first run */
	if (last_stable_state == 0xFF) {
		last_stable_state = previous_state;
	}
	
	/* Read current state from cache (NO I2C - interrupt-driven mode) */
	/* Cache is updated by CAT9555_RefreshInputCache() called after interrupt */
	CAT9555_Status_t status = CAT9555_GetCachedInput(&cat9555_handle, IO_PIN_USER_BUTTON, &current_raw_state);
	
	if (status == CAT9555_OK) {
		// current_raw_state now contains the cached pin state (no I2C transaction)
		
		/* Debounce logic: require consecutive stable reads before accepting state change */
		if (current_raw_state != last_stable_state) {
			/* Potential state change detected */
			if (debounce_state == current_raw_state) {
				/* Same new state as last time - increment counter */
				debounce_counter++;
				
				if (debounce_counter >= DEBOUNCE_COUNT) {
					/* State has been stable for required count - accept the change */
					LOG_DEBUG_SYSTEM("User Button (Pin %d) state changed: %s -> %s (0x%02X)\r\n", 
					               IO_PIN_USER_BUTTON, 
					               last_stable_state ? "HIGH" : "LOW",
					               current_raw_state ? "HIGH" : "LOW", 
					               current_raw_state);
					
					/* Send GPIO event to display task */
					send_gpio_event(IO_PIN_USER_BUTTON, current_raw_state, 1, EVENT_SOURCE_CAT9555_PIN);
					
					/* UI will automatically poll button state - no direct UI call needed */
					
					/* Detect LOW to HIGH transition (button press) */
					if (last_stable_state == 0 && current_raw_state == 1) {
						#ifdef DISPENSE_ON_BUTTON_PRESS
						process_dispensing_logic();
						#endif
					}
					
					/* Update last stable state */
					last_stable_state = current_raw_state;
					
					/* Note: Cache is already updated by CAT9555_RefreshInputCache() in interrupt handler */
					
					/* Reset debounce counter */
					debounce_counter = 0;
					debounce_state = 0xFF;
				}
			} else {
				/* Different state than we were debouncing - restart debounce */
				debounce_state = current_raw_state;
				debounce_counter = 1;
			}
		} else {
			/* State matches last stable - reset debounce tracking */
			debounce_counter = 0;
			debounce_state = 0xFF;
			
			/* State unchanged - log periodically every 100 polls (approximately 1 second at 10ms intervals) */
			poll_counter++;
			if (poll_counter >= 100) {
				
				poll_counter = 0;
			}
		}
	} else {
		/* Error reading pin state */
		error_counter++;
		if (error_counter >= 50) { /* Log errors every 50 failures (approximately 500ms) */
			LOG_ERROR_SYSTEM(": Failed to read User Button (Pin %d) state: %s\r\n", 
			               IO_PIN_USER_BUTTON, CAT9555_GetStatusString(status));
			error_counter = 0;
		}
	}
	
	#undef DEBOUNCE_COUNT
}


void Task_Start_System_Task()
{

	xTaskCreate(System_Task, "System Task", SYSTEM_TASK_STACK_WORDS, NULL, SYSTEM_TASK_PRIORITY, &System_Task_TaskHandle);
}



