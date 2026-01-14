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

/* ===================================================================== */
/* HARDWARE PIN DEFINITIONS - RASPBERRY PI PICO (RP2040)                */
/* ===================================================================== */

#if defined(PICO_BOARD) || defined(PICO_BUILD)

#include <stdint.h>
#include <stdbool.h>
#include "Hardware_Access.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* ===================================================================== */
/* Hardware Configuration Constants                                     */
/* ===================================================================== */

#define SPI_0_BAUDRATE          62500000    // 62.5 MHz (RP2040 max) for LCD and SD card
#define I2C_0_BAUDRATE          400000      // 400 kHz for NFC/RFID and I/O expander
#define I2C_1_BAUDRATE          400000      // 400 kHz for secondary I2C bus

#define SPI_MUTEX_TIMEOUT_MS    1000        // Mutex timeout in milliseconds

/* ===================================================================== */
/* Generic GPIO Pin Assignments                                         */
/* ===================================================================== */

#define GPIO_0              0       // GPIO 0 (Physical pin 1)
#define GPIO_1              1       // GPIO 1 (Physical pin 2)
#define GPIO_2              2       // GPIO 2 (Physical pin 4)
#define GPIO_3              3       // GPIO 3 (Physical pin 5)
#define GPIO_4              4       // GPIO 4 (Physical pin 6)
#define GPIO_5              5       // GPIO 5 (Physical pin 7)
#define GPIO_6              6       // GPIO 6 (Physical pin 9)
#define GPIO_7              7       // GPIO 7 (Physical pin 10)
#define GPIO_8              8       // GPIO 8 (Physical pin 11)
#define GPIO_9              9       // GPIO 9 (Physical pin 12)
#define GPIO_13             13      // GPIO 13 (Physical pin 17)
#define GPIO_14             14      // GPIO 14 (Physical pin 19)
#define GPIO_15             15      // GPIO 15 (Physical pin 20)
#define GPIO_16             16      // GPIO 16 (Physical pin 21)
#define GPIO_17             17      // GPIO 17 (Physical pin 22)
#define GPIO_18             18      // GPIO 18 (Physical pin 24)
#define GPIO_19             19      // GPIO 19 (Physical pin 25)
#define GPIO_20             20      // GPIO 20 (Physical pin 26)
#define GPIO_21             21      // GPIO 21 (Physical pin 27)
#define GPIO_22             22      // GPIO 22 (Physical pin 29)
#define GPIO_25             25      // GPIO 25 - Built-in LED
#define GPIO_26             26      // GPIO 26 (Physical pin 31)
#define GPIO_27             27      // GPIO 27 (Physical pin 32)
#define GPIO_28             28      // GPIO 28 (Physical pin 34) - ADC2

/* ===================================================================== */
/* Generic SPI Pin Assignments                                          */
/* ===================================================================== */

/* SPI0 Peripheral Pins */
#define SPI_0_SCK           GPIO_18     // GPIO 18 (Physical pin 24) - SPI0_SCK
#define SPI_0_MOSI          GPIO_19     // GPIO 19 (Physical pin 25) - SPI0_TX (MOSI)
#define SPI_0_MISO          GPIO_16     // GPIO 16 (Physical pin 21) - SPI0_RX (MISO)
#define SPI_0_CS0           GPIO_17     // GPIO 17 (Physical pin 22) - SPI0_CS0
#define SPI_0_CS1           GPIO_13     // GPIO 13 (Physical pin 17) - SPI0_CS1
#define SPI_0               spi0        // SPI0 peripheral

/* ===================================================================== */
/* Generic I2C Pin Assignments                                          */
/* ===================================================================== */

/* I2C_0 - Primary I2C bus */
#define I2C_0_SCL           GPIO_21     // GPIO 21 (Physical pin 27) - I2C_0_SCL
#define I2C_0_SDA           GPIO_20     // GPIO 20 (Physical pin 26) - I2C_0_SDA
#define I2C_0               i2c0        // I2C0 peripheral

/* I2C_1 - Secondary I2C bus */
#define I2C_1_SCL           GPIO_27     // GPIO 27 (Physical pin 32) - I2C_1_SCL
#define I2C_1_SDA           GPIO_26     // GPIO 26 (Physical pin 31) - I2C_1_SDA
#define I2C_1               i2c1        // I2C1 peripheral

/* ===================================================================== */
/* Generic UART Pin Assignments                                         */
/* ===================================================================== */

#define UART_0_TX           GPIO_0      // GPIO 0 (Physical pin 1) - UART_0_TXD
#define UART_0_RX           GPIO_1      // GPIO 1 (Physical pin 2) - UART_0_RXD
#define UART_1_TX           GPIO_4      // GPIO 4 (Physical pin 6) - UART_1_TXD
#define UART_1_RX           GPIO_5      // GPIO 5 (Physical pin 7) - UART_1_RXD

/* ===================================================================== */
/* Module-Specific Pin Assignments (using generic aliases)             */
/* ===================================================================== */

/* LCD Display Module Pins */
#define LCD_DC_PIN          GPIO_2
#define LCD_RESET_PIN       GPIO_3
#define LCD_BACKLIGHT_PIN   GPIO_7
#define LCD_CS_PIN          SPI_0_CS0
#define LCD_SCK_PIN         SPI_0_SCK
#define LCD_MOSI_PIN        SPI_0_MOSI
#define LCD_MISO_PIN        SPI_0_MISO
#define LCD_SPI             SPI_0

/* NFC/RFID Module Pins */
#define PCD_I2C_SDA_PIN     I2C_0_SDA
#define PCD_I2C_SCL_PIN     I2C_0_SCL
#define PCD_I2C             I2C_0

/* SD Card Module Pins (shares SPI0 with LCD) */
#define SD_CS_PIN           SPI_0_CS1
#define SD_SCK_PIN          SPI_0_SCK
#define SD_MOSI_PIN         SPI_0_MOSI
#define SD_MISO_PIN         SPI_0_MISO
#define SD_SPI              SPI_0

/* UART Module Pins */
#define UART_0_TXD_PIN      UART_0_TX
#define UART_0_RXD_PIN      UART_0_RX
#define UART_1_TXD_PIN      UART_1_TX
#define UART_1_RXD_PIN      UART_1_RX

/* Application-Specific GPIO Pins */
#define SYSTEM_COMM_LED_PIN 6//////////
#define EXP_INTR_PIN        8///////////
#define RS485_DATA_EN_PIN   9
#define LIGHT_SENSOR_PIN    28
#define FLOW_SENSOR_PIN     22
#define VALVE_CONTROL_PIN   15
#define PICO_LED_PIN        25///////////

/* I/O Expander Module Pins */
#define IO_EXPANDER_I2C_ADDRESS     0x27
#define IO_EXPANDER_I2C_SCL_PIN     I2C_0_SCL
#define IO_EXPANDER_I2C_SDA_PIN     I2C_0_SDA
#define IO_EXPANDER_INT_PIN         UART_0_RX  // GPIO 1 (Physical pin 2)

/* ===================================================================== */
/* Generic Peripheral Initialization Functions                          */
/* ===================================================================== */

/* Static flags to track peripheral initialization state */
static bool spi_0_initialized = false;
static bool i2c_0_initialized = false;
static bool i2c_1_initialized = false;
static uint8_t pwm_slice_initialized = 0;  // Bitmask for initialized PWM slices (8 slices)

/* DMA channel for SPI TX */
static int spi_0_dma_tx_channel = -1;

/* SPI mutex for thread-safe access */
static SemaphoreHandle_t spi_0_mutex = NULL;

/* I2C mutexes for thread-safe access */
static SemaphoreHandle_t i2c_0_mutex = NULL;
static SemaphoreHandle_t i2c_1_mutex = NULL;

/**
 * @brief Initialize hardware abstraction layer resources
 * Creates mutexes and prepares system for peripheral initialization
 * @note Must be called before any peripheral initialization functions
 */
void Init_Hardware_Layer(void)
{
    // Create SPI mutex for thread-safe bus access
    if (spi_0_mutex == NULL) {
        spi_0_mutex = xSemaphoreCreateMutex();
    }
    
    // Create I2C mutexes for thread-safe bus access
    if (i2c_0_mutex == NULL) {
        i2c_0_mutex = xSemaphoreCreateMutex();
    }
    if (i2c_1_mutex == NULL) {
        i2c_1_mutex = xSemaphoreCreateMutex();
    }
}

/**
 * @brief Initialize SPI0 peripheral with DMA support
 * @param baudrate SPI clock frequency in Hz
 */
void Init_SPI_0(uint32_t baudrate)
{
    if (spi_0_initialized) {
        return;  // Already initialized
    }
    
    spi_init(SPI_0, baudrate);
    gpio_set_function(SPI_0_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SPI_0_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SPI_0_MISO, GPIO_FUNC_SPI);
    
    // Claim a DMA channel for SPI TX
    spi_0_dma_tx_channel = dma_claim_unused_channel(true);
    
    // Configure DMA channel for SPI TX
    dma_channel_config c = dma_channel_get_default_config(spi_0_dma_tx_channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(SPI_0, true));  // TX DREQ
    channel_config_set_read_increment(&c, true);   // Increment read address
    channel_config_set_write_increment(&c, false); // Fixed write address (SPI DR)
    
    // Save config for later use (will be applied when writing)
    dma_channel_set_config(spi_0_dma_tx_channel, &c, false);
    dma_channel_set_write_addr(spi_0_dma_tx_channel, &spi_get_hw(SPI_0)->dr, false);
    
    spi_0_initialized = true;
}

/**
 * @brief Acquire exclusive access to SPI0 bus
 * @return true if mutex acquired successfully, false on timeout
 * @note For complex protocols requiring manual CS control. Must call SPI_0_Release() when done.
 */
bool SPI_0_Acquire(void)
{
    if (spi_0_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(spi_0_mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    USB_Log_Printf("[SPI] TIMEOUT acquiring mutex from %s!\r\n", pcTaskGetName(NULL));
    return false;
}

/**
 * @brief Release exclusive access to SPI0 bus
 * @note Must be called after SPI_0_Acquire() when operations are complete
 */
void SPI_0_Release(void)
{
    if (spi_0_mutex != NULL) {
        xSemaphoreGive(spi_0_mutex);
    }
}

/**
 * @brief Write single byte to SPI0 (no CS control, no mutex - caller must acquire first)
 * @param data Byte to write
 */
void SPI_0_WriteByte_Raw(uint8_t data)
{
    spi_write_blocking(SPI_0, &data, 1);
}

/**
 * @brief Read single byte from SPI0 (no CS control, no mutex - caller must acquire first)
 * @param repeated_tx_data Byte to write during read
 * @return Byte read
 */
uint8_t SPI_0_ReadByte_Raw(uint8_t repeated_tx_data)
{
    uint8_t data;
    spi_read_blocking(SPI_0, repeated_tx_data, &data, 1);
    return data;
}

/**
 * @brief Write buffer to SPI0 using DMA (no CS control, no mutex - caller must acquire first)
 * @param src Source buffer
 * @param len Number of bytes to write
 * @note Uses DMA for transfers >= 32 bytes, blocking for smaller transfers
 */
void SPI_0_WriteBuffer_Raw(const uint8_t *src, size_t len)
{
    // Use DMA for larger transfers (reduces CPU load during display updates)
    if (spi_0_dma_tx_channel >= 0 && len >= 32) {
        dma_channel_set_read_addr(spi_0_dma_tx_channel, src, false);
        dma_channel_set_trans_count(spi_0_dma_tx_channel, len, true);  // Start transfer
        
        /* Optimization: Yield CPU if transfer is long enough */
        uint32_t actual_baud = spi_get_baudrate(SPI_0);
        if (actual_baud > 0) {
            /* Calculate duration in ms: (bytes * 8 bits * 1000 ms/s) / baudrate */
            uint32_t duration_ms = (len * 8000U) / actual_baud;
            if (duration_ms > 1) {
                /* Yield for the estimated duration. 
                 * The final blocking wait will catch any small remainder. */
                vTaskDelay(pdMS_TO_TICKS(duration_ms));
            }
        }

        dma_channel_wait_for_finish_blocking(spi_0_dma_tx_channel);
        
        // Wait for SPI FIFO to drain completely
        while (spi_is_busy(SPI_0)) {
            tight_loop_contents();
        }
    } else {
        // Small transfers use blocking (faster for small data)
        spi_write_blocking(SPI_0, src, len);
    }
}

/**
 * @brief Read buffer from SPI0 (no CS control, no mutex - caller must acquire first)
 * @param repeated_tx_data Byte to write repeatedly during read
 * @param dst Destination buffer
 * @param len Number of bytes to read
 */
void SPI_0_ReadBuffer_Raw(uint8_t repeated_tx_data, uint8_t *dst, size_t len)
{
    spi_read_blocking(SPI_0, repeated_tx_data, dst, len);
}

/**
 * @brief Set SPI0 baudrate
 * @param baudrate Desired baudrate in Hz
 * @return Actual baudrate set
 */
uint32_t SPI_0_SetBaudrate(uint32_t baudrate)
{
    return spi_set_baudrate(SPI_0, baudrate);
}

/**
 * @brief Set SPI0 format
 * @param data_bits Number of data bits (4-16)
 * @param cpol Clock polarity (0 or 1)
 * @param cpha Clock phase (0 or 1)
 */
void SPI_0_SetFormat(uint8_t data_bits, uint8_t cpol, uint8_t cpha)
{
    spi_cpol_t spi_cpol = (cpol == 0) ? SPI_CPOL_0 : SPI_CPOL_1;
    spi_cpha_t spi_cpha = (cpha == 0) ? SPI_CPHA_0 : SPI_CPHA_1;
    spi_set_format(SPI_0, data_bits, spi_cpol, spi_cpha, SPI_MSB_FIRST);
}

/**
 * @brief Generic I2C initialization helper
 * @param instance I2C peripheral instance (I2C_0 or I2C_1)
 * @param sda_pin SDA pin number
 * @param scl_pin SCL pin number
 * @param initialized_flag Pointer to initialization flag
 * @param baudrate I2C clock frequency in Hz
 */
static void Init_I2C_Generic(i2c_inst_t *instance, uint sda_pin, uint scl_pin, 
                             bool *initialized_flag, uint32_t baudrate)
{
    if (*initialized_flag) {
        return;  // Already initialized
    }
    
    i2c_init(instance, baudrate);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda_pin);
    gpio_pull_up(scl_pin);
    
    *initialized_flag = true;
}

/**
 * @brief Initialize I2C0 peripheral
 * @param baudrate I2C clock frequency in Hz (typically 100000 or 400000)
 */
void Init_I2C_0(uint32_t baudrate)
{
    Init_I2C_Generic(I2C_0, I2C_0_SDA, I2C_0_SCL, &i2c_0_initialized, baudrate);
}

/**
 * @brief Initialize I2C1 peripheral
 * @param baudrate I2C clock frequency in Hz (typically 100000 or 400000)
 */
void Init_I2C_1(uint32_t baudrate)
{
    Init_I2C_Generic(I2C_1, I2C_1_SDA, I2C_1_SCL, &i2c_1_initialized, baudrate);
}

/**
 * @brief Release exclusive access to I2C0 bus
 * @note Must be called after I2C_0_Acquire() when operations are complete
 */
void I2C_0_Release(void)
{
    if (i2c_0_mutex != NULL) {
        xSemaphoreGive(i2c_0_mutex);
    }
}

/**
 * @brief Write single byte to I2C0 (no mutex - caller must acquire first)
 * @param addr 7-bit I2C device address
 * @param data Byte to write
 * @return Number of bytes written, or PICO_ERROR_GENERIC on error
 */
int I2C_0_WriteByte_Raw(uint8_t addr, uint8_t data)
{
    return i2c_write_blocking(I2C_0, addr, &data, 1, false);
}

/**
 * @brief Read single byte from I2C0 (no mutex - caller must acquire first)
 * @param addr 7-bit I2C device address
 * @param data Pointer to store read byte
 * @return Number of bytes read, or PICO_ERROR_GENERIC on error
 */
int I2C_0_ReadByte_Raw(uint8_t addr, uint8_t *data)
{
    return i2c_read_blocking(I2C_0, addr, data, 1, false);
}

/**
 * @brief Write buffer to I2C0 (no mutex - caller must acquire first)
 * @param addr 7-bit I2C device address
 * @param src Source buffer
 * @param len Number of bytes to write
 * @param nostop If true, master retains control of the bus at the end of the transfer
 * @return Number of bytes written, or PICO_ERROR_GENERIC on error
 */
int I2C_0_WriteBuffer_Raw(uint8_t addr, const uint8_t *src, size_t len, bool nostop)
{
    return i2c_write_blocking(I2C_0, addr, src, len, nostop);
}

/**
 * @brief Read buffer from I2C0 (no mutex - caller must acquire first)
 * @param addr 7-bit I2C device address
 * @param dst Destination buffer
 * @param len Number of bytes to read
 * @param nostop If true, master retains control of the bus at the end of the transfer
 * @return Number of bytes read, or PICO_ERROR_GENERIC on error
 */
int I2C_0_ReadBuffer_Raw(uint8_t addr, uint8_t *dst, size_t len, bool nostop)
{
    return i2c_read_blocking(I2C_0, addr, dst, len, nostop);
}

/**
 * @brief Write data to I2C0 bus (thread-safe)
 * @param addr 7-bit I2C device address
 * @param src Source buffer containing data to write
 * @param len Number of bytes to write
 * @param nostop If true, master retains control of the bus at the end of the transfer
 * @return Number of bytes written, or -1 on error
 */
int I2C_0_Write(uint8_t addr, const uint8_t *src, size_t len, bool nostop)
{
    if (i2c_0_mutex == NULL || src == NULL) {
        return -1;
    }
    
    if (xSemaphoreTake(i2c_0_mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return -1;  // Timeout acquiring mutex
    }
    
    int result = i2c_write_blocking(I2C_0, addr, src, len, nostop);
    
    xSemaphoreGive(i2c_0_mutex);
    return result;
}

/**
 * @brief Read data from I2C0 bus (thread-safe)
 * @param addr 7-bit I2C device address
 * @param dst Destination buffer for received data
 * @param len Number of bytes to read
 * @param nostop If true, master retains control of the bus at the end of the transfer
 * @return Number of bytes read, or -1 on error
 */
int I2C_0_Read(uint8_t addr, uint8_t *dst, size_t len, bool nostop)
{
    if (i2c_0_mutex == NULL || dst == NULL) {
        return -1;
    }
    
    if (xSemaphoreTake(i2c_0_mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return -1;  // Timeout acquiring mutex
    }
    
    int result = i2c_read_blocking(I2C_0, addr, dst, len, nostop);
    
    xSemaphoreGive(i2c_0_mutex);
    return result;
}

/**
 * @brief Write then read data on I2C0 bus in single transaction (thread-safe)
 * @param addr 7-bit I2C device address
 * @param src Source buffer containing data to write
 * @param src_len Number of bytes to write
 * @param dst Destination buffer for received data
 * @param dst_len Number of bytes to read
 * @return Number of bytes read, or -1 on error
 */
int I2C_0_WriteRead(uint8_t addr, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    if (i2c_0_mutex == NULL || src == NULL || dst == NULL) {
        return -1;
    }
    
    if (xSemaphoreTake(i2c_0_mutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return -1;  // Timeout acquiring mutex
    }
    
    // Write with nostop=true to keep bus control
    int write_result = i2c_write_blocking(I2C_0, addr, src, src_len, true);
    if (write_result < 0) {
        xSemaphoreGive(i2c_0_mutex);
        return -1;
    }
    
    // Read with nostop=false to release bus
    int result = i2c_read_blocking(I2C_0, addr, dst, dst_len, false);
    
    xSemaphoreGive(i2c_0_mutex);
    return result;
}

/**
 * @brief Initialize a GPIO pin as output
 * @param gpio GPIO pin number
 */
void Init_GPIO_Output(uint32_t gpio)
{
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_OUT);
}

/**
 * @brief Initialize a GPIO pin as input
 * @param gpio GPIO pin number
 */
void Init_GPIO_Input(uint32_t gpio)
{
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
}

/**
 * @brief Initialize a GPIO pin as input with pull-up
 * @param gpio GPIO pin number
 */
void Init_GPIO_Input_PullUp(uint32_t gpio)
{
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}

/**
 * @brief Initialize a GPIO pin as input with pull-down
 * @param gpio GPIO pin number
 */
void Init_GPIO_Input_PullDown(uint32_t gpio)
{
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_down(gpio);
}

/**
 * @brief Set GPIO pin state (generic GPIO control)
 * @param gpio GPIO pin number
 * @param state true for HIGH, false for LOW
 */
void Hardware_GPIO_Set_State(uint32_t gpio, bool state)
{
    gpio_put(gpio, state ? 1 : 0);
}

/**
 * @brief Turn on onboard LED (GPIO 25 on Pico)
 */
void Hardware_LED_On(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

/**
 * @brief Turn off onboard LED (GPIO 25 on Pico)
 */
void Hardware_LED_Off(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

/**
 * @brief Initialize GPIO interrupts (stub for future implementation)
 */
void Hardware_Init_GPIO_Interrupts(void)
{
    // TODO: Initialize GPIO interrupt handlers as needed
    // For now, this is a stub to allow compilation
}

/* ===================================================================== */
/* PWM Control Functions                                                */
/* ===================================================================== */

/**
 * @brief Check if a GPIO pin supports PWM
 * @param gpio GPIO pin number
 * @return true if pin supports PWM, false otherwise
 */
bool GPIO_Is_PWM_Capable(uint32_t gpio)
{
    // RP2040 GPIO 0-28 are all PWM capable
    // GPIO 29 is used for ADC only on some boards
    return (gpio <= 28);
}

/**
 * @brief Initialize and set PWM on a GPIO pin
 * @param gpio GPIO pin number (0-28)
 * @param duty_cycle PWM duty cycle (0-65535, where 65535 = 100%)
 * @param frequency PWM frequency in Hz (default 1000 Hz if 0)
 * @return true if successful, false on error
 */
bool Set_GPIO_PWM(uint32_t gpio, uint16_t duty_cycle, uint32_t frequency)
{
    // Validate GPIO pin
    if (!GPIO_Is_PWM_Capable(gpio)) {
        return false;
    }
    
    // Get PWM slice and channel for this GPIO
    uint slice_num = pwm_gpio_to_slice_num(gpio);
    
    // Check if this slice has been initialized
    bool slice_is_initialized = (pwm_slice_initialized & (1 << slice_num)) != 0;
    
    if (!slice_is_initialized) {
        // Configure PWM frequency
        if (frequency == 0) {
            frequency = 1000;  // Default 1kHz
        }
        
        // Calculate divider for desired frequency
        // PWM clock = sys_clock / divider
        // PWM frequency = PWM_clock / wrap_value
        // Using wrap = 65535 for maximum resolution
        float clock_div = (float)clock_get_hz(clk_sys) / (frequency * 65536.0f);
        
        // Set PWM configuration
        pwm_config config = pwm_get_default_config();
        pwm_config_set_clkdiv(&config, clock_div);
        pwm_config_set_wrap(&config, 65535);  // 16-bit resolution
        
        // Initialize PWM slice but DON'T start it yet (false = don't start)
        pwm_init(slice_num, &config, false);
        
        // Set duty cycle BEFORE starting PWM and switching GPIO function
        // This ensures no glitch when the GPIO transitions to PWM mode
        pwm_set_gpio_level(gpio, duty_cycle);
        
        // NOW start the PWM running
        pwm_set_enabled(slice_num, true);
        
        // Finally switch GPIO to PWM function (output should already be at correct level)
        gpio_set_function(gpio, GPIO_FUNC_PWM);
        
        // Mark slice as initialized
        pwm_slice_initialized |= (1 << slice_num);
        
        return true;
    }
    
    // Set duty cycle
    pwm_set_gpio_level(gpio, duty_cycle);
    
    return true;
}

/**
 * @brief Set PWM duty cycle on an already initialized GPIO
 * @param gpio GPIO pin number
 * @param duty_cycle PWM duty cycle (0-65535)
 * @return true if successful, false on error
 */
bool Set_GPIO_PWM_Duty(uint32_t gpio, uint16_t duty_cycle)
{
    if (!GPIO_Is_PWM_Capable(gpio)) {
        return false;
    }
    
    uint slice_num = pwm_gpio_to_slice_num(gpio);
    
    // Check if slice is initialized
    if ((pwm_slice_initialized & (1 << slice_num)) == 0) {
        // Not initialized, initialize with default frequency
        return Set_GPIO_PWM(gpio, duty_cycle, 1000);
    }
    
    pwm_set_gpio_level(gpio, duty_cycle);
    return true;
}

/**
 * @brief Set PWM duty cycle as percentage (0-100)
 * @param gpio GPIO pin number
 * @param percent Duty cycle percentage (0-100)
 * @return true if successful, false on error
 */
bool Set_GPIO_PWM_Percent(uint32_t gpio, uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    
    // Convert percentage to 16-bit value
    uint16_t duty_cycle = (uint16_t)((uint32_t)percent * 65535 / 100);
    
    return Set_GPIO_PWM_Duty(gpio, duty_cycle);
}

/* ===================================================================== */
/* Module-Specific Hardware Initialization Functions                    */
/* ===================================================================== */

/**
 * @brief Initialize LCD hardware
 * Configures SPI0, control pins, and backlight for LCD module
 */
void init_lcd_hw(void)
{
    // Initialize SPI0 at 10MHz for LCD
    Init_SPI_0(SPI_0_BAUDRATE);
    
    // Initialize LCD CS pin
    Init_GPIO_Output(LCD_CS_PIN);
    gpio_put(LCD_CS_PIN, 1);  // CS inactive (high)
    
    // Initialize LCD control pins
    Init_GPIO_Output(LCD_DC_PIN);
    Init_GPIO_Output(LCD_RESET_PIN);
    
    // Initialize backlight as PWM at 100% duty cycle (backlight OFF)
    // Hardware uses inverted logic: 100% PWM = backlight off, 0% PWM = backlight on
    Set_GPIO_PWM_Percent(LCD_BACKLIGHT_PIN, 100);
    
    // Set default states
    gpio_put(LCD_DC_PIN, 1);           // Data mode (default HIGH to prevent glitches)
    gpio_put(LCD_RESET_PIN, 0);        // Not in reset (inverted in HW)
}

/**
 * @brief Initialize NFC/RFID hardware
 * Configures I2C0 and reset pin for NFC/RFID module
 */
void init_pcd_hw(void)
{
    // Initialize I2C0 at 400kHz
    Init_I2C_0(I2C_0_BAUDRATE);
    
    // Initialize reset pin
    Init_GPIO_Output(PCD_RST_PIN);
    gpio_put(PCD_RST_PIN, 1);  // Default state
}

/* ===================================================================== */
/* Module Pin Configuration Functions                                    */
/* ===================================================================== */

/**
 * @brief Get LCD module pin configuration
 * @return LCD_Pins_t struct containing all LCD pin assignments
 */
LCD_Pins_t Get_LCD_Pins(void)
{
    LCD_Pins_t pins = {
        .dc_pin = LCD_DC_PIN,
        .reset_pin = LCD_RESET_PIN,
        .backlight_pin = LCD_BACKLIGHT_PIN,
        .cs_pin = LCD_CS_PIN,
        .sck_pin = LCD_SCK_PIN,
        .mosi_pin = LCD_MOSI_PIN,
        .miso_pin = LCD_MISO_PIN,
        .spi_instance = LCD_SPI
    };
    return pins;
}

/**
 * @brief Get NFC/RFID module pin configuration
 * @return NFC_Pins_t struct containing all NFC/RFID pin assignments
 */
NFC_Pins_t Get_NFC_Pins(void)
{
    NFC_Pins_t pins = {
        .rst_pin = PCD_RST_PIN,
        .i2c_sda_pin = PCD_I2C_SDA_PIN,
        .i2c_scl_pin = PCD_I2C_SCL_PIN,
        .i2c_instance = PCD_I2C,
        .i2c_address = 0x24  // PN532 I2C address
    };
    return pins;
}

/**
 * @brief Get SD Card module pin configuration
 * @return SD_Card_Pins_t struct containing all SD Card pin assignments
 */
SD_Card_Pins_t Get_SD_Card_Pins(void)
{
    SD_Card_Pins_t pins = {
        .cs_pin = SD_CS_PIN,
        .sck_pin = SD_SCK_PIN,
        .mosi_pin = SD_MOSI_PIN,
        .miso_pin = SD_MISO_PIN,
        .spi_instance = SD_SPI
    };
    return pins;
}

/**
 * @brief Get I/O Expander module pin configuration
 * @return IO_Expander_Pins_t struct containing all I/O Expander pin assignments
 */
IO_Expander_Pins_t Get_IO_Expander_Pins(void)
{
    IO_Expander_Pins_t pins = {
        .i2c_address = IO_EXPANDER_I2C_ADDRESS,
        .i2c_sda_pin = IO_EXPANDER_I2C_SDA_PIN,
        .i2c_scl_pin = IO_EXPANDER_I2C_SCL_PIN,
        .int_pin = IO_EXPANDER_INT_PIN,
        .i2c_instance = I2C_0
    };
    return pins;
}

/**
 * @brief Get Application GPIO pin configuration
 * @return App_GPIO_Pins_t struct containing all application GPIO assignments
 */
App_GPIO_Pins_t Get_App_GPIO_Pins(void)
{
    App_GPIO_Pins_t pins = {
        .system_comm_led_pin = SYSTEM_COMM_LED_PIN,
        .exp_intr_pin = EXP_INTR_PIN,
        .rs485_data_en_pin = RS485_DATA_EN_PIN,
        .light_sensor_pin = LIGHT_SENSOR_PIN,
        .flow_sensor_pin = FLOW_SENSOR_PIN,
        .valve_control_pin = VALVE_CONTROL_PIN,
        .pico_led_pin = PICO_LED_PIN
    };
    return pins;
}

/**
 * @brief Initialize SD Card hardware
 * Configures SPI0 (shared with LCD) and CS pin for SD card
 * Note: SPI0 is shared with LCD and will be initialized if not already done
 */
void init_sd_hw(void)
{
    // SPI0 is shared with LCD, initialize if not already done
    Init_SPI_0(SPI_0_BAUDRATE);  // 10MHz default for SD card
    
    // Initialize SD card CS pin
    Init_GPIO_Output(SD_CS_PIN);
    gpio_put(SD_CS_PIN, 1);  // CS inactive (high)
}

/**
 * @brief Initialize I/O Expander hardware
 * Configures I2C0 (shared with NFC/RFID) and interrupt pin
 * Note: I2C0 is shared with NFC/RFID and will be initialized if not already done
 */
void init_io_expander_hw(void)
{
    // I2C0 is shared with NFC/RFID, initialize if not already done
    Init_I2C_0(I2C_0_BAUDRATE);  // 400kHz for I/O expander
    
    // Initialize interrupt pin as input with pull-up
    Init_GPIO_Input_PullUp(IO_EXPANDER_INT_PIN);
}

/**
 * @brief Initialize application-specific GPIO pins
 * Configures system LED, sensor inputs, and control outputs
 */
void init_application_gpio_hw(void)
{
    // System LEDs
    Init_GPIO_Output(SYSTEM_COMM_LED_PIN);
    Init_GPIO_Output(PICO_LED_PIN);
    
    // Control outputs
    Init_GPIO_Output(RS485_DATA_EN_PIN);
    Init_GPIO_Output(VALVE_CONTROL_PIN);
    
    // Sensor inputs
    Init_GPIO_Input(FLOW_SENSOR_PIN);
    Init_GPIO_Input(LIGHT_SENSOR_PIN);
    Init_GPIO_Input(EXP_INTR_PIN);
    
    // Set default states for outputs
    gpio_put(SYSTEM_COMM_LED_PIN, 0);
    gpio_put(PICO_LED_PIN, 0);
    gpio_put(RS485_DATA_EN_PIN, 0);
    gpio_put(VALVE_CONTROL_PIN, 0);
}

#endif // PICO_BOARD || PICO_BUILD

/* ===================================================================== */
/* HARDWARE PIN DEFINITIONS - STM32F411xE                               */
/* ===================================================================== */

#if defined(STM32F411xE)

/* 
 * Note: STM32 pin definitions are typically managed by STM32CubeMX 
 * and defined in main.h. The following are logical mappings for 
 * hardware abstraction layer.
 * 
 * Physical pins are defined in main.h as:
 * - LCD_CS_Pin, LCD_CS_GPIO_Port           (GPIOA, PIN_0)
 * - PCD_IRQ_Pin, PCD_IRQ_GPIO_Port         (GPIOA, PIN_8)
 * - LCD_BACKLIGHT_Pin, LCD_BACKLIGHT_GPIO_Port (GPIOA, PIN_9)
 * - LCD_DC_Pin, LCD_DC_GPIO_Port           (GPIOB, PIN_4)
 * - LCD_RESET_Pin, LCD_RESET_GPIO_Port     (GPIOB, PIN_5)
 * - PCD_RST_Pin, PCD_RST_GPIO_Port         (GPIOB, PIN_9)
 * 
 * SPI peripherals:
 * - SPI1: LCD Display
 * - SPI2: RFID/NFC (RC522 or PN532)
 * 
 * I2C peripherals:
 * - I2C1: Primary I2C bus (NFC/RFID, sensors)
 * - I2C2: Secondary I2C bus (if available)
 */

/* Platform-abstracted pin identifiers */
#define HW_PIN_LCD_CS               0  // Maps to LCD_CS_Pin (GPIOA, PIN_0)
#define HW_PIN_PCD_IRQ              1  // Maps to PCD_IRQ_Pin (GPIOA, PIN_8) 
#define HW_PIN_LCD_BACKLIGHT        2  // Maps to LCD_BACKLIGHT_Pin (GPIOA, PIN_9)
#define HW_PIN_LCD_DC               3  // Maps to LCD_DC_Pin (GPIOB, PIN_4)
#define HW_PIN_LCD_RESET            4  // Maps to LCD_RESET_Pin (GPIOB, PIN_5)
#define HW_PIN_PCD_RST              5  // Maps to PCD_RST_Pin (GPIOB, PIN_9)
#define HW_PIN_NFC_RST              HW_PIN_PCD_RST  // Alias for NFC/RFID reset pin

/* Note: Actual GPIO port/pin assignments are configured in STM32CubeMX 
 * and defined in main.h generated by the IDE */

#endif // STM32F411xE

/* ===================================================================== */
/* END PIN DEFINITIONS                                                   */
/* ===================================================================== */
