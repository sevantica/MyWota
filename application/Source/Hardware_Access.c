/*
 * @attention
 *
 * Copyright (c) Sevantica 2025.
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
#include "semphr.h"
#include "System.h"
#include "Hardware_Access.h"
#include <string.h>  // Added for memset
#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include "PN532_Driver.h"
#include "USB_Logging.h"

#if defined(STM32F411xE)
    #include "spi.h"
	#include "main.h"
	#include "i2c.h"
#elif defined(PICO_BOARD) || defined(PICO_BUILD)  // Support both PICO_BOARD and PICO_BUILD
    #include <stdio.h>
    #include "pico/stdlib.h"
    #include "hardware/spi.h"
    #include "hardware/i2c.h"
    #include "hardware/gpio.h"
    #include "hardware/dma.h"
    #include "hardware/uart.h"
    #include "hardware/pwm.h"
    #include "pico/stdio.h"
    #include "pico/stdio_usb.h"
    #include "pico/stdio/driver.h"
    #include "pico/platform.h"
    #include "pico/util/pheap.h"
    #include "pico/binary_info.h"
    #include "pico/error.h"  /* For PICO_ERROR_TIMEOUT and PICO_ERROR_GENERIC */
    #include "hardware/irq.h"  /* For interrupt control functions */
    
    /* Define missing constants for older Pico SDK versions */
    #ifndef PICO_DEFAULT_LED_PIN
    #define PICO_DEFAULT_LED_PIN 25
    #endif
    
    /* Define GPIO constants if not already defined */
    #ifndef GPIO_IN
    #define GPIO_IN false
    #endif
    #ifndef GPIO_OUT  
    #define GPIO_OUT true
    #endif
    
    /* Include function constants */
    #include "hardware/gpio.h"
#endif

/**
 * @brief Set all Pico GPIO pins to input mode
 * This function configures all 30 available GPIO pins (0-29) on the RP2040 as inputs
 * with no pull-up or pull-down resistors enabled
 */
void Hardware_Set_All_Pins_Input(void)
{
#if defined(PICO_BOARD) || defined(PICO_BUILD)
	// RP2040 has GPIO pins 0-29 (30 pins total)
	for (uint32_t pin = 0; pin < 30; pin++) {
		// Initialize the pin
		gpio_init(pin);
		// Set direction to input
		gpio_set_dir(pin, GPIO_IN);
		// Disable pull-up and pull-down resistors
		gpio_disable_pulls(pin);
		// Set pin function to SIO (Software controlled I/O)
		gpio_set_function(pin, GPIO_FUNC_SIO);
	}
	
	USB_Log_Printf("All GPIO pins (0-29) set to input mode\r\n");
#elif defined(STM32F411xE)
	// For STM32, this would require platform-specific implementation
	// STM32 pins are organized by ports (GPIOA, GPIOB, etc.)
	USB_Log_Printf("Set all pins to input not implemented for STM32 platform\r\n");
#else
	USB_Log_Printf("Set all pins to input not implemented for this platform\r\n");
#endif
}

/**
 * @brief Set all Pico GPIO pins to input mode with pull-up resistors enabled
 * This is safer for floating inputs as it prevents undefined states
 */
void Hardware_Set_All_Pins_Input_Pullup(void)
{
#if defined(PICO_BUILD)
	// RP2040 has GPIO pins 0-29 (30 pins total)
	for (uint32_t pin = 0; pin < 30; pin++) {
		// Initialize the pin
		gpio_init(pin);
		// Set direction to input
		gpio_set_dir(pin, GPIO_IN);
		// Enable pull-up resistor
		gpio_pull_up(pin);
		// Set pin function to SIO (Software controlled I/O)
		gpio_set_function(pin, GPIO_FUNC_SIO);
	}
	
	USB_Log_Printf("All GPIO pins (0-29) set to input mode with pull-up resistors\r\n");
#elif defined(STM32F411xE)
	// For STM32, this would require platform-specific implementation
	USB_Log_Printf("Set all pins to input with pull-up not implemented for STM32 platform\r\n");
#else
	USB_Log_Printf("Set all pins to input with pull-up not implemented for this platform\r\n");
#endif
}

/**
 * @brief Set all Pico GPIO pins to input mode with pull-down resistors enabled
 */
void Hardware_Set_All_Pins_Input_Pulldown(void)
{
#if defined(PICO_BUILD)
	// RP2040 has GPIO pins 0-29 (30 pins total)
	for (uint32_t pin = 0; pin < 30; pin++) {
		// Initialize the pin
		gpio_init(pin);
		// Set direction to input
		gpio_set_dir(pin, GPIO_IN);
		// Enable pull-down resistor
		gpio_pull_down(pin);
		// Set pin function to SIO (Software controlled I/O)
		gpio_set_function(pin, GPIO_FUNC_SIO);
	}
	
	USB_Log_Printf("All GPIO pins (0-29) set to input mode with pull-down resistors\r\n");
#elif defined(STM32F411xE)
	// For STM32, this would require platform-specific implementation
	USB_Log_Printf("Set all pins to input with pull-down not implemented for STM32 platform\r\n");
#else
	USB_Log_Printf("Set all pins to input with pull-down not implemented for this platform\r\n");
#endif
}

/**
 * @brief Platform-abstracted delay function for PN532
 */
void PN532_delay_ms(uint32_t ms)
{
#if defined(STM32F411xE)
    HAL_Delay(ms);
#elif defined(PICO_BUILD)
    sleep_ms(ms);
#endif
}

/* Private includes ----------------------------------------------------------*/

/* Platform-specific data types */
#if defined(PICO_BUILD)
/* DMA channel storage for Pico - reserved for future use */
// static int dma_channel = -1;  // Commented out until needed

#endif

/* Private defines ------------------------------------------------------------*/
#define IO_EXP_REG_INPUTPORT_0      0x00
#define IO_EXP_REG_INPUTPORT_1      0x01
#define IO_EXP_REG_OUTPUTPORT_0     0x02
#define IO_EXP_REG_OUTPUTPORT_1     0x03
#define IO_EXP_REG_POLARITY_0       0x04
#define IO_EXP_REG_POLARITY_1       0x05
#define IO_EXP_REG_CONFIG_0         0x06
#define IO_EXP_REG_CONFIG_1         0x07

#define IO_EXP_REG_POS_COMMAND      0x0
#define IO_EXP_REG_POS_DATA         0x1

#define SPI_TIMEOUT_MS              100
#define I2C_TIMEOUT_MS              100

#define SPI_TIMEOUT_MS              100
#define I2C_TIMEOUT_MS              100

/* Private typedefs -----------------------------------------------------------*/
/* FreeRTOS task configuration - priorities now defined in task_stack_config.h */

/* Private macros -------------------------------------------------------------*/

/*Static variables ---------------------------------------------------------*/

static TaskHandle_t GPIO_Driver_TaskHandle;

static volatile TaskHandle_t spi1_task_to_notify_id = NULL;
static volatile TaskHandle_t spi2_task_to_notify_id = NULL;
/* Track active chip-select so we only deassert after DMA really finishes */
static volatile uint8_t spi1_cs_active = 0; /* 1 while CS asserted for SPI1 transaction */
static volatile uint8_t spi2_cs_active = 0; /* 1 while CS asserted for SPI2 transaction */
/*Extern variables ---------------------------------------------------------*/
#if defined(STM32F411xE)
extern SPI_HandleTypeDef hspi1;
extern SPI_HandleTypeDef hspi2;
extern DMA_HandleTypeDef hdma_spi1_tx; /* existing DMA handle (extend if SPI2 DMA later) */
//extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c1;
#endif

extern SemaphoreHandle_t gpio_semaphore;
extern SemaphoreHandle_t i2c_semaphore;
extern SemaphoreHandle_t mux_semaphore;
extern SemaphoreHandle_t spi_1_Semaphore; /* From System.c */
extern SemaphoreHandle_t spi_2_Semaphore; /* From System.c */

/*Global variables ---------------------------------------------------------*/
#if defined(PICO_BUILD)
// Global semaphore for PN532 I2C communication - now declared in System.c
extern SemaphoreHandle_t i2c_1_Semaphore;
#endif


// Optimization: Improved task with proper event-driven behavior
static void GPIO_Driver_Task(void *argument)
{
	// Optimization: Use event-driven approach instead of polling
	// This task can be activated when GPIO operations are needed
	for (;;)
	{
		// Wait for GPIO operations or use a larger delay to reduce CPU usage
		vTaskDelay(pdMS_TO_TICKS(100));  // Increased from 1ms to 100ms for better efficiency
		TASK_HEARTBEAT_EVERY_SECOND("GPIO");
	}
}


void gpio_driver_io(IO_Def *p_io_msg)
{
	// Optimization: Early parameter validation
	if (!p_io_msg) return;

	// Select appropriate semaphore based on IO type
	SemaphoreHandle_t selected_semaphore = NULL;
	switch (p_io_msg->IO_TYPE)
	{
	case GPIO_Type:
		selected_semaphore = gpio_semaphore;
		break;
	case I2C_Type:
		selected_semaphore = i2c_semaphore;
		break;
	case MUX_Type:
		selected_semaphore = mux_semaphore;
		break;
	default:
		return; // Unsupported IO type
	}

	// Acquire the appropriate semaphore with timeout protection
	TickType_t xTimeout = pdMS_TO_TICKS(100); // 100ms timeout to prevent deadlocks
	if (!selected_semaphore || xSemaphoreTake(selected_semaphore, xTimeout) != pdTRUE) {
		// Log timeout error for debugging
		USB_Log_Printf("Hardware_Access: Semaphore timeout for IO type %d\r\n", p_io_msg->IO_TYPE);
		return; // Failed to acquire semaphore within timeout
	}

	switch (p_io_msg->IO_TYPE)
	{
	case GPIO_Type:
	{
#if defined(STM32F411xE)
		// STM32 GPIO implementation would go here
#elif defined(PICO_BUILD)
		// Extract GPIO pin number from IO_ID
		uint32_t gpio_pin = (uint32_t)(p_io_msg->IO_ID & 0xFFFFFFFF);
		
		if (p_io_msg->rw == IO_write) {
			// Write to GPIO pin
			gpio_put(gpio_pin, p_io_msg->state ? 1 : 0);
		} else if (p_io_msg->rw == IO_read) {
			// Read from GPIO pin
			p_io_msg->state = gpio_get(gpio_pin) ? 1 : 0;
		}
#endif
		break;
	}

	case I2C_Type:
	{
		// I2C operations would go here for both platforms
		break;
	}

	default:
		break;
	}

	xSemaphoreGive(selected_semaphore);
}

// Optimization: Improved SPI driver task with better resource management
void spi_driver_transact(SPI_MSG_DEF *spi_msg)
{
	if (!spi_msg || !spi_msg->tx_data) return;
	
	SemaphoreHandle_t bus_lock = NULL;
	switch (spi_msg->spi_id) {
	case SPI_LCD: bus_lock = spi_1_Semaphore; break;
#if defined(STM32F411xE)
	case SPI_RC522: bus_lock = spi_2_Semaphore; break;
#elif defined(PICO_BUILD)
	case SPI_GENERIC: bus_lock = spi_2_Semaphore; break;
#endif
	// PN532 case removed as it now handles its own SPI transactions
	default: break;
	}
	
	// Add timeout protection to prevent deadlocks
	TickType_t xTimeout = pdMS_TO_TICKS(200); // 200ms timeout for SPI operations
	if (bus_lock && xSemaphoreTake(bus_lock, xTimeout) != pdTRUE) {
		USB_Log_Printf("SPI: Timeout acquiring semaphore for SPI_%d\r\n", spi_msg->spi_id);
		return;
	}
	/* Critical section: single transaction on bus */
	switch (spi_msg->spi_id) {
#if defined(STM32F411xE)
	case SPI_RC522:
		// STM32 SPI2 implementation for RC522 would go here
		break;
#elif defined(PICO_BUILD)
	case SPI_GENERIC:
		/* Always assert CS before transaction */
		gpio_put(RC522_CS_PIN, 0);  // Assert CS (drive low)
		spi2_cs_active = 1;
		
		if (spi_msg->rw == 0) {
			/* Write operation */
			int bytes_written = spi_write_blocking(RC522_SPI, spi_msg->tx_data, spi_msg->tx_msg_length);
			if (bytes_written != (int)spi_msg->tx_msg_length) {
				/* Error occurred during transmission */
				gpio_put(RC522_CS_PIN, 1);  // Deassert CS (drive high)
				spi2_cs_active = 0;
				break;
			}
		} else {
			/* Read operation */
			if (spi_msg->rx_data && spi_msg->rx_msg_length > 0) {
				int bytes_read = spi_read_blocking(RC522_SPI, 0, spi_msg->rx_data, spi_msg->rx_msg_length);
				if (bytes_read != (int)spi_msg->rx_msg_length) {
					/* Error occurred during reception */
					gpio_put(RC522_CS_PIN, 1);  // Deassert CS (drive high)
					spi2_cs_active = 0;
					break;
				}
			}
		}
		
		/* Always deassert CS after transaction */
		gpio_put(RC522_CS_PIN, 1);  // Deassert CS (drive high)
		spi2_cs_active = 0;

		if (bus_lock) { xSemaphoreGive(bus_lock); bus_lock = NULL; }
		break;
#endif
	case SPI_LCD:
#if defined(STM32F411xE)
		/* Always assert CS before transaction */
		HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
		spi1_cs_active = 1;
		
		if (spi_msg->rw == 0) {
			/* Write operation */
			if (HAL_SPI_Transmit(&hspi1, spi_msg->tx_data, spi_msg->tx_msg_length, SPI_TIMEOUT_MS) != HAL_OK) {
				/* Error occurred during transmission */
				HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
				spi1_cs_active = 0;
				break;
			}
		} else {
			/* Read operation */
			if (spi_msg->rx_data && spi_msg->rx_msg_length > 0) {
				if (HAL_SPI_Receive(&hspi1, spi_msg->rx_data, spi_msg->rx_msg_length, SPI_TIMEOUT_MS) != HAL_OK) {
					/* Error occurred during reception */
					HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
					spi1_cs_active = 0;
					break;
				}
			}
		}
		
		/* Always deassert CS after transaction */
		HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
		spi1_cs_active = 0;
#elif defined(PICO_BUILD)
		/* Always assert CS before transaction */
		gpio_put(LCD_CS_PIN, 0);  // Assert CS (drive low)
		spi1_cs_active = 1;
		
		if (spi_msg->rw == 0) {
			/* Write operation */
			int bytes_written = spi_write_blocking(LCD_SPI, spi_msg->tx_data, spi_msg->tx_msg_length);
			if (bytes_written != (int)spi_msg->tx_msg_length) {
				/* Error occurred during transmission */
				gpio_put(LCD_CS_PIN, 1);  // Deassert CS (drive high)
				spi1_cs_active = 0;
				break;
			}
		} else {
			/* Read operation */
			if (spi_msg->rx_data && spi_msg->rx_msg_length > 0) {
				int bytes_read = spi_read_blocking(LCD_SPI, 0, spi_msg->rx_data, spi_msg->rx_msg_length);
				if (bytes_read != (int)spi_msg->rx_msg_length) {
					/* Error occurred during reception */
					gpio_put(LCD_CS_PIN, 1);  // Deassert CS (drive high)
					spi1_cs_active = 0;
					break;
				}
			}
		}
		
		/* Always deassert CS after transaction */
		gpio_put(LCD_CS_PIN, 1);  // Deassert CS (drive high)
		spi1_cs_active = 0;
#endif

		if (bus_lock) { xSemaphoreGive(bus_lock); bus_lock = NULL; }
		break;
	default:
		if (bus_lock) { xSemaphoreGive(bus_lock); bus_lock = NULL; }
		break;
	}
}

// I2C driver transaction function
void i2c_driver_transact(I2C_MSG_DEF *i2c_msg)
{
	if (!i2c_msg || (!i2c_msg->tx_data && !i2c_msg->rx_data)) {
		if (i2c_msg) i2c_msg->status = 1; // Set error status
		return;
	}
	
	SemaphoreHandle_t bus_lock = NULL;
	switch (i2c_msg->i2c_id) {
	case I2C_PN532:
	case I2C_GENERIC:
		bus_lock = i2c_1_Semaphore;
		break;
	case I2C_SECONDARY:
		bus_lock = i2c_semaphore;  // Use general I2C semaphore for I2C2
		break;
	default:
		break;
	}
	
	// Add timeout protection for I2C semaphore
	TickType_t xTimeout = pdMS_TO_TICKS(150); // 150ms timeout for I2C operations
	if (bus_lock && xSemaphoreTake(bus_lock, xTimeout) != pdTRUE) {
		USB_Log_Printf("I2C: Timeout acquiring semaphore for I2C_%d\r\n", i2c_msg->i2c_id);
		i2c_msg->status = 1; // Set error status
		return;
	}
	
	/* Critical section: single transaction on bus */
	switch (i2c_msg->i2c_id) {
	case I2C_PN532:
	case I2C_GENERIC:
#if defined(STM32F411xE)
		// STM32 I2C implementation
		HAL_StatusTypeDef status = HAL_ERROR;
		if (i2c_msg->rw == 0) {
			/* Write operation */
			
			status = HAL_I2C_Master_Transmit(&hi2c1, i2c_msg->device_address, i2c_msg->tx_data, i2c_msg->tx_msg_length, I2C_TIMEOUT_MS);

		} else {
			/* Read operation */
			if (i2c_msg->rx_data && i2c_msg->rx_msg_length > 0) {
				status = HAL_I2C_Master_Receive(&hi2c1, i2c_msg->device_address, i2c_msg->rx_data, i2c_msg->rx_msg_length, I2C_TIMEOUT_MS);
				// Debug: Log I2C operation details
				#ifdef PN532_DEBUG
				if (status != HAL_OK) {
					// Get HAL error for debugging
					uint32_t error = HAL_I2C_GetError(&hi2c1);
					// Note: This requires USB_Logging.h to be included
					// USB_Log_Printf("I2C Read Error: HAL_Status=%d, HAL_Error=0x%lX, Addr=0x%02X, Len=%lu\r\n", 
					//               status, error, i2c_msg->device_address, i2c_msg->rx_msg_length);
				}
				#endif
			}
		}
		// Set status in message structure
		i2c_msg->status = (status == HAL_OK) ? 0 : 1;
#elif defined(PICO_BUILD)
		// Use timeout variants to prevent infinite blocking
		// Timeout: 10000us per character (10ms per byte) for PN532
		// PN532 can be VERY slow to process write commands, especially SAMConfig
		// After extensive testing, even 5ms was too short. A 25ms timeout provides
		// much greater reliability for slow devices like the PN532.
		#define I2C_TIMEOUT_PER_CHAR_US 100000
		
		int result = 0;
		if (i2c_msg->rw == 0) {
			/* Write operation with timeout */
			// Note: device_address is already in 7-bit format from the driver
			result = i2c_write_timeout_per_char_us(PN532_I2C, i2c_msg->device_address, 
			                                       i2c_msg->tx_data, i2c_msg->tx_msg_length, 
			                                       false, I2C_TIMEOUT_PER_CHAR_US);
		} else {
			/* Read operation with timeout */
			if (i2c_msg->rx_data && i2c_msg->rx_msg_length > 0) {
				// Note: device_address is already in 7-bit format from the driver
				result = i2c_read_timeout_per_char_us(PN532_I2C, i2c_msg->device_address, 
				                                      i2c_msg->rx_data, i2c_msg->rx_msg_length, 
				                                      false, I2C_TIMEOUT_PER_CHAR_US);
			}
		}
		// Set status: success if bytes transferred equal expected
		// PICO_ERROR_TIMEOUT (-1) indicates timeout occurred
		if (result == PICO_ERROR_TIMEOUT) {
			i2c_msg->status = 2; // Timeout error
		} else if (result == PICO_ERROR_GENERIC) {
			i2c_msg->status = 3; // Generic error (NACK, etc.)
		} else if (result == (int)i2c_msg->tx_msg_length || result == (int)i2c_msg->rx_msg_length) {
			i2c_msg->status = 0; // Success
		} else {
			i2c_msg->status = 1; // Other error
		}
#endif
		break;
		
	case I2C_SECONDARY:
		// I2C2 implementation (if available on platform)
		// Set default success status for now
		i2c_msg->status = 0;
		break;
		
	default:
		// Unknown I2C ID - set error status
		i2c_msg->status = 1;
		break;
		
	}
	
	if (bus_lock) { xSemaphoreGive(bus_lock); bus_lock = NULL; }
}

void Task_Start_IO_Driver(){ xTaskCreate(GPIO_Driver_Task, "GPIO Driver", GPIO_DRIVER_TASK_STACK_WORDS, NULL, GPIO_DRIVER_TASK_PRIORITY, &GPIO_Driver_TaskHandle); }
TaskHandle_t task_get_handle_GPIO_Driver_Task(){ return GPIO_Driver_TaskHandle; }

// Optimization: Enhanced io_driver_set_state with improved error handling
void io_driver_set_state(int pin_pos, int set_state, TaskHandle_t task_id) 
{
	// Optimization: Parameter validation
	if (pin_pos < 0) return;

	system_io_collection[pin_pos].requestTime = xTaskGetTickCount();
	system_io_collection[pin_pos].state = set_state;
	system_io_collection[pin_pos].RequestingTask = xTaskGetCurrentTaskHandle();
	system_io_collection[pin_pos].rw = IO_write;
	
#if defined(PICO_BUILD)
	// Initialize GPIO pin if not already done
	uint32_t gpio_pin = (uint32_t)(system_io_collection[pin_pos].IO_ID & 0xFFFFFFFF);
	if (gpio_pin < 32) {  // Valid GPIO pin range for Pico
		gpio_init(gpio_pin);
		gpio_set_dir(gpio_pin, GPIO_OUT);
	}
#endif
	
	gpio_driver_io(&system_io_collection[pin_pos]);
}

// Platform-specific DMA functions
#if defined(STM32F411xE)
HAL_StatusTypeDef spi1_dma_transmit(uint8_t *data, uint16_t size, TaskHandle_t task_id)
{
	// STM32 HAL DMA implementation would go here
	// Placeholder for now
	return HAL_OK;
}

HAL_StatusTypeDef spi2_dma_transmit(uint8_t *data, uint16_t size, TaskHandle_t task_id)
{
	// STM32 HAL DMA implementation would go here
	// Placeholder for now
	return HAL_OK;
}
#elif defined(PICO_BUILD)
// Pico SDK SPI DMA functions (placeholders for future implementation)
int spi1_dma_transmit(uint8_t *data, uint16_t size, TaskHandle_t task_id)
{
	// Placeholder for SPI0 DMA transmission
	// For now, use blocking SPI write
	if (!data || size == 0) return -1;
	
	int bytes_written = spi_write_blocking(LCD_SPI, data, size);
	return (bytes_written == size) ? 0 : -1;
}

int spi2_dma_transmit(uint8_t *data, uint16_t size, TaskHandle_t task_id)
{
	// Placeholder for SPI2 DMA transmission
	// For now, use blocking SPI write
	if (!data || size == 0) return -1;
	
	int bytes_written = spi_write_blocking(RC522_SPI, data, size);
	return (bytes_written == size) ? 0 : -1;
}
#endif

// Platform-specific hardware initialization functions
#if defined(PICO_BUILD)
// Hardware initialization function
void Hardware_Init(void)
{
	// Initialize STDIO first for printf support
	Hardware_Init_STDIO();
	
	// Initialize hardware peripherals in safe order
	// Each peripheral init function will initialize only its required pins
	Hardware_Init_LED();        // Simple GPIO first
	Hardware_Init_SPI();        // SPI peripherals
	Hardware_Init_I2C();        // I2C peripherals
	Hardware_Init_UART();       // Additional UART if needed
	Hardware_Init_USB();        // USB initialization
	Hardware_Init_PN532();      // PN532 NFC module - DISABLED
	Hardware_Init_LCD();        // LCD display (last due to complexity)
	
	// Additional hardware initialization can be added here
}

// STDIO initialization function
void Hardware_Init_STDIO(void)
{
	// STDIO should already be initialized in main() before Hardware_Init()
	// This function is kept for compatibility but does minimal work
	
	// Explicitly disable UART stdio to prevent pin conflicts
	// GPIO 0 and 1 are reserved for RC522 SPI interface
	stdio_set_driver_enabled(&stdio_uart, false);
	
	// USB stdio should already be enabled from main()
	printf("Hardware_Init_STDIO: USB logging ready\r\n");
	fflush(stdout);
}

// LED initialization function
void Hardware_Init_LED(void)
{
	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
	gpio_put(PICO_DEFAULT_LED_PIN, 0);  // Start with LED off
}

// LED control functions
void Hardware_LED_On(void)
{
	gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

void Hardware_LED_Off(void)
{
	gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

void Hardware_LED_Toggle(void)
{
	gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
}

// SPI initialization function
void Hardware_Init_SPI(void)
{
	// Initialize SPI0 for RC522 RFID module at 1MHz (safe speed)
	spi_init(RC522_SPI, 1000000);
	gpio_set_function(RC522_MISO_PIN, GPIO_FUNC_SPI);
	gpio_set_function(RC522_SCK_PIN,  GPIO_FUNC_SPI);
	gpio_set_function(RC522_MOSI_PIN, GPIO_FUNC_SPI);
	
	// RC522 CS pin as regular GPIO (manually controlled)
	gpio_init(RC522_CS_PIN);
	gpio_set_dir(RC522_CS_PIN, GPIO_OUT);
	gpio_put(RC522_CS_PIN, 1);  // CS inactive (high)
	
	// Note: SPI1 for LCD is initialized separately in Hardware_Init_LCD()
}

// I2C initialization function  
/**
 * @brief Recover stuck I2C bus by toggling clock line
 * This can help if a slave device is holding SDA low after an unexpected master reset
 */
void Hardware_I2C_BusRecovery(void)
{
#if defined(PICO_BUILD)
	USB_Log_Printf("I2C: Attempting bus recovery...\r\n");
	
	// Temporarily switch I2C pins to GPIO mode
	gpio_set_function(PN532_I2C_SDA_PIN, GPIO_FUNC_SIO);
	gpio_set_function(PN532_I2C_SCL_PIN, GPIO_FUNC_SIO);
	
	// Set both as outputs
	gpio_set_dir(PN532_I2C_SDA_PIN, GPIO_OUT);
	gpio_set_dir(PN532_I2C_SCL_PIN, GPIO_OUT);
	
	// Pull both high
	gpio_put(PN532_I2C_SDA_PIN, 1);
	gpio_put(PN532_I2C_SCL_PIN, 1);
	sleep_us(10);
	
	// Generate 9 clock pulses to clear any stuck slave
	for (int i = 0; i < 9; i++) {
		gpio_put(PN532_I2C_SCL_PIN, 0);  // Clock low
		sleep_us(5);
		gpio_put(PN532_I2C_SCL_PIN, 1);  // Clock high
		sleep_us(5);
	}
	
	// Generate STOP condition
	gpio_put(PN532_I2C_SDA_PIN, 0);  // SDA low while SCL high
	sleep_us(5);
	gpio_put(PN532_I2C_SCL_PIN, 0);  // SCL low
	sleep_us(5);
	gpio_put(PN532_I2C_SCL_PIN, 1);  // SCL high
	sleep_us(5);
	gpio_put(PN532_I2C_SDA_PIN, 1);  // SDA high (STOP)
	sleep_us(10);
	
	USB_Log_Printf("I2C: Bus recovery completed\r\n");
#endif
}

void Hardware_Init_I2C(void)
{
	// Perform I2C bus recovery BEFORE initialization
	// This clears any stuck state from previous session (e.g., after debugger reset)
	Hardware_I2C_BusRecovery();

	// I2C Initialisation. Using it at 400Khz.
	i2c_init(PN532_I2C, 400000);
	
	gpio_set_function(PN532_I2C_SDA_PIN, GPIO_FUNC_I2C);
	gpio_set_function(PN532_I2C_SCL_PIN, GPIO_FUNC_I2C);
	// Enable internal pull-ups for I2C (recommended for RP2040)
	gpio_pull_up(PN532_I2C_SDA_PIN);
	gpio_pull_up(PN532_I2C_SCL_PIN);
	
	// Give I2C and connected devices time to stabilize after recovery
	sleep_ms(50);
	
	// For more examples of I2C use see https://github.com/raspberrypi/pico-examples/tree/master/i2c
}


// UART initialization function
void Hardware_Init_UART(void)
{
	// Set up our UART
	uart_init(uart0, 115200);
	// Set the TX and RX pins by using the function select on the GPIO
	// Set datasheet for more information on function select
	gpio_set_function(0, GPIO_FUNC_UART);  // UART TX Pin 0
	gpio_set_function(1, GPIO_FUNC_UART);  // UART RX Pin 1
	
	// Use some the various UART functions to send out data
	// In a default system, printf will also output via the default UART
	
	// Send out a string, with CR/LF conversions
	//uart_puts(uart0, " Hello, UART!\n");
	
	// For more examples of UART use see https://github.com/raspberrypi/pico-examples/tree/master/uart
}

// PN532 hardware initialization function
void Hardware_Init_PN532(void)
{
	// Initialize PN532 reset pin
	gpio_init(PN532_RST_PIN);
	gpio_set_dir(PN532_RST_PIN, GPIO_OUT);
	gpio_put(PN532_RST_PIN, PCD_RESET_ACTIVE); // Default to active in reset (considering inverting logic)
	
	// Note: I2C initialization is handled by Hardware_Init_I2C()
	// No need to initialize I2C again here since PN532 uses the same I2C port
}

// PN532 release from reset function
void Hardware_PN532_Release_Reset(void)
{
	gpio_put(PN532_RST_PIN, PCD_RESET_INACTIVE); // Release reset (considering inverting logic)
	sleep_ms(10);               // Give time to come out of reset
}

// LCD hardware initialization function
void Hardware_Init_LCD(void)
{
	// Initialize LCD control pins
	gpio_init(LCD_DC_PIN);
	gpio_set_dir(LCD_DC_PIN, GPIO_OUT);
	gpio_put(LCD_DC_PIN, 0); // Default to command mode
	
	gpio_init(LCD_RESET_PIN);
	gpio_set_dir(LCD_RESET_PIN, GPIO_OUT);
	gpio_put(LCD_RESET_PIN, LCD_RESET_INACTIVE); // Default to not in reset (considering inverting logic)
	
	gpio_init(LCD_BACKLIGHT_PIN);
	gpio_set_dir(LCD_BACKLIGHT_PIN, GPIO_OUT);
	gpio_put(LCD_BACKLIGHT_PIN, 1); // Default backlight off (assuming active low)
	
	// Initialize SPI for LCD (uses SPI0 as defined in header)
	spi_init(LCD_SPI, 40*1000*1000);  // 10MHz SPI for LCD (safe speed)
	gpio_set_function(LCD_SCK_PIN, GPIO_FUNC_SPI);
	gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
	gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);
	
	// CS pin as regular GPIO (manually controlled)
	gpio_init(LCD_CS_PIN);
	gpio_set_dir(LCD_CS_PIN, GPIO_OUT);
	gpio_put(LCD_CS_PIN, 1);  // CS inactive (high)
}

// USB hardware initialization function
void Hardware_Init_USB(void)
{
#if defined(PICO_BUILD)
	// For Pico, we use the native stdio USB implementation
	// stdio_init_all() should already be called in main
	// Initialize USB logging system

#else
	// No additional USB hardware initialization needed for RP2040
	// TinyUSB will handle the USB PHY initialization when tusb_init() is called
	// The USB pins on RP2040 are dedicated and don't need GPIO configuration
#endif
}
#endif

// LCD Hardware Control Functions (unified for both platforms)
#if defined(PICO_BUILD)
/* PWM state tracking for Pico */
static uint lcd_pwm_slice = 0;
static uint lcd_pwm_channel = 0;
static bool lcd_pwm_initialized = false;
#endif

/**
 * @brief Initialize LCD backlight PWM
 * Configures the backlight pin for PWM control at 1kHz with 8-bit resolution
 * @note Call this function once during LCD initialization
 */
void Hardware_LCD_Init_PWM_Backlight(void)
{
#if defined(STM32F411xE)
    /* STM32: Configure Timer for PWM on backlight pin (GPIOA PIN_9 = TIM1_CH2)
     * Using TIM1 for PWM generation at 1kHz with 8-bit resolution
     * TIM1 clock = 100MHz, prescaler = 390, period = 255 gives ~1kHz PWM */
    /* Note: This assumes TIM1 is configured in STM32CubeMX
     * If not already configured, add timer initialization here */
    
    /* For now, we'll use GPIO mode as fallback until timer is configured */
    HAL_GPIO_WritePin(LCD_BACKLIGHT_GPIO_Port, LCD_BACKLIGHT_Pin, GPIO_PIN_RESET);
    
#elif defined(PICO_BUILD)
    /* Pico: Configure PWM slice for LCD backlight pin (GPIO 7)
     * PWM frequency: 1kHz (1000Hz)
     * PWM resolution: 256 levels (8-bit, 0-255)
     * System clock: 125MHz
     * PWM clock = 125MHz / (DIV * WRAP)
     * For 1kHz: 125,000,000 / (488.28 * 256) = 1000Hz */
    
    // Get PWM slice and channel for the backlight pin
    lcd_pwm_slice = pwm_gpio_to_slice_num(LCD_BACKLIGHT_PIN);
    lcd_pwm_channel = pwm_gpio_to_channel(LCD_BACKLIGHT_PIN);
    
    // Set the backlight pin to PWM function
    gpio_set_function(LCD_BACKLIGHT_PIN, GPIO_FUNC_PWM);
    
    // Configure PWM with 256 wrap (8-bit resolution)
    pwm_set_wrap(lcd_pwm_slice, 255);
    
    // Set clock divider for 1kHz frequency
    // 125MHz / (488.28125 * 256) = 1000Hz
    pwm_set_clkdiv(lcd_pwm_slice, 488.28125f);
    
    // Start with backlight off (255 for active-low backlight)
    pwm_set_chan_level(lcd_pwm_slice, lcd_pwm_channel, 255);
    
    // Enable PWM
    pwm_set_enabled(lcd_pwm_slice, true);
    
    lcd_pwm_initialized = true;
    
#else
    // Unknown platform
#endif
}

/**
 * @brief Set LCD backlight brightness with PWM
 * @param brightness Brightness level (0-255): 0 = off, 255 = max brightness
 * @note This function provides hardware-level PWM control
 *       Higher-level functions should apply gamma correction before calling this
 */
void Hardware_LCD_Set_Backlight_Brightness(uint8_t brightness)
{
#if defined(STM32F411xE)
    /* STM32: Set TIM1 channel 2 compare value for PWM duty cycle
     * For now, use simple on/off until timer is configured */
    if (brightness > 127) {
        HAL_GPIO_WritePin(LCD_BACKLIGHT_GPIO_Port, LCD_BACKLIGHT_Pin, GPIO_PIN_SET);
    } else if (brightness == 0) {
        HAL_GPIO_WritePin(LCD_BACKLIGHT_GPIO_Port, LCD_BACKLIGHT_Pin, GPIO_PIN_RESET);
    }
    // TODO: Replace with proper TIM1 PWM control when timer is configured
    
#elif defined(PICO_BUILD)
    if (lcd_pwm_initialized) {
        // Pico backlight is active-low, so invert the brightness
        uint8_t inverted_brightness = 255 - brightness;
        pwm_set_chan_level(lcd_pwm_slice, lcd_pwm_channel, inverted_brightness);
    } else {
        // Fallback to GPIO control if PWM not initialized
        gpio_put(LCD_BACKLIGHT_PIN, brightness > 0 ? 0 : 1);
    }
    
#else
    // Unknown platform
    (void)brightness;
#endif
}

/**
 * @brief Reset LCD display
 */
void Hardware_LCD_Reset(void)
{
#if defined(STM32F411xE)
    // Pull reset low (considering inverting hardware logic)
    HAL_GPIO_WritePin(LCD_RESET_GPIO_Port, LCD_RESET_Pin, LCD_RESET_ACTIVE);
    HAL_Delay(10);  // Hold reset for 10ms
    // Release reset (considering inverting hardware logic)
    HAL_GPIO_WritePin(LCD_RESET_GPIO_Port, LCD_RESET_Pin, LCD_RESET_INACTIVE);
    HAL_Delay(10);  // Wait 10ms after release
#elif defined(PICO_BUILD)
    gpio_put(LCD_RESET_PIN, LCD_RESET_ACTIVE);   // Assert reset (inverted logic)
    sleep_ms(10);                                // Hold reset for 10ms
    gpio_put(LCD_RESET_PIN, LCD_RESET_INACTIVE); // Release reset (inverted logic)
    sleep_ms(10);                                // Give time to come out of reset
#else
    // Unknown platform
#endif
}

/**
 * @brief Set LCD Data/Command line state
 * @param state 1 for data mode, 0 for command mode
 */
void Hardware_LCD_Set_DC(uint8_t state)
{
#if defined(STM32F411xE)
    if (state) {
        HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
    }
#elif defined(PICO_BUILD)
    gpio_put(LCD_DC_PIN, state ? 1 : 0);
#else
    // Unknown platform
    (void)state;
#endif
}

/* ========================================================================== */
/*                    PLATFORM-ABSTRACTED INTERFACE FUNCTIONS               */
/* ========================================================================== */

/**
 * @brief Platform-abstracted SPI transaction function
 * @param spi_msg Pointer to SPI message structure
 */
void task_transact_spi_msg(SPI_MSG_DEF *spi_msg)
{
    if (!spi_msg) return;
    spi_driver_transact(spi_msg);
}

/**
 * @brief Platform-abstracted I2C transaction function
 * @param i2c_msg Pointer to I2C message structure
 */
void task_transact_i2c_msg(I2C_MSG_DEF *i2c_msg)
{
    if (!i2c_msg) return;
    i2c_driver_transact(i2c_msg);
}

/* ========================================================================== */
/*                    I2C WRAPPER FUNCTIONS (SIMPLIFIED API)                */
/* ========================================================================== */

/**
 * @brief Write data to I2C device - Simplified wrapper function
 * @param i2c_id I2C bus identifier (I2C_PN532, I2C_GENERIC, etc.)
 * @param device_address 7-bit I2C device address
 * @param tx_data Pointer to data to transmit
 * @param tx_length Number of bytes to transmit
 * @return uint8_t 0 = success, non-zero = error
 * 
 * This function wraps the I2C_MSG_DEF structure, providing a cleaner API
 * for drivers that don't need to access the low-level message structure.
 */
uint8_t Hardware_I2C_Write(I2C_ACCESS_IDS_ENUM i2c_id, uint8_t device_address, 
                           uint8_t *tx_data, uint32_t tx_length)
{
    // Validate parameters
    if (tx_data == NULL || tx_length == 0) {
        return 1; // Error: Invalid parameters
    }
    
    // Create and initialize I2C message structure
    I2C_MSG_DEF i2c_msg = {0};
    
    i2c_msg.tx_data = tx_data;
    i2c_msg.tx_msg_length = tx_length;
    i2c_msg.device_address = device_address;
    i2c_msg.i2c_id = i2c_id;
    i2c_msg.rw = 0; // Write operation
    i2c_msg.RequestingTask = xTaskGetCurrentTaskHandle();
    i2c_msg.status = 1; // Initialize as error
    
    // Perform I2C transaction
    task_transact_i2c_msg(&i2c_msg);
    
    // Return status (0 = success, non-zero = error)
    return i2c_msg.status;
}

/**
 * @brief Read data from I2C device - Simplified wrapper function
 * @param i2c_id I2C bus identifier (I2C_PN532, I2C_GENERIC, etc.)
 * @param device_address 7-bit I2C device address
 * @param rx_data Pointer to buffer for received data
 * @param rx_length Number of bytes to receive
 * @return uint8_t 0 = success, non-zero = error
 * 
 * This function wraps the I2C_MSG_DEF structure, providing a cleaner API
 * for drivers that don't need to access the low-level message structure.
 */
uint8_t Hardware_I2C_Read(I2C_ACCESS_IDS_ENUM i2c_id, uint8_t device_address,
                          uint8_t *rx_data, uint32_t rx_length)
{
    // Validate parameters
    if (rx_data == NULL || rx_length == 0) {
        return 1; // Error: Invalid parameters
    }
    
    // Create and initialize I2C message structure
    I2C_MSG_DEF i2c_msg = {0};
    
    i2c_msg.rx_data = rx_data;
    i2c_msg.rx_msg_length = rx_length;
    i2c_msg.device_address = device_address;
    i2c_msg.i2c_id = i2c_id;
    i2c_msg.rw = 1; // Read operation
    i2c_msg.RequestingTask = xTaskGetCurrentTaskHandle();
    i2c_msg.status = 1; // Initialize as error
    
    // Perform I2C transaction
    task_transact_i2c_msg(&i2c_msg);
    
    // Return status (0 = success, non-zero = error)
    return i2c_msg.status;
}

/* ========================================================================== */
/*                    PLATFORM-ABSTRACTED GPIO FUNCTIONS                    */
/* ========================================================================== */

/**
 * @brief Write to a GPIO pin in a platform-independent way
 * @param pin Pin number/identifier
 * @param state State to write (HW_GPIO_STATE_LOW or HW_GPIO_STATE_HIGH)
 */
void Hardware_GPIO_Write(uint32_t pin, uint8_t state)
{
#if defined(STM32F411xE)
    // STM32 implementation with pin identification
    // Since multiple pins can have the same GPIO_PIN_x value on different ports,
    // we need to use a more sophisticated identification system
    GPIO_TypeDef* port = NULL;
    uint16_t gpio_pin = 0;
    
    // Map pin identifier to port and pin
    switch(pin) {
        case HW_PIN_LCD_CS: // LCD_CS (GPIOA, PIN_0)
            port = LCD_CS_GPIO_Port;
            gpio_pin = LCD_CS_Pin;
            break;
        case HW_PIN_PCD_IRQ: // PCD_IRQ (GPIOA, PIN_8)
            port = PCD_IRQ_GPIO_Port;
            gpio_pin = PCD_IRQ_Pin;
            break;
        case HW_PIN_LCD_BACKLIGHT: // LCD_BACKLIGHT (GPIOA, PIN_9)
            port = LCD_BACKLIGHT_GPIO_Port;
            gpio_pin = LCD_BACKLIGHT_Pin;
            break;
        case HW_PIN_LCD_DC: // LCD_DC (GPIOB, PIN_4)
            port = LCD_DC_GPIO_Port;
            gpio_pin = LCD_DC_Pin;
            break;
        case HW_PIN_LCD_RESET: // LCD_RESET (GPIOB, PIN_5)
            port = LCD_RESET_GPIO_Port;
            gpio_pin = LCD_RESET_Pin;
            break;
        case HW_PIN_PCD_RST: // PCD_RST (GPIOB, PIN_9)
            port = PCD_RST_GPIO_Port;
            gpio_pin = PCD_RST_Pin;
            break;
        default:
            // Unknown pin identifier - do nothing
            return;
    }
    
    if (port != NULL) {
        HAL_GPIO_WritePin(port, gpio_pin, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
#elif defined(PICO_BUILD)
    gpio_put(pin, state ? 1 : 0);
#else
    (void)pin;
    (void)state;
#endif
}

/**
 * @brief Read from a GPIO pin in a platform-independent way
 * @param pin Pin number/identifier
 * @return Pin state (HW_GPIO_STATE_LOW or HW_GPIO_STATE_HIGH)
 */
uint8_t Hardware_GPIO_Read(uint32_t pin)
{
#if defined(STM32F411xE)
    // STM32 implementation with pin identification
    GPIO_TypeDef* port = NULL;
    uint16_t gpio_pin = 0;
    
    // Map pin identifier to port and pin
    switch(pin) {
        case HW_PIN_LCD_CS: // LCD_CS (GPIOA, PIN_0)
            port = LCD_CS_GPIO_Port;
            gpio_pin = LCD_CS_Pin;
            break;
        case HW_PIN_PCD_IRQ: // PCD_IRQ (GPIOA, PIN_8)
            port = PCD_IRQ_GPIO_Port;
            gpio_pin = PCD_IRQ_Pin;
            break;
        case HW_PIN_LCD_BACKLIGHT: // LCD_BACKLIGHT (GPIOA, PIN_9)
            port = LCD_BACKLIGHT_GPIO_Port;
            gpio_pin = LCD_BACKLIGHT_Pin;
            break;
        case HW_PIN_LCD_DC: // LCD_DC (GPIOB, PIN_4)
            port = LCD_DC_GPIO_Port;
            gpio_pin = LCD_DC_Pin;
            break;
        case HW_PIN_LCD_RESET: // LCD_RESET (GPIOB, PIN_5)
            port = LCD_RESET_GPIO_Port;
            gpio_pin = LCD_RESET_Pin;
            break;
        case HW_PIN_PCD_RST: // PCD_RST (GPIOB, PIN_9)
            port = PCD_RST_GPIO_Port;
            gpio_pin = PCD_RST_Pin;
            break;
        default:
            // Unknown pin identifier
            return HW_GPIO_STATE_LOW;
    }
    
    if (port != NULL) {
        return HAL_GPIO_ReadPin(port, gpio_pin) == GPIO_PIN_SET ? HW_GPIO_STATE_HIGH : HW_GPIO_STATE_LOW;
    }
    return HW_GPIO_STATE_LOW;
#elif defined(PICO_BUILD)
    return gpio_get(pin) ? HW_GPIO_STATE_HIGH : HW_GPIO_STATE_LOW;
#else
    (void)pin;
    return HW_GPIO_STATE_LOW;
#endif
}

/* ========================================================================== */
/*                    PLATFORM-ABSTRACTED SPI FUNCTIONS                     */
/* ========================================================================== */

static void* get_spi_inst(SPI_ACCESS_IDS_ENUM spi_id) {
#if defined(PICO_BUILD)
    switch(spi_id) {
        case SPI_LCD: return LCD_SPI;
        case SPI_RC522: return RC522_SPI;
        default: return NULL;
    }
#else
    return NULL;
#endif
}

uint32_t Hardware_SPI_SetBaudrate(SPI_ACCESS_IDS_ENUM spi_id, uint32_t baudrate) {
#if defined(PICO_BUILD)
    spi_inst_t *spi = (spi_inst_t*)get_spi_inst(spi_id);
    if (spi) return spi_set_baudrate(spi, baudrate);
#endif
    return 0;
}

void Hardware_SPI_SetFormat(SPI_ACCESS_IDS_ENUM spi_id, uint8_t data_bits, uint8_t cpol, uint8_t cpha) {
#if defined(PICO_BUILD)
    spi_inst_t *spi = (spi_inst_t*)get_spi_inst(spi_id);
    if (spi) {
        spi_cpol_t cpol_enum = cpol ? SPI_CPOL_1 : SPI_CPOL_0;
        spi_cpha_t cpha_enum = cpha ? SPI_CPHA_1 : SPI_CPHA_0;
        spi_set_format(spi, data_bits, cpol_enum, cpha_enum, SPI_MSB_FIRST);
    }
#endif
}

void Hardware_SPI_WriteByte(SPI_ACCESS_IDS_ENUM spi_id, uint8_t data) {
#if defined(PICO_BUILD)
    spi_inst_t *spi = (spi_inst_t*)get_spi_inst(spi_id);
    if (spi) spi_write_blocking(spi, &data, 1);
#endif
}

void Hardware_SPI_ReadByte(SPI_ACCESS_IDS_ENUM spi_id, uint8_t dummy, uint8_t *data) {
#if defined(PICO_BUILD)
    spi_inst_t *spi = (spi_inst_t*)get_spi_inst(spi_id);
    if (spi && data) spi_read_blocking(spi, dummy, data, 1);
#endif
}

void Hardware_SPI_WriteBuffer(SPI_ACCESS_IDS_ENUM spi_id, const uint8_t *buffer, uint16_t length) {
#if defined(PICO_BUILD)
    spi_inst_t *spi = (spi_inst_t*)get_spi_inst(spi_id);
    if (spi && buffer) spi_write_blocking(spi, buffer, length);
#endif
}

void Hardware_SPI_ReadBuffer(SPI_ACCESS_IDS_ENUM spi_id, uint8_t dummy, uint8_t *buffer, uint16_t length) {
#if defined(PICO_BUILD)
    spi_inst_t *spi = (spi_inst_t*)get_spi_inst(spi_id);
    if (spi && buffer) spi_read_blocking(spi, dummy, buffer, length);
#endif
}

/**
 * @brief Initialize a GPIO pin in a platform-independent way
 * @param pin Pin number/identifier
 * @param direction Direction (HW_GPIO_DIR_INPUT or HW_GPIO_DIR_OUTPUT)
 */
void Hardware_GPIO_Init(uint32_t pin, uint8_t direction)
{
#if defined(STM32F411xE)
    // STM32 implementation - Note: GPIO pins are typically configured by MX_GPIO_Init()
    // This function provides a platform-abstracted interface for runtime configuration
    // if needed, but most pins are already configured during system initialization
    GPIO_TypeDef* port = NULL;
    uint16_t gpio_pin = 0;
    
    // Map pin identifier to port and pin
    switch(pin) {
        case HW_PIN_LCD_CS: // LCD_CS (GPIOA, PIN_0)
            port = LCD_CS_GPIO_Port;
            gpio_pin = LCD_CS_Pin;
            break;
        case HW_PIN_PCD_IRQ: // PCD_IRQ (GPIOA, PIN_8)
            port = PCD_IRQ_GPIO_Port;
            gpio_pin = PCD_IRQ_Pin;
            break;
        case HW_PIN_LCD_BACKLIGHT: // LCD_BACKLIGHT (GPIOA, PIN_9)
            port = LCD_BACKLIGHT_GPIO_Port;
            gpio_pin = LCD_BACKLIGHT_Pin;
            break;
        case HW_PIN_LCD_DC: // LCD_DC (GPIOB, PIN_4)
            port = LCD_DC_GPIO_Port;
            gpio_pin = LCD_DC_Pin;
            break;
        case HW_PIN_LCD_RESET: // LCD_RESET (GPIOB, PIN_5)
            port = LCD_RESET_GPIO_Port;
            gpio_pin = LCD_RESET_Pin;
            break;
        case HW_PIN_PCD_RST: // PCD_RST (GPIOB, PIN_9)
            port = PCD_RST_GPIO_Port;
            gpio_pin = PCD_RST_Pin;
            break;
        default:
            // Unknown pin identifier - do nothing
            return;
    }
    
    // For STM32, runtime GPIO reconfiguration would require HAL GPIO configuration
    // which is typically not needed since pins are configured by MX_GPIO_Init()
    (void)port;
    (void)gpio_pin;
    (void)direction;
#elif defined(PICO_BUILD)
    gpio_init(pin);
    gpio_set_dir(pin, direction == HW_GPIO_DIR_OUTPUT ? GPIO_OUT : GPIO_IN);
#else
    (void)pin;
    (void)direction;
#endif
}

/* ========================================================================== */
/*                    PLATFORM-ABSTRACTED PN532 FUNCTIONS                   */
/* ========================================================================== */

/**
 * @brief Reset the PN532 module in a platform-independent way
 */
void Hardware_PN532_Reset(void)
{
#if defined(STM32F411xE)
    // STM32 implementation using platform-abstracted GPIO functions
    // Initialize reset pin as output (though it should already be configured by MX_GPIO_Init)
    //Hardware_GPIO_Init(PN532_RST_PIN_ID, HW_GPIO_DIR_OUTPUT);
    
    // Pull reset pin to active state (considering inverting hardware logic)
    Hardware_GPIO_Write(PN532_RST_PIN_ID, PCD_RESET_ACTIVE);
    Hardware_PN532_Delay_MS(PN532_RESET_DELAY_MS);  // Hold reset for 10ms
    
    // Release reset pin to inactive state (considering inverting hardware logic)
    Hardware_GPIO_Write(PN532_RST_PIN_ID, PCD_RESET_INACTIVE);
    Hardware_PN532_Delay_MS(PN532_RESET_DELAY_MS);  // Wait 10ms for PN532 to initialize

    // Alternative direct STM32 HAL implementation (commented out):
    // HAL_GPIO_WritePin(PCD_RST_GPIO_Port, PCD_RST_Pin, PCD_RESET_ACTIVE);
    // HAL_Delay(10);
    // HAL_GPIO_WritePin(PCD_RST_GPIO_Port, PCD_RST_Pin, PCD_RESET_INACTIVE);
    // HAL_Delay(10);
#elif defined(PICO_BUILD)
    // Pico implementation using platform-abstracted GPIO functions
    // Initialize reset pin as output
    Hardware_GPIO_Init(PN532_RST_PIN_ID, HW_GPIO_DIR_OUTPUT);
    
    // Pull reset pin to active state (considering inverting hardware logic)
    Hardware_GPIO_Write(PN532_RST_PIN_ID, PCD_RESET_ACTIVE);
    Hardware_PN532_Delay_MS(200);
    
    // Release reset pin to inactive state (considering inverting hardware logic)
    Hardware_GPIO_Write(PN532_RST_PIN_ID, PCD_RESET_INACTIVE);
    Hardware_PN532_Delay_MS(700);
#else
    // Generic fallback implementation
    // Initialize reset pin as output
    Hardware_GPIO_Init(PN532_RST_PIN_ID, HW_GPIO_DIR_OUTPUT);
    
    // Pull reset pin low
    Hardware_GPIO_Write(PN532_RST_PIN_ID, HW_GPIO_STATE_LOW);
    Hardware_PN532_Delay_MS(10);
    
    // Release reset pin (high)
    Hardware_GPIO_Write(PN532_RST_PIN_ID, HW_GPIO_STATE_HIGH);
    Hardware_PN532_Delay_MS(10);
#endif
}

/**
 * @brief Platform-abstracted delay function for PN532
 * @param milliseconds Delay in milliseconds
 */
void Hardware_PN532_Delay_MS(uint32_t milliseconds)
{
	vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

/**
 * @brief Disable all interrupts (critical section entry)
 * @return Previous interrupt state (to restore later)
 */
uint32_t Hardware_Disable_All_Interrupts(void)
{
#if defined(PICO_BUILD)
    // For RP2040, disable all interrupts and return previous state
    uint32_t previous_state = save_and_disable_interrupts();
    USB_Log_Printf("All interrupts disabled\r\n");
    return previous_state;
#elif defined(STM32F411xE)
    // For STM32, disable all interrupts
    __disable_irq();
    USB_Log_Printf("All interrupts disabled\r\n");
    return 0; // STM32 doesn't return previous state in this simple case
#else
    // Generic ARM Cortex-M implementation
    uint32_t previous_state = __get_PRIMASK();
    __disable_irq();
    return previous_state;
#endif
}

/**
 * @brief Re-enable all interrupts (critical section exit)
 * @param previous_state The state returned by Hardware_Disable_All_Interrupts()
 */
void Hardware_Enable_All_Interrupts(uint32_t previous_state)
{
#if defined(PICO_BUILD)
    // For RP2040, restore previous interrupt state
    restore_interrupts(previous_state);
    USB_Log_Printf("All interrupts restored\r\n");
#elif defined(STM32F411xE)
    // For STM32, enable all interrupts
    __enable_irq();
    USB_Log_Printf("All interrupts enabled\r\n");
    (void)previous_state; // Unused in this simple implementation
#else
    // Generic ARM Cortex-M implementation
    __set_PRIMASK(previous_state);
#endif
}

/**
 * @brief Force disable all interrupts permanently (emergency use only)
 * WARNING: This will disable ALL interrupts including FreeRTOS scheduler!
 * Use only in emergency situations or for debugging.
 */
void Hardware_Force_Disable_All_Interrupts(void)
{
#if defined(PICO_BUILD)
    // Disable all interrupt sources at NVIC level
    for (int i = 0; i < 32; i++) {
        irq_set_enabled(i, false);
    }
    
    // Also disable core interrupts using Pico SDK
    save_and_disable_interrupts();
    
    USB_Log_Printf("EMERGENCY: All interrupts forcibly disabled!\r\n");
    
    // Flash LED to indicate interrupts are disabled
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    
    // Infinite loop with LED flashing to show system state
    while(1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(100);
    }
    
#elif defined(STM32F411xE)
    // Disable all NVIC interrupts
    for (int i = 0; i < 240; i++) { // STM32F411 has up to 240 interrupt lines
        HAL_NVIC_DisableIRQ((IRQn_Type)i);
    }
    
    // Disable core interrupts
    __disable_irq();
    
    USB_Log_Printf("EMERGENCY: All interrupts forcibly disabled!\r\n");
    
    // Infinite loop to show system state
    while(1) {
        // Basic delay without interrupts
        for(volatile int i = 0; i < 1000000; i++);
    }
#else
    // Generic implementation
    __disable_irq();
    while(1) {
        // Infinite loop
        for(volatile int i = 0; i < 1000000; i++);
    }
#endif
}

/* =================================================================== */
/* I2C PROTECTION FUNCTIONS - THREAD-SAFE I2C BUS ACCESS             */
/* =================================================================== */

/* External variables from System.c */
extern SemaphoreHandle_t i2c_1_Semaphore;

/* Private variables */
static bool hardware_i2c_initialized = false;

/**
 * @brief Initialize I2C protection system
 * @return Hardware_I2C_Status_t Status of initialization
 */
Hardware_I2C_Status_t Hardware_I2C_Init(void)
{
    // Check if the system I2C semaphore is available
    if (i2c_1_Semaphore == NULL) {
        USB_Log_Printf("ERROR: Hardware I2C - semaphore not available\r\n");
        return HARDWARE_I2C_ERROR;
    }
    
    hardware_i2c_initialized = true;
    USB_Log_Printf("Hardware I2C protection initialized successfully\r\n");
    return HARDWARE_I2C_OK;
}

/**
 * @brief Check if the I2C protection system is initialized
 * @return bool true if initialized, false otherwise
 */
bool Hardware_IsI2CInitialized(void)
{
    return hardware_i2c_initialized;
}

/**
 * @brief Thread-safe I2C write operation
 * @param i2c I2C instance
 * @param addr I2C slave address
 * @param src Source data buffer
 * @param len Number of bytes to write
 * @param nostop Whether to send stop condition
 * @return int Number of bytes written, or negative error code
 */
int Hardware_I2C_Write_Protected(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop)
{
    // Parameter validation
    if (src == NULL || len == 0) {
        return -1;
    }
    
    // Check initialization
    if (!hardware_i2c_initialized) {
        return -1;
    }
    
    // Take mutex with timeout
    TickType_t timeout_ticks = pdMS_TO_TICKS(HARDWARE_I2C_TIMEOUT_MS);
    if (xSemaphoreTake(i2c_1_Semaphore, timeout_ticks) != pdTRUE) {
        return -1;
    }
    
    int result = -1;
    
    // Perform platform-specific I2C write
#if defined(PICO_BOARD) || defined(PICO_BUILD)
    if (i2c == NULL) {
        xSemaphoreGive(i2c_1_Semaphore);
        return -1;
    }
    
    // Use timeout variant to prevent infinite blocking
    result = i2c_write_timeout_us(i2c, addr, src, len, nostop, HARDWARE_I2C_TIMEOUT_MS * 1000);
    
#elif defined(STM32F411xE)
    // STM32 implementation - use HAL functions
    HAL_StatusTypeDef hal_status = HAL_I2C_Master_Transmit(&hi2c1, 
                                                          (addr << 1), 
                                                          (uint8_t*)src, 
                                                          len, 
                                                          100); // 100ms timeout
    result = (hal_status == HAL_OK) ? (int)len : -1;
#else
    result = -1;
#endif
    
    // Release mutex
    xSemaphoreGive(i2c_1_Semaphore);
    
    return result;
}

/**
 * @brief Thread-safe I2C read operation
 * @param i2c I2C instance
 * @param addr I2C slave address
 * @param dst Destination data buffer
 * @param len Number of bytes to read
 * @param nostop Whether to send stop condition
 * @return int Number of bytes read, or negative error code
 */
int Hardware_I2C_Read_Protected(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop)
{
    // Parameter validation
    if (dst == NULL || len == 0) {
        return -1;
    }
    
    // Check initialization
    if (!hardware_i2c_initialized) {
        return -1;
    }
    
    // Take mutex with timeout
    TickType_t timeout_ticks = pdMS_TO_TICKS(HARDWARE_I2C_TIMEOUT_MS);
    if (xSemaphoreTake(i2c_1_Semaphore, timeout_ticks) != pdTRUE) {
        return -1;
    }
    
    int result = -1;
    
    // Perform platform-specific I2C read
#if defined(PICO_BOARD) || defined(PICO_BUILD)
    if (i2c == NULL) {
        xSemaphoreGive(i2c_1_Semaphore);
        return -1;
    }
    
    // Use timeout variant to prevent infinite blocking
    result = i2c_read_timeout_us(i2c, addr, dst, len, nostop, HARDWARE_I2C_TIMEOUT_MS * 1000);
    
#elif defined(STM32F411xE)
    // STM32 implementation - use HAL functions
    HAL_StatusTypeDef hal_status = HAL_I2C_Master_Receive(&hi2c1, 
                                                         (addr << 1), 
                                                         dst, 
                                                         len, 
                                                         100); // 100ms timeout
    result = (hal_status == HAL_OK) ? (int)len : -1;
#else
    result = -1;
#endif
    
    // Release mutex
    xSemaphoreGive(i2c_1_Semaphore);
    
    return result;
}

/**
 * @brief Thread-safe atomic I2C write-then-read operation
 * @details Performs write and read as a single atomic transaction with semaphore held throughout
 * @param i2c I2C instance
 * @param addr I2C slave address
 * @param src Source data buffer for write
 * @param src_len Number of bytes to write
 * @param dst Destination data buffer for read
 * @param dst_len Number of bytes to read
 * @return int Number of bytes read on success, or negative error code
 */
int Hardware_I2C_WriteRead_Protected(i2c_inst_t *i2c, uint8_t addr, 
                                     const uint8_t *src, size_t src_len,
                                     uint8_t *dst, size_t dst_len)
{
    // Parameter validation
    if (src == NULL || src_len == 0 || dst == NULL || dst_len == 0) {
        return -1;
    }
    
    // Check initialization
    if (!hardware_i2c_initialized) {
        return -1;
    }
    
    // Take mutex with timeout
    TickType_t timeout_ticks = pdMS_TO_TICKS(HARDWARE_I2C_TIMEOUT_MS);
    if (xSemaphoreTake(i2c_1_Semaphore, timeout_ticks) != pdTRUE) {
        return -1;
    }
    
    int result = -1;
    
    // Perform platform-specific atomic I2C write-read
#if defined(PICO_BOARD) || defined(PICO_BUILD)
    if (i2c == NULL) {
        xSemaphoreGive(i2c_1_Semaphore);
        return -1;
    }
    
    // Write with restart condition (nostop=true) and timeout
    int write_result = i2c_write_timeout_us(i2c, addr, src, src_len, true, HARDWARE_I2C_TIMEOUT_MS * 1000);
    if (write_result < 0) {
        xSemaphoreGive(i2c_1_Semaphore);
        return write_result;
    }
    
    // Read with stop condition (nostop=false) and timeout
    result = i2c_read_timeout_us(i2c, addr, dst, dst_len, false, HARDWARE_I2C_TIMEOUT_MS * 1000);
    
#elif defined(STM32F411xE)
    // STM32 implementation - use HAL functions
    HAL_StatusTypeDef hal_status;
    
    // Write phase
    hal_status = HAL_I2C_Master_Transmit(&hi2c1, (addr << 1), (uint8_t*)src, src_len, 100);
    if (hal_status != HAL_OK) {
        xSemaphoreGive(i2c_1_Semaphore);
        return -1;
    }
    
    // Read phase
    hal_status = HAL_I2C_Master_Receive(&hi2c1, (addr << 1), dst, dst_len, 100);
    result = (hal_status == HAL_OK) ? (int)dst_len : -1;
#else
    result = -1;
#endif
    
    // Release mutex
    xSemaphoreGive(i2c_1_Semaphore);
    
    return result;
}

/**
 * @brief Get the I2C bus semaphore for advanced operations
 * @return SemaphoreHandle_t Handle to the I2C mutex
 */
SemaphoreHandle_t Hardware_I2C_GetMutex(void)
{
    return i2c_1_Semaphore;
}

// Forward declarations for device-specific interrupt handlers
extern void YS_S201_GPIO_InterruptHandler(uint gpio, uint32_t events);

// Global callback pointer for CAT9555 (registered by Hardware_CAT9555_Init_Interrupt_GPIO)
static gpio_irq_callback_t g_cat9555_callback = NULL;

/**
 * @brief Central GPIO interrupt dispatcher for all GPIO interrupts
 * @param gpio GPIO pin number that triggered the interrupt
 * @param events Interrupt event flags (GPIO_IRQ_EDGE_RISE, GPIO_IRQ_EDGE_FALL, etc.)
 * @note This is the single global GPIO interrupt handler for the Pico
 *       It dispatches to device-specific handlers based on GPIO pin number
 */
static void Hardware_GPIO_Central_Dispatcher(uint gpio, uint32_t events)
{
    // Dispatch based on GPIO pin
    switch (gpio) {
        case EXP_INTR_PIN:  // GPIO 8 - CAT9555 I/O Expander
            if (g_cat9555_callback != NULL) {
                g_cat9555_callback(gpio, events);
            }
            break;
            
        case FLOW_SENSOR_PIN:  // GPIO 22 - YS-S201 Flow Sensor
            YS_S201_GPIO_InterruptHandler(gpio, events);
            break;
            
        default:
            // Unknown GPIO interrupt - log for debugging
            USB_Log_Printf("GPIO: Unhandled interrupt on GPIO %d, events=0x%lX\r\n", gpio, events);
            break;
    }
}

/**
 * @brief Initialize global GPIO interrupt system
 * @note This MUST be called once before any device enables GPIO interrupts
 *       Registers the central dispatcher as the global GPIO interrupt handler
 */
void Hardware_Init_GPIO_Interrupts(void)
{
#if defined(PICO_BUILD) || defined(PICO_BOARD)
    // Register the central dispatcher as the global GPIO interrupt handler
    // This must be done BEFORE any device calls gpio_set_irq_enabled()
    gpio_set_irq_callback(&Hardware_GPIO_Central_Dispatcher);
    irq_set_enabled(IO_IRQ_BANK0, true);
    
    USB_Log_Printf("Hardware: Global GPIO interrupt dispatcher initialized\r\n");
#endif
}

/**
 * @brief Initialize CAT9555 interrupt GPIO pin and register callback
 * @param callback GPIO interrupt callback function for CAT9555 events
 * @note Platform-specific implementation for Pico SDK
 *       Requires Hardware_Init_GPIO_Interrupts() to be called first
 */
void Hardware_CAT9555_Init_Interrupt_GPIO(gpio_irq_callback_t callback)
{
#if defined(PICO_BUILD) || defined(PICO_BOARD)
    // Store the CAT9555 callback for the central dispatcher
    g_cat9555_callback = callback;
    
    // Initialize GPIO pin
    gpio_init(EXP_INTR_PIN);
    gpio_set_dir(EXP_INTR_PIN, GPIO_IN);
    gpio_pull_up(EXP_INTR_PIN);  // Pull-up for active-low interrupt
    
    // Read initial state for diagnostics
    bool initial_state = gpio_get(EXP_INTR_PIN);
    USB_Log_Printf("CAT9555 INT pin (GPIO %d) initial state: %s\r\n", 
                   EXP_INTR_PIN, initial_state ? "HIGH (idle)" : "LOW (active)");
    
    // Enable interrupt on falling edge (active low)
    // Note: Global callback already registered by Hardware_Init_GPIO_Interrupts()
    gpio_set_irq_enabled(EXP_INTR_PIN, GPIO_IRQ_EDGE_FALL, true);
    
    USB_Log_Printf("CAT9555 interrupt enabled on GPIO %d (falling edge)\r\n", EXP_INTR_PIN);
#endif
}

/**
 * @brief De-initialize I2C bus
 * This function disables the I2C peripheral and returns the pins to GPIO mode.
 * It's useful for forcing a bus reset.
 */
void Hardware_I2C_Deinit(void)
{
#if defined(PICO_BUILD)
    USB_Log_Printf("I2C: De-initializing I2C bus...\r\n");
    
    // Disable the I2C peripheral
    i2c_deinit(PN532_I2C);
    
    // Set pins back to SIO (standard GPIO) function
    gpio_set_function(PN532_I2C_SDA_PIN, GPIO_FUNC_SIO);
    gpio_set_function(PN532_I2C_SCL_PIN, GPIO_FUNC_SIO);
    
    USB_Log_Printf("I2C: Bus de-initialized.\r\n");
#endif
}
