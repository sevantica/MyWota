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

/* Hardware Pin Definitions for Driver Access */
#define PCD_RST_PIN                    14  // GPIO 14 - NFC/RFID Reset Pin

/* ===================================================================== */
/* END PIN DEFINITIONS                                                   */
/* ===================================================================== */

/*Typedefs -----------------------------------------------------------*/

#if defined(PICO_BOARD) || defined(PICO_BUILD)

/* LCD Module Pin Configuration */
typedef struct {
    uint32_t dc_pin;           // Data/Command control pin
    uint32_t reset_pin;        // Hardware reset pin
    uint32_t backlight_pin;    // Backlight control pin
    uint32_t cs_pin;           // Chip select pin
    uint32_t sck_pin;          // SPI clock pin
    uint32_t mosi_pin;         // SPI MOSI pin
    uint32_t miso_pin;         // SPI MISO pin
    void *spi_instance;        // SPI peripheral instance (spi0 or spi1)
} LCD_Pins_t;

/* NFC/RFID Module Pin Configuration */
typedef struct {
    uint32_t rst_pin;          // Hardware reset pin
    uint32_t i2c_sda_pin;      // I2C data pin
    uint32_t i2c_scl_pin;      // I2C clock pin
    void *i2c_instance;        // I2C peripheral instance (i2c0 or i2c1)
    uint8_t i2c_address;       // I2C device address
} NFC_Pins_t;

/* SD Card Module Pin Configuration */
typedef struct {
    uint32_t cs_pin;           // Chip select pin
    uint32_t sck_pin;          // SPI clock pin
    uint32_t mosi_pin;         // SPI MOSI pin
    uint32_t miso_pin;         // SPI MISO pin
    void *spi_instance;        // SPI peripheral instance (spi0 or spi1)
} SD_Card_Pins_t;

/* I/O Expander Module Pin Configuration */
typedef struct {
    uint8_t i2c_address;       // I2C device address
    uint32_t i2c_sda_pin;      // I2C data pin
    uint32_t i2c_scl_pin;      // I2C clock pin
    uint32_t int_pin;          // Interrupt pin
    void *i2c_instance;        // I2C peripheral instance (i2c0 or i2c1)
} IO_Expander_Pins_t;

/* Application GPIO Pin Configuration */
typedef struct {
    uint32_t system_comm_led_pin;   // System communication LED
    uint32_t exp_intr_pin;          // Expander interrupt pin
    uint32_t rs485_data_en_pin;     // RS485 data enable pin
    uint32_t light_sensor_pin;      // Light sensor input
    uint32_t flow_sensor_pin;       // Flow sensor input
    uint32_t valve_control_pin;     // Valve control output
    uint32_t pico_led_pin;          // Built-in Pico LED
} App_GPIO_Pins_t;

#endif // PICO_BOARD || PICO_BUILD

/*Defines ------------------------------------------------------------*/


/*Macros -------------------------------------------------------------*/

/* ===================================================================== */
/* Function Declarations                                                */
/* ===================================================================== */

#if defined(PICO_BOARD) || defined(PICO_BUILD)

/* Hardware Layer Initialization */
void Init_Hardware_Layer(void);

/* Generic Peripheral Initialization Functions */
void Init_SPI_0(uint32_t baudrate);
void Init_I2C_0(uint32_t baudrate);
void Init_I2C_1(uint32_t baudrate);
void Init_UART_0(uint32_t baudrate);

/* GPIO Initialization Functions */
void Init_GPIO_Output(uint32_t gpio);
void Init_GPIO_Input(uint32_t gpio);
void Init_GPIO_Input_PullUp(uint32_t gpio);
void Init_GPIO_Input_PullDown(uint32_t gpio);

/* GPIO Control Functions */
void Hardware_GPIO_Set_State(uint32_t gpio, bool state);
void Hardware_LED_On(void);
void Hardware_LED_Off(void);
void Hardware_Init_GPIO_Interrupts(void);

/* PWM Control Functions */
bool GPIO_Is_PWM_Capable(uint32_t gpio);
bool Set_GPIO_PWM(uint32_t gpio, uint16_t duty_cycle, uint32_t frequency);
bool Set_GPIO_PWM_Duty(uint32_t gpio, uint16_t duty_cycle);
bool Set_GPIO_PWM_Percent(uint32_t gpio, uint8_t percent);

/* SPI Bus Control Functions */
bool SPI_0_Acquire(void);
void SPI_0_Release(void);
void SPI_0_WriteByte_Raw(uint8_t data);
uint8_t SPI_0_ReadByte_Raw(uint8_t repeated_tx_data);
void SPI_0_WriteBuffer_Raw(const uint8_t *src, size_t len);
void SPI_0_ReadBuffer_Raw(uint8_t repeated_tx_data, uint8_t *dst, size_t len);
int SPI_0_Write(uint32_t cs_pin, const uint8_t *src, size_t len);
int SPI_0_Read(uint32_t cs_pin, uint8_t repeated_tx_data, uint8_t *dst, size_t len);
uint32_t SPI_0_SetBaudrate(uint32_t baudrate);
void SPI_0_SetFormat(uint8_t data_bits, uint8_t cpol, uint8_t cpha);

/* I2C Bus Control Functions - I2C0 */
bool I2C_0_Acquire(void);
void I2C_0_Release(void);
int I2C_0_WriteByte_Raw(uint8_t addr, uint8_t data);
int I2C_0_ReadByte_Raw(uint8_t addr, uint8_t *data);
int I2C_0_WriteBuffer_Raw(uint8_t addr, const uint8_t *src, size_t len, bool nostop);
int I2C_0_ReadBuffer_Raw(uint8_t addr, uint8_t *dst, size_t len, bool nostop);
int I2C_0_Write(uint8_t addr, const uint8_t *src, size_t len, bool nostop);
int I2C_0_Read(uint8_t addr, uint8_t *dst, size_t len, bool nostop);
int I2C_0_WriteRead(uint8_t addr, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

/* Module-Specific Hardware Initialization Functions */
void init_lcd_hw(void);
void init_pcd_hw(void);
void init_sd_hw(void);
void init_io_expander_hw(void);
void init_application_gpio_hw(void);

/* Hardware Control Functions */
void Hardware_GPIO_Set_State(uint32_t gpio, bool state);

/* Module Pin Configuration Functions */
LCD_Pins_t Get_LCD_Pins(void);
NFC_Pins_t Get_NFC_Pins(void);
SD_Card_Pins_t Get_SD_Card_Pins(void);
IO_Expander_Pins_t Get_IO_Expander_Pins(void);
App_GPIO_Pins_t Get_App_GPIO_Pins(void);

#endif // PICO_BOARD || PICO_BUILD

#endif /* APPLICATION_INCLUDE_HARDWARE_ACCESS_H_ */
