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
#include "LCD_Display_Driver.h"

/* Private includes ----------------------------------------------------------*/
#include "System.h"
#include "PN532_Driver.h"
#include "MIFARE_Transaction_Manager.h"
#include "Hardware_Access.h"
#include "YS_S201_Driver.h"
#include "CAT9555_Driver.h"
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
// #define DISPENSE_ON_BUTTON_PRESS

/* Private typedefs -----------------------------------------------------------*/
//static QueueHandle_t IO_Message_Queue_QueueHandle = NULL;
static QueueHandle_t PICC_Message_Queue_QueueHandle = NULL;
static QueueHandle_t SPI_TX_Message_Queue_QueueHandle = NULL;
static QueueHandle_t SPI_RX_Message_Queue_QueueHandle = NULL;
static QueueHandle_t DISPLAY_Message_Queue_QueueHandle = NULL;

static QueueHandle_t IO_Message_Queue_Pointers_QueueHandle = NULL;

static TaskHandle_t System_Task_TaskHandle;

SemaphoreHandle_t gpio_semaphore;
SemaphoreHandle_t i2c_semaphore;
SemaphoreHandle_t i2c_1_Semaphore;  /* Platform-specific I2C1 bus semaphore */
SemaphoreHandle_t mux_semaphore;

SemaphoreHandle_t spi_1_Semaphore;
SemaphoreHandle_t spi_2_Semaphore; /* New semaphore for SPI2 bus serialization */

/* System I/O collection - global array for hardware abstraction */
IO_Def system_io_collection[30];

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
static void process_mifare_polling(void);
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
				USB_Log_Printf("ERROR: Unknown state %d\r\n", system_context.current_state);
				change_state(SYSTEM_STATE_ERROR);
				break;
		}
		
		// Run every 10ms
		vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(10));
	}
}


static void systemInitialisations()
{
	/* Create semaphores first - BEFORE any I2C operations */
	gpio_semaphore = xSemaphoreCreateMutex();
	i2c_semaphore = xSemaphoreCreateMutex();
	i2c_1_Semaphore = xSemaphoreCreateMutex();
	mux_semaphore = xSemaphoreCreateMutex();
	spi_1_Semaphore = xSemaphoreCreateMutex();
	spi_2_Semaphore = xSemaphoreCreateMutex();

	/* Initialize I2C protection layer now that semaphores exist */
	Hardware_I2C_Init();

	USB_CDC_Task_Start(); 
	
	// Check if semaphore creation succeeded
	
	/* This MUST be done before any device enables GPIO interrupts */
	Hardware_Init_GPIO_Interrupts();
	
	/* Initialize CAT9555 I/O Expander and register its interrupt callback */
	//CAT9555_Init(&cat9555_handle, CAT9555_I2C_ADDRESS);
	//Hardware_CAT9555_Init_Interrupt_GPIO(&cat9555_interrupt_callback);

	/* Initialize PN532 Driver */
	if (PN532_Init() != PN532_STATUS_OK) {
		USB_Log_Printf("WARNING: PN532 Init failed, continuing without NFC\r\n");
		pn532_initialized = false;
	} else {
		USB_Log_Printf("PN532: Initialized successfully\r\n");
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

	// Tasks will handle notifications when ready
	
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
	
	//picc_comm_start_task();
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
		// Commented out to reduce log verbosity
		// USB_Log_Printf("[STATE] %d -> %d (time: %lu ms)\r\n", 
		//                system_context.current_state, new_state, system_context.time_in_state);
		
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
		USB_Log_Printf("[STATE] Startup complete, initializing hardware...\r\n");
		change_state(SYSTEM_STATE_INITIALIZING);
	}
}

/**
 * @brief INITIALIZING state - Perform all hardware initialization
 */
static void state_initializing(void)
{
	systemInitialisations();
	USB_Log_Printf("[STATE] Initialization complete, entering idle state\r\n");
	change_state(SYSTEM_STATE_IDLE);
}

/**
 * @brief IDLE state - Normal operation, waiting for events
 */
/**
 * @brief Helper function to handle MIFARE card polling logic
 */
static void process_mifare_polling(void)
{
	// Skip PN532 polling if hardware was not successfully initialized
	if (!pn532_initialized) {
		return;
	}
	
	// Poll for MIFARE cards (every cycle = 10ms)
	static uint32_t card_poll_counter = 0;
	static MIFARE_CardState_t last_card_state = MIFARE_CARD_STATE_ABSENT;
	
	card_poll_counter++;
	
	// Poll for cards every 30ms (3 cycles) - optimized for faster detection
	if (card_poll_counter < 3) {
		return;
	}
	card_poll_counter = 0;
	
	MIFARE_CardState_t current_card_state = MIFARE_GetCardState();
	MIFARE_DispenseState_t current_dispense_state = MIFARE_GetDispenseState();
	
	// Only log significant state changes, not every poll cycle
	
	// Detect transition to PRESENT state for auto-dispensing
	if (current_card_state == MIFARE_CARD_STATE_PRESENT && last_card_state != MIFARE_CARD_STATE_PRESENT) {
		#ifndef DISPENSE_ON_BUTTON_PRESS
		// USB_Log_Printf("SYSTEM: Card ready (Auto-Dispense Mode) - Triggering dispense logic\r\n");
		// process_dispensing_logic();
		#endif
	}
	last_card_state = current_card_state;
	
	// CRITICAL: Once card is present and confirmed, STOP polling entirely!
	// The MIFARE layer will monitor presence via write/read operations.
	if (current_card_state == MIFARE_CARD_STATE_PRESENT) {
		// ONLY stop polling if a transaction is active (MIFARE layer handles presence)
		// If idle, we MUST poll to detect removal
		if (g_transaction_manager.transaction_active) {
			// Card is fully present AND transaction active - don't poll
			// Reset any removal tracking since we're not polling
			system_context.card_detection_first_failed_tick = 0;
			return;
		}
		// Card present but idle - continue polling for removal (no log spam)
	}
	
	// If in ERROR state, check if we're still in cooldown before retrying
	if (current_card_state == MIFARE_CARD_STATE_ERROR) {
		// Error state detected - don't spam retries, wait for cooldown in MIFARE layer
		// MIFARE_ProcessCardDetected will check pn532_recovery_until_tick internally
		// Just continue with polling - MIFARE layer will handle cooldown
	}
	
	// Card is absent, needs polling cycle, or in error - poll to detect/confirm card
	PN532_CardInfo_t card_info;
	PN532_Status_t status = PN532_DetectCard(&card_info);
	// Only log when card state changes (removed repetitive poll logging)
	
	if (status == PN532_STATUS_CARD_DETECTED) {
		// Card detected - only log significant events
		if (current_card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
			// Card was waiting for polling confirmation - now transition to PRESENT
			USB_Log_Printf("[SYSTEM POLL] Card confirmed after polling cycle, setting to PRESENT\r\n");
			// MIFARE_SetCardState(MIFARE_CARD_STATE_PRESENT); // Handled by ConfirmReadyAfterPolling
			MIFARE_ConfirmReadyAfterPolling();
		} else if (current_card_state == MIFARE_CARD_STATE_PRESENT) {
			// Card still present (polling in idle mode) - do nothing
			USB_Log_Printf("[SYSTEM POLL] Card still PRESENT (idle polling)\r\n");
		} else {
			// New card detected or retry after error!
			if (current_dispense_state == DISPENSE_STATE_ERROR) {
				USB_Log_Printf("SYSTEM: Card detected in error state, attempting recovery\r\n");
			} else {
				USB_Log_Printf("SYSTEM: New card detected, notifying MIFARE manager\r\n");
			}
			MIFARE_ProcessCardDetected(&card_info);
		}
	} else {
		// No card detected - only log when card was previously present
		if (current_card_state == MIFARE_CARD_STATE_PRESENT) {
			USB_Log_Printf("[SYSTEM POLL] Card REMOVED (detected by polling)\r\n");
			MIFARE_ProcessCardRemoved();
		} else if (current_card_state == MIFARE_CARD_STATE_NEEDS_POLLING_CYCLE) {
			// Card was waiting for polling confirmation but not detected - may have been removed
			USB_Log_Printf("SYSTEM: Card in NEEDS_POLLING_CYCLE but not detected - resetting to ABSENT\r\n");
			MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
		} else if (current_card_state == MIFARE_CARD_STATE_ERROR && status != PN532_STATUS_CARD_DETECTED) {
			// Error state but no card detected - reset to ABSENT to clear error
			static uint32_t error_no_card_counter = 0;
			error_no_card_counter++;
			if (error_no_card_counter >= 10) {  // After 1 second (10 * 100ms)
				USB_Log_Printf("SYSTEM: No card detected in error state, resetting to ABSENT\r\n");
				MIFARE_SetCardState(MIFARE_CARD_STATE_ABSENT);
				error_no_card_counter = 0;
			}
		}
	}
	// If no card detected and state is ABSENT, just continue polling in next cycle
}

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
	USB_Log_Printf("CAT9555 interrupt notification received! Total count: %lu\r\n", cat9555_interrupt_count);
	
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
		USB_Log_Printf("[CAT9555] Interrupt count: %lu (delta: %lu)\r\n", 
		               current_count, current_count - system_context.last_interrupt_count);
		system_context.last_interrupt_count = current_count;
	}
	
#if defined(PICO_BUILD) || defined(PICO_BOARD)
	// Commented out to reduce log verbosity
	// bool pin_state = gpio_get(EXP_INTR_PIN);
	// USB_Log_Printf("[CAT9555] INT pin state: %s\r\n", pin_state ? "HIGH (idle)" : "LOW (active)");
#endif
	
	// Return to idle state
	change_state(SYSTEM_STATE_IDLE);
}

/**
 * @brief ERROR state - Handle system errors
 */
static void state_error(void)
{
	USB_Log_Printf("[ERROR] System in error state\r\n");
	
	// For now, just return to idle after 1 second
	if (system_context.time_in_state >= pdMS_TO_TICKS(1000)) {
		USB_Log_Printf("[ERROR] Attempting recovery...\r\n");
		change_state(SYSTEM_STATE_IDLE);
	}
}

/**
 * @brief SHUTDOWN state - Graceful system shutdown
 */
static void state_shutdown(void)
{
	USB_Log_Printf("[STATE] System shutdown requested\r\n");
	
	// Perform cleanup operations
	// ...
	
	// Suspend task
	vTaskSuspend(NULL);
}

/**
* @brief GPIO interrupt callback for CAT9555 interrupt pin
* @details Called from the central Hardware_GPIO_Central_Dispatcher when CAT9555 INT pin triggers
*          Uses CAT9555 driver's interrupt notification mechanism (interrupt-driven mode)
*/
static void cat9555_interrupt_callback(uint gpio, uint32_t events)
{
	cat9555_interrupt_count++;  // Debug: track ISR calls
	
	// CAT9555 INT pin is active low - trigger on falling edge
	if (events & GPIO_IRQ_EDGE_FALL) {
		// Notify via CAT9555 driver's interrupt handler (interrupt-driven mode)
		CAT9555_ISR_Notify(&cat9555_handle);
	}
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
	if (send_event_gpio_pin(pin_id, pin_state, bank_id, source) != pdTRUE) 
	{
	}
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
	
	USB_Log_Printf("Dispense Trigger #%u - Card Balance: %u mL\r\n", button_press_count, current_balance);
	
	if (current_balance < amount_to_dispense) {
		USB_Log_Printf("Insufficient balance to dispense %u mL\r\n", amount_to_dispense);
		return;
	}
	
	// Perform a real transaction
	if (MIFARE_BeginTransaction(amount_to_dispense) == MIFARE_RESULT_OK) {
		// Simulate the dispensing process (instantaneous for this test)
		MIFARE_UpdateTransactionProgress(amount_to_dispense, 10.0f); // Simulate 10 LPM flow
		
		// Commit the transaction to the card
		if (MIFARE_CommitTransaction() == MIFARE_RESULT_OK) {
			// Update UI with new real values
			current_balance = MIFARE_GetBalanceML();
			USB_Log_Printf("Dispense successful. New Balance: %u mL\r\n", current_balance);
			
			/* Update the remaining bar (Scale to 1000L / 1,000,000 mL max capacity) */
            uint8_t percentage = 0;
            uint32_t max_capacity = 1000000;
            if (max_capacity > 0) {
                if (current_balance >= max_capacity) {
                    percentage = 100;
                } else {
                    percentage = (uint8_t)((current_balance * 100) / max_capacity);
                }
            }
            ui_set_bar_value(ui_totalRemainingBar, percentage, LV_ANIM_ON);
            
            // Update level color indicator
            if (ui_levelColourIndicator != NULL) {
                if (percentage > 25) {
                    ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0x05820A), LV_PART_MAIN | LV_STATE_DEFAULT);
                } else if (percentage > 10) {
                    ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFFA500), LV_PART_MAIN | LV_STATE_DEFAULT);
                } else {
                    ui_set_obj_style_bg_color(ui_levelColourIndicator, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
			
			/* Update the text box with actual remaining value */
            static char balance_str[16];
            if (current_balance > 9000) {
                uint32_t liters = current_balance / 1000;
                snprintf(balance_str, sizeof(balance_str), "%luL", liters);
            } else {
                snprintf(balance_str, sizeof(balance_str), "%luml", current_balance);
            }
            ui_set_label_text(ui_cardRemaining, balance_str);
			
			/* Update the dispensed session display */
            static char dispensed_str[16];
            uint32_t liters = amount_to_dispense / 1000;
            uint32_t decimal = (amount_to_dispense % 1000) / 100;
            snprintf(dispensed_str, sizeof(dispensed_str), "%lu.%luL", liters, decimal);
            ui_set_label_text(ui_dispensedSession, dispensed_str);
		} else {
			USB_Log_Printf("Transaction commit failed!\r\n");
		}
	} else {
		USB_Log_Printf("Failed to begin transaction (Busy or Error)\r\n");
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
					USB_Log_Printf("User Button (Pin %d) state changed: %s -> %s (0x%02X)\r\n", 
					               IO_PIN_USER_BUTTON, 
					               last_stable_state ? "HIGH" : "LOW",
					               current_raw_state ? "HIGH" : "LOW", 
					               current_raw_state);
					
					/* Send GPIO event to display task */
					send_gpio_event(IO_PIN_USER_BUTTON, current_raw_state, 1, EVENT_SOURCE_CAT9555_PIN);
					
					/* Update button state display */
					if (current_raw_state == 1) {
                        ui_set_label_text(ui_buttonState, "HIGH");
                    } else {
                        ui_set_label_text(ui_buttonState, "LOW");
                    }
					
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
			USB_Log_Printf("ERROR: Failed to read User Button (Pin %d) state: %s\r\n", 
			               IO_PIN_USER_BUTTON, CAT9555_GetStatusString(status));
			error_counter = 0;
		}
	}
	
	#undef DEBOUNCE_COUNT
}


void Task_Start_System_Task()
{
	IO_Message_Queue_Pointers_QueueHandle = xQueueCreate(10, sizeof(uint32_t));
	PICC_Message_Queue_QueueHandle = xQueueCreate(1, sizeof(PICC_MSG_Def));
	SPI_TX_Message_Queue_QueueHandle = xQueueCreate(1, sizeof(uint32_t));
	SPI_RX_Message_Queue_QueueHandle = xQueueCreate(1, sizeof(uint32_t));
	DISPLAY_Message_Queue_QueueHandle = xQueueCreate(1, sizeof(DISPLAY_MSG_Def));
	
	xTaskCreate(System_Task, "System Task", SYSTEM_TASK_STACK_WORDS, NULL, SYSTEM_TASK_PRIORITY, &System_Task_TaskHandle);
}





QueueHandle_t get_msg_queue_io()
{
	return IO_Message_Queue_Pointers_QueueHandle;
}
QueueHandle_t get_msg_queue_spi_tx()
{
	return SPI_TX_Message_Queue_QueueHandle;
}
QueueHandle_t get_msg_queue_picc()
{
	return PICC_Message_Queue_QueueHandle;
}
QueueHandle_t get_msg_queue_spi_rx()
{
	return SPI_RX_Message_Queue_QueueHandle;
}
QueueHandle_t get_msg_queue_display()
{
	return DISPLAY_Message_Queue_QueueHandle;
}

/* ========================================================================== */
/*                         EVENT UTILITY FUNCTIONS                           */
/* ========================================================================== */

/**
* @brief Send a GPIO pin state change event
*/
BaseType_t send_event_gpio_pin(uint8_t pin_id, uint8_t pin_state, uint8_t bank_id, EVENT_SOURCE_Enum source)
{
	// TODO: Implement event sending logic
	return pdFALSE;
}

/**
* @brief Send a sensor reading event
*/
BaseType_t send_event_sensor(uint8_t sensor_id, uint8_t sensor_type, uint32_t sensor_value, uint8_t sensor_status, EVENT_SOURCE_Enum source)
{
	// TODO: Implement event sending logic
	return pdFALSE;
}

/**
* @brief Send a UI update event
*/
BaseType_t send_event_ui_update(uint8_t element_id, uint8_t action, uint32_t value, uint32_t color, EVENT_SOURCE_Enum source)
{
	// TODO: Implement event sending logic
	return pdFALSE;
}

/**
* @brief Send a system state change event
*/
BaseType_t send_event_system_state(uint8_t component, uint8_t state, uint16_t error_code, uint32_t additional_info, EVENT_SOURCE_Enum source)
{
	// TODO: Implement event sending logic
	return pdFALSE;
}

/**
* @brief Send a user input event
*/
BaseType_t send_event_user_input(uint8_t input_id, uint8_t input_type, uint8_t input_action, uint32_t input_duration, EVENT_SOURCE_Enum source)
{
	// TODO: Implement event sending logic
	return pdFALSE;
}

/**
* @brief Send an RFID/PICC event (backwards compatibility)
*/
BaseType_t send_event_rfid_picc(uint8_t picc_position, uint8_t picc_state, uint32_t card_uid, EVENT_SOURCE_Enum source)
{
	// TODO: Implement event sending logic
	return pdFALSE;
}


