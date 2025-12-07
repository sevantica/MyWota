/*
  * @attention
  * Copyright (c) Sevantica 2025
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
*/

#ifndef APPLICATION_INCLUDE_HARDWARE_ACCESS_H_
#define APPLICATION_INCLUDE_HARDWARE_ACCESS_H_

/*Includes ----------------------------------------------------------*/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "CAT9555_Driver.h"
#include "stdbool.h"

/* Platform-specific includes */
#if defined(STM32F411xE)
#include "stm32f4xx_hal.h"
//#include "i2c.h"
#include "spi.h"
#elif defined(PICO_BOARD)
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

/* Pico-specific pin definitions */
/* ===================================================================== */
/* HARDWARE PIN DEFINITIONS - GROUPED BY MODULE                         */
/* ===================================================================== */

/* LCD Display Module Pins (SPI0) */
#define LCD_DC_PIN          2       // GPIO 2 (Physical pin 4)
#define LCD_RESET_PIN       3       // GPIO 3 (Physical pin 5)
#define LCD_BACKLIGHT_PIN   7       // GPIO 7 (Physical pin 10) - LCD_BACK_LIGHT_PIN

/* SPI0 for LCD - Standard Pico SPI0 pins */
#define LCD_CS_PIN          17      // GPIO 17 (Physical pin 22) - SPI0_CSn
#define LCD_SCK_PIN         18      // GPIO 18 (Physical pin 24) - SPI0_SCK  
#define LCD_MOSI_PIN        19      // GPIO 19 (Physical pin 25) - SPI0_TX (MOSI)
#define LCD_MISO_PIN        16      // GPIO 16 (Physical pin 21) - SPI0_RX (MISO)
#define LCD_SPI             spi0    // SPI0 peripheral for LCD

/* PN532 NFC Module Pins (I2C0) */
#define PN532_RST_PIN       14      // GPIO 14 (Physical pin 19) - PCD_RESET  
#define PN532_I2C_SDA_PIN   20      // GPIO 20 (Physical pin 26) - I2C_0_SDA
#define PN532_I2C_SCL_PIN   21      // GPIO 21 (Physical pin 27) - I2C_0_SCL
#define PN532_I2C           i2c0    // I2C0 peripheral
/* RC522 RFID Module Pins (SPI0) */
#define RC522_SCK_PIN       4       // GPIO 4 (Physical pin 6) - SPI0_CLK via UART1_TX
#define RC522_MOSI_PIN      5       // GPIO 5 (Physical pin 7) - SPI0_MOSI via UART1_RX
#define RC522_MISO_PIN      0       // GPIO 0 (Physical pin 1) - SPI0_MISO via UART0_TX
#define RC522_CS_PIN        1       // GPIO 1 (Physical pin 2) - SPI0_CS via UART0_RX
#define RC522_SPI           spi0    // SPI0 peripheral for RC522

/* UART Pins - From Pinout Diagram */
#define UART_0_TXD_PIN      0       // GPIO 0 (Physical pin 1) - UART_0_TXD
#define UART_0_RXD_PIN      1       // GPIO 1 (Physical pin 2) - UART_0_RXD
#define UART_1_TXD_PIN      4       // GPIO 4 (Physical pin 6) - UART_1_TXD  
#define UART_1_RXD_PIN      5       // GPIO 5 (Physical pin 7) - UART_1_RXD

/* Additional GPIO assignments from schematic */
#define SYSTEM_COMM_LED_PIN 6       // GPIO 6 (Physical pin 9) - SYSTEM_COMM_LED_SIG
#define EXP_INTR_PIN        8       // GPIO 8 (Physical pin 11) - EXP_INTR
#define RS485_DATA_EN_PIN   9       // GPIO 9 (Physical pin 12) - RS485_DATA_EN
#define LIGHT_SENSOR_PIN    28      // GPIO 28 (Physical pin 34) - LIGHT_SENSOR (ADC2)
#define FLOW_SENSOR_PIN     22      // GPIO 22 (Physical pin 29) - FLOW_SENSOR
#define VALVE_CONTROL_PIN   15      // GPIO 15 (Physical pin 20) - VALVE_CONTROL

/* Note: SW_RST and SW_3V3 are power/reset pins, not regular GPIO pins */
/* They appear on physical pins 37 and 36 but are not accessible as GPIO */

/* I2C Bus Assignments - Based on Pinout Diagram */
/* I2C_0 - Primary I2C bus (PN532) */
#define I2C_0_SCL_PIN       21      // GPIO 21 (Physical pin 27) - I2C_0_SCL
#define I2C_0_SDA_PIN       20      // GPIO 20 (Physical pin 26) - I2C_0_SDA

/* I2C_1 - Secondary I2C bus */  
#define I2C_1_SCL_PIN       27      // GPIO 27 (Physical pin 32) - I2C_1_SCL
#define I2C_1_SDA_PIN       26      // GPIO 26 (Physical pin 31) - I2C_1_SDA

/* Legacy compatibility - map to I2C_1 */
#define I2C_SCL_PIN         I2C_1_SCL_PIN
#define I2C_SDA_PIN         I2C_1_SDA_PIN

/* System LED Pin */
#define PICO_LED_PIN        25      // Built-in LED (if available)

/* I/O Expander (CAT9555) Module Pins - Connected via I2C_0 */
#define CAT9555_I2C_ADDRESS     0x27    // I2C address (A2=A1=A0=1, all pulled high)
#define CAT9555_I2C_SCL_PIN     I2C_0_SCL_PIN   // Uses I2C_0 SCL
#define CAT9555_I2C_SDA_PIN     I2C_0_SDA_PIN   // Uses I2C_0 SDA
#define CAT9555_INT_PIN         1       // GPIO 1 (Physical pin 2) - Interrupt pin

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

/* Application-level aliases */
#define SYSTEM_LED_PIN                 IO_PIN_SYSTEM_STATUS_LED

/* ===================================================================== */
/* END PIN DEFINITIONS                                                   */
/* ===================================================================== */

#endif

/*Typedefs -----------------------------------------------------------*/

typedef enum IO_type
{
	GPIO_Type,
	I2C_Type,
	I2C_Secondary_Type,
	MUX_Type,
	IO_read,
	IO_write

}IO_Types_Enum;


typedef enum SPI_ACCESS_IDS
{
	SPI_RC522,
	SPI_LCD,
	SPI_GENERIC

}SPI_ACCESS_IDS_ENUM;

typedef enum I2C_ACCESS_IDS
{
	I2C_PN532,
	I2C_GENERIC,
	I2C_SECONDARY

}I2C_ACCESS_IDS_ENUM;


typedef struct IO_Definition{

	uint64_t IO_ID;
	uint8_t IO_TYPE;
	uint8_t state;
	uint8_t rw;
	TaskHandle_t RequestingTask;
	uint64_t requestTime;
	void(*staleRequestCallback)(void);
	void(*resultCallback)(uint8_t);


}IO_Def;

typedef struct I2C_Device{

	uint8_t address;
	uint8_t data[4];
	uint8_t busy_status;
	uint8_t available;
	//#I2C_Device_Def i2c_bus;
	uint8_t pin_config_array[8];

}I2C_Device_Def;

typedef struct SPI_MSG_DEF{

	uint8_t* rx_data;
	uint8_t* tx_data;
	uint32_t rx_msg_length;
	uint32_t tx_msg_length;
void *spi_handle;
	SPI_ACCESS_IDS_ENUM spi_id;
	uint8_t rw;
	TaskHandle_t RequestingTask;
	uint64_t requestTime;
	void(*staleRequestCallback)(void);
	void(*resultCallback)(uint8_t);
	/* Optional robustness: explicit chip-select control per transfer part
	 * If both are zero (default), driver asserts before and deasserts after transmit (legacy behavior).
	 * If either is set, the driver only performs the specified actions, allowing CS to stay asserted
	 * across a sequence (e.g., cmd + params, or long pixel bursts).
	 */
	uint8_t cs_assert;   /* 1 = assert CS (drive low) before transmit */
	uint8_t cs_deassert; /* 1 = deassert CS (drive high) after transmit */

}SPI_MSG_DEF;

typedef struct I2C_MSG_DEF{

	uint8_t* rx_data;
	uint8_t* tx_data;
	uint32_t rx_msg_length;
	uint32_t tx_msg_length;
	void *i2c_handle;
	I2C_ACCESS_IDS_ENUM i2c_id;
	uint8_t device_address;
	uint8_t rw;
	uint8_t status;          /* 0 = success, non-zero = error */
	TaskHandle_t RequestingTask;
	uint64_t requestTime;
	void(*staleRequestCallback)(void);
	void(*resultCallback)(uint8_t);

}I2C_MSG_DEF;

/*Defines ------------------------------------------------------------*/

/* Generic GPIO states - platform independent */
#define HW_GPIO_STATE_LOW   0
#define HW_GPIO_STATE_HIGH  1

/* Hardware-specific logic states for inverted pins */
/* PCD (PN532) and LCD Reset pins have inverting logic in hardware */
/* 
 * IMPORTANT: These pins are connected through inverting buffers/logic on the PCB
 * - When GPIO outputs HIGH (1), the actual signal to the device is LOW
 * - When GPIO outputs LOW (0), the actual signal to the device is HIGH
 * 
 * For reset functionality:
 * - To ASSERT reset (put device in reset): GPIO = HIGH (1)
 * - To RELEASE reset (let device run): GPIO = LOW (0)
 */
#define PCD_RESET_ACTIVE    1  /* Due to inverting logic, active reset = GPIO HIGH */
#define PCD_RESET_INACTIVE  0  /* Due to inverting logic, inactive reset = GPIO LOW */
#define LCD_RESET_ACTIVE    1  /* Due to inverting logic, active reset = GPIO HIGH */
#define LCD_RESET_INACTIVE  0  /* Due to inverting logic, inactive reset = GPIO LOW */

/* Generic GPIO directions */
#define HW_GPIO_DIR_INPUT   0
#define HW_GPIO_DIR_OUTPUT  1

/* GPIO Pin Identifiers for platform-abstracted functions */
/* These map to the pin defines in main.h for STM32F411xE */
#define HW_PIN_LCD_CS               0  // Maps to LCD_CS_Pin (GPIOA, PIN_0)
#define HW_PIN_PCD_IRQ              1  // Maps to PCD_IRQ_Pin (GPIOA, PIN_8) 
#define HW_PIN_LCD_BACKLIGHT        2  // Maps to LCD_BACKLIGHT_Pin (GPIOA, PIN_9)
#define HW_PIN_LCD_DC               3  // Maps to LCD_DC_Pin (GPIOB, PIN_4)
#define HW_PIN_LCD_RESET            4  // Maps to LCD_RESET_Pin (GPIOB, PIN_5)
#define HW_PIN_PCD_RST              5  // Maps to PCD_RST_Pin (GPIOB, PIN_9)
#define HW_PIN_PN532_RST            HW_PIN_PCD_RST  // Alias for PN532 reset pin

/* Aliases for compatibility */
#define GPIO_PIN_RESET      HW_GPIO_STATE_LOW
#define GPIO_PIN_SET        HW_GPIO_STATE_HIGH

/* PN532 Pin definitions - platform abstracted */
#if defined(STM32F411xE)
#define PN532_RST_PIN_ID    HW_PIN_PN532_RST   /* Maps to PCD_RST_Pin in main.h */
#elif defined(PICO_BOARD)
#define PN532_RST_PIN_ID    PN532_RST_PIN
#endif


#define		I2C_EXP_ADD_OFFSET	16
#define 	I2C_EXP_ADD_UPPER	0b01000000

#define 	I2C_EXP1_A2_A0		0b00000010
#define 	I2C_EXP2_A2_A0		0b00001110 /*Fixed by board resistors*/

#define		IO_EXP1_ID			0//((uint64_t)(uint32_t)&hi2c1 << 32 | (I2C_EXP_ADD_UPPER | I2C_EXP1_A2_A0) << I2C_EXP_ADD_OFFSET)
#define		IO_EXP2_ID			0//((uint64_t)(uint32_t)&hi2c1 << 32 | (I2C_EXP_ADD_UPPER | I2C_EXP2_A2_A0) << I2C_EXP_ADD_OFFSET)

#define		USER_GPIO_ID_x(gpio_name, pin_num)	((uint64_t)(((uint64_t)(uint32_t)GPIO##gpio_name << 32)| GPIO_PIN_##pin_num ))


#define GPIO_EXP1_EXT_0  		IO_EXP1_ID | 0
#define GPIO_EXP1_EXT_1  		IO_EXP1_ID | 1
#define GPIO_EXP1_EXT_2  		IO_EXP1_ID | 2
#define GPIO_EXP1_EXT_3  		IO_EXP1_ID | 3
#define GPIO_EXP1_EXT_4  		IO_EXP1_ID | 4
#define GPIO_EXP1_EXT_5  		IO_EXP1_ID | 5
#define GPIO_EXP1_EXT_6  		IO_EXP1_ID | 6
#define GPIO_EXP1_EXT_7  		IO_EXP1_ID | 7
#define GPIO_EXP1_EXT_8  		IO_EXP1_ID | 8
#define GPIO_EXP1_EXT_9  		IO_EXP1_ID | 9
#define GPIO_EXP1_EXT_10  		IO_EXP1_ID | 10
#define GPIO_EXP1_EXT_11  		IO_EXP1_ID | 11
#define GPIO_EXP1_EXT_12  		IO_EXP1_ID | 12
#define GPIO_EXP1_EXT_13  		IO_EXP1_ID | 13
#define GPIO_EXP1_EXT_14  		IO_EXP1_ID | 14
#define GPIO_EXP1_EXT_15  		IO_EXP1_ID | 15


#define GPIO_EXP2_EXT_0  		IO_EXP2_ID | 0
#define GPIO_EXP2_EXT_1  		IO_EXP2_ID | 1
#define GPIO_EXP2_EXT_2  		IO_EXP2_ID | 2
#define GPIO_EXP2_EXT_3  		IO_EXP2_ID | 3
#define GPIO_EXP2_EXT_4  		IO_EXP2_ID | 4
#define GPIO_EXP2_EXT_5  		IO_EXP2_ID | 5
#define GPIO_EXP2_EXT_6  		IO_EXP2_ID | 6
#define GPIO_EXP2_EXT_7  		IO_EXP2_ID | 7
#define GPIO_EXP2_EXT_8  		IO_EXP2_ID | 8
#define GPIO_EXP2_EXT_9  		IO_EXP2_ID | 9
#define GPIO_EXP2_EXT_10  		IO_EXP2_ID | 10
#define GPIO_EXP2_EXT_11  		IO_EXP2_ID | 11
#define GPIO_EXP2_EXT_12  		IO_EXP2_ID | 12
#define GPIO_EXP2_EXT_13  		IO_EXP2_ID | 13
#define GPIO_EXP2_EXT_14  		IO_EXP2_ID | 14
#define GPIO_EXP2_EXT_15  		IO_EXP2_ID | 15


/*Macros -------------------------------------------------------------*/


/*Extern Variables ---------------------------------------------------*/

void Task_Start_IO_Driver();


TaskHandle_t task_get_handle_GPIO_Driver_Task();

void gpio_driver_io(IO_Def *p_io_msg);
void spi_driver_transact(SPI_MSG_DEF *spi_msg);
void i2c_driver_transact(I2C_MSG_DEF *i2c_msg);
void io_driver_set_state(int pin_pos, int set_state, TaskHandle_t task_id);

/* Platform-Abstracted Hardware Interface Functions */
void task_transact_spi_msg(SPI_MSG_DEF *spi_msg);
void task_transact_i2c_msg(I2C_MSG_DEF *i2c_msg);

/* I2C Wrapper Functions - Simplified interface without exposing I2C_MSG_DEF */
/**
 * @brief Write data to I2C device (blocking)
 * @param i2c_id I2C bus identifier (I2C_PN532, I2C_GENERIC, etc.)
 * @param device_address 7-bit I2C device address
 * @param tx_data Pointer to data to transmit
 * @param tx_length Number of bytes to transmit
 * @return uint8_t 0 = success, non-zero = error
 */
uint8_t Hardware_I2C_Write(I2C_ACCESS_IDS_ENUM i2c_id, uint8_t device_address, 
                           uint8_t *tx_data, uint32_t tx_length);

/**
 * @brief Read data from I2C device (blocking)
 * @param i2c_id I2C bus identifier (I2C_PN532, I2C_GENERIC, etc.)
 * @param device_address 7-bit I2C device address
 * @param rx_data Pointer to buffer for received data
 * @param rx_length Number of bytes to receive
 * @return uint8_t 0 = success, non-zero = error
 */
uint8_t Hardware_I2C_Read(I2C_ACCESS_IDS_ENUM i2c_id, uint8_t device_address,
                          uint8_t *rx_data, uint32_t rx_length);

/* Hardware initialization functions */
void Hardware_Init_STDIO(void);
void Hardware_Init_LED(void);
void Hardware_Init_SPI(void);
void Hardware_Init_I2C(void);
void Hardware_Init_DMA(void);
void Hardware_Init_UART(void);
void Hardware_Init_USB(void);
void Hardware_Init_PN532(void);

/* Hardware abstracted GPIO functions */
void Hardware_GPIO_Write(uint32_t pin, uint8_t state);
uint8_t Hardware_GPIO_Read(uint32_t pin);
void Hardware_GPIO_Init(uint32_t pin, uint8_t direction);

/* Hardware abstracted PN532 functions */
void Hardware_PN532_Reset(void);
void Hardware_PN532_Delay_MS(uint32_t milliseconds);

/* Hardware abstracted SPI functions */
uint32_t Hardware_SPI_SetBaudrate(SPI_ACCESS_IDS_ENUM spi_id, uint32_t baudrate);
void Hardware_SPI_SetFormat(SPI_ACCESS_IDS_ENUM spi_id, uint8_t data_bits, uint8_t cpol, uint8_t cpha);
void Hardware_SPI_WriteByte(SPI_ACCESS_IDS_ENUM spi_id, uint8_t data);
void Hardware_SPI_ReadByte(SPI_ACCESS_IDS_ENUM spi_id, uint8_t dummy, uint8_t *data);
void Hardware_SPI_WriteBuffer(SPI_ACCESS_IDS_ENUM spi_id, const uint8_t *buffer, uint16_t length);
void Hardware_SPI_ReadBuffer(SPI_ACCESS_IDS_ENUM spi_id, uint8_t dummy, uint8_t *buffer, uint16_t length);

/* LCD Hardware Control Functions - Platform Abstracted */
void Hardware_LCD_Init_PWM_Backlight(void);
void Hardware_LCD_Set_Backlight_Brightness(uint8_t brightness);
void Hardware_LCD_Reset(void);
void Hardware_LCD_Set_DC(uint8_t state);
void Hardware_Init_LCD(void);
void Hardware_LED_On(void);
void Hardware_LED_Off(void);
void Hardware_Init(void);

/* GPIO Configuration Functions */
void Hardware_Set_All_Pins_Input(void);
void Hardware_Set_All_Pins_Input_Pullup(void);
void Hardware_Set_All_Pins_Input_Pulldown(void);

/* Interrupt Control Functions */
uint32_t Hardware_Disable_All_Interrupts(void);
void Hardware_Enable_All_Interrupts(uint32_t previous_state);
void Hardware_Force_Disable_All_Interrupts(void);

/* I2C Protection Functions - Thread-Safe I2C Bus Access */
typedef enum {
    HARDWARE_I2C_OK = 0,
    HARDWARE_I2C_ERROR = -1,
    HARDWARE_I2C_TIMEOUT = -2
} Hardware_I2C_Status_t;

#define HARDWARE_I2C_TIMEOUT_MS 5000  // 5000ms timeout for I2C operations (shared bus with CAT9555 and heartbeat tasks)

bool Hardware_IsI2CInitialized(void);
Hardware_I2C_Status_t Hardware_I2C_Init(void);
int Hardware_I2C_Write_Protected(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop);
int Hardware_I2C_Read_Protected(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop);
int Hardware_I2C_WriteRead_Protected(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);
SemaphoreHandle_t Hardware_I2C_GetMutex(void);

/* GPIO Interrupt System */
void Hardware_Init_GPIO_Interrupts(void);

/* CAT9555 Interrupt GPIO Configuration */
void Hardware_CAT9555_Init_Interrupt_GPIO(gpio_irq_callback_t callback);

/* I2C Bus Recovery Function */
/**
 * @brief Recover stuck I2C bus by toggling clock line
 * This can help if a slave device is holding SDA low
 */
void Hardware_I2C_BusRecovery(void);
void Hardware_I2C_Deinit(void);

#endif /* APPLICATION_INCLUDE_HARDWARE_ACCESS_H_ */
