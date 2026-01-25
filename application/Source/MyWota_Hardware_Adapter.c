/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file BigYellow_Hardware_Adapter.c
 * @brief BigYellow Hardware Configuration Adapter
 * @details Defines all hardware pin mappings for BigYellow car wash controller
 */

/* Includes ------------------------------------------------------------------*/
#include "MyWota_Hardware_Adapter.h"
#include "USB_Logging.h"

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_HW_ADAPTER_EN    0

#if LOG_DEBUG_HW_ADAPTER_EN
    #define LOG_DEBUG_HW(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_HW(...)
#endif

/*===========================================================================*/
/*                    MyWota Pin Definitions (RP2040)                     */
/*===========================================================================*/

/* GPIO Pin Numbers */
#define GPIO_0              0       /* Physical pin 1 */
#define GPIO_1              1       /* Physical pin 2 */
#define GPIO_2              2       /* Physical pin 4 */
#define GPIO_3              3       /* Physical pin 5 */
#define GPIO_4              4       /* Physical pin 6 */
#define GPIO_5              5       /* Physical pin 7 */
#define GPIO_6              6       /* Physical pin 9 */
#define GPIO_7              7       /* Physical pin 10 */
#define GPIO_8              8       /* Physical pin 11 */
#define GPIO_9              9       /* Physical pin 12 */
#define GPIO_13             13      /* Physical pin 17 */
#define GPIO_16             16      /* Physical pin 21 */
#define GPIO_17             17      /* Physical pin 22 */
#define GPIO_18             18      /* Physical pin 24 */
#define GPIO_19             19      /* Physical pin 25 */
#define GPIO_20             20      /* Physical pin 26 */
#define GPIO_21             21      /* Physical pin 27 */
#define GPIO_22             22      /* Physical pin 29 */
#define GPIO_25             25      /* Built-in LED */
#define GPIO_26             26      /* Physical pin 31 */
#define GPIO_27             27      /* Physical pin 32 */
#define GPIO_28             28      /* Physical pin 34 - ADC2 */

/* Peripheral Baudrates */
#define SPI0_DEFAULT_BAUDRATE       62500000    /* 62.5 MHz (RP2040 max) */
#define I2C0_DEFAULT_BAUDRATE       400000      /* 400 kHz for NFC/RFID */
#define I2C1_DEFAULT_BAUDRATE       400000      /* 400 kHz */
#define UART0_DEFAULT_BAUDRATE      115200      /* RS485 */
#define UART1_DEFAULT_BAUDRATE      115200      /* Debug */

/*===========================================================================*/
/*                      MyWota Hardware Pin Map                           */
/*===========================================================================*/

static const HW_Pin_Map_t mywota_pin_map = {
    /* SPI0 - Shared by LCD and SD Card */
    .spi_bus0 = {
        .spi_instance = 0,
        .sck_pin = GPIO_18,
        .mosi_pin = GPIO_19,
        .miso_pin = GPIO_16,
        .default_baudrate = SPI0_DEFAULT_BAUDRATE
    },
    
    /* SPI1 - Not used */
    .spi_bus1 = {
        .spi_instance = 1,
        .sck_pin = 0xFF,
        .mosi_pin = 0xFF,
        .miso_pin = 0xFF,
        .default_baudrate = 0
    },
    
    /* I2C0 - NFC/RFID and IO Expander */
    .i2c_bus0 = {
        .i2c_instance = 0,
        .sda_pin = GPIO_20,
        .scl_pin = GPIO_21,
        .default_baudrate = I2C0_DEFAULT_BAUDRATE
    },
    
    /* I2C1 - Secondary */
    .i2c_bus1 = {
        .i2c_instance = 1,
        .sda_pin = GPIO_26,
        .scl_pin = GPIO_27,
        .default_baudrate = I2C1_DEFAULT_BAUDRATE
    },
    
    /* UART0 - RS485 Communication */
    .uart_bus0 = {
        .uart_instance = 0,
        .tx_pin = GPIO_0,
        .rx_pin = GPIO_1,
        .default_baudrate = UART0_DEFAULT_BAUDRATE
    },
    
    /* UART1 - Debug */
    .uart_bus1 = {
        .uart_instance = 1,
        .tx_pin = GPIO_4,
        .rx_pin = GPIO_5,
        .default_baudrate = UART1_DEFAULT_BAUDRATE
    },
    
    /* LCD Display (ILI9488) */
    .lcd = {
        .dc_pin = GPIO_2,
        .reset_pin = GPIO_3,
        .backlight_pin = GPIO_7,
        .cs_pin = GPIO_17,
        .spi_instance = 0,
        .backlight_inverted = true  /* 100% PWM = backlight OFF */
    },
    
    /* NFC/RFID (PN532) */
    .nfc = {
        .reset_pin = GPIO_22,
        .irq_pin = 0xFF,            /* Not used on MyWota */
        .i2c_instance = 0,
        .i2c_address = 0x24         /* PN532 I2C address */
    },
    
    /* SD Card (shared SPI0) */
    .sd_card = {
        .cs_pin = GPIO_13,
        .spi_instance = 0,
        .card_detect_pin = 0xFF     /* Not used */
    },
    
    /* RS485 Transceiver */
    .rs485 = {
        .uart_instance = 0,
        .de_pin = GPIO_9,           /* Direction Enable pin */
        .default_baudrate = UART0_DEFAULT_BAUDRATE
    },
    
    /* IO Expander (CAT9555) */
    .io_expander = {
        .i2c_instance = 0,
        .i2c_address = 0x27,
        .interrupt_pin = GPIO_8
    },
    
    /* Application-Specific GPIO */
    .app_gpio = {
        .status_led_pin = GPIO_6,
        .pico_led_pin = GPIO_25,
        .light_sensor_pin = GPIO_28,
        .flow_sensor_pin = GPIO_22,
        .valve_control_pin = 0xFF   /* Not used on MyWota (uses IO Expander) */
    },
    
    /* Project Identification */
    .project_name = "MyWota Controller",
    .board_revision = "Rev 1.0"
};

/*===========================================================================*/
/*                          Interface Callbacks                               */
/*===========================================================================*/

/**
 * @brief Get MyWota pin map
 */
static const HW_Pin_Map_t* mywota_get_pin_map(void)
{
    return &mywota_pin_map;
}

/**
 * @brief MyWota-specific hardware initialization
 * @note This is called during system startup to perform any 
 *       project-specific hardware setup beyond the standard peripheral init
 */
static HW_Interface_Result_t mywota_hw_init(void)
{
    LOG_DEBUG_HW("[HW_ADAPTER] MyWota hardware initialization...\\r\\n");
    
    /* Any MyWota-specific early initialization goes here */
    /* Most initialization is handled by Hardware_Access.c */
    
    LOG_DEBUG_HW("[HW_ADAPTER] ✓ MyWota hardware initialized\\r\\n");
    return HW_INTERFACE_OK;
}

/*===========================================================================*/
/*                          Interface Definition                              */
/*===========================================================================*/

static const HW_Interface_t mywota_hw_interface = {
    .init = mywota_hw_init,
    .get_pins = mywota_get_pin_map,
    .project_name = "MyWota"
};

/*===========================================================================*/
/*                          Public Functions                                  */
/*===========================================================================*/

/**
 * @brief Initialize MyWota hardware adapter
 */
HW_Interface_Result_t MyWota_Hardware_Adapter_Init(void)
{
    LOG_DEBUG_HW("[HW_ADAPTER] Registering MyWota hardware configuration...\\r\\n");
    
    HW_Interface_Result_t result = HW_RegisterInterface(&mywota_hw_interface);
    
    if (result == HW_INTERFACE_OK) {
        LOG_DEBUG_HW("[HW_ADAPTER] ✓ MyWota hardware adapter registered\\r\\n");
        LOG_DEBUG_HW("[HW_ADAPTER]   Project: %s\\r\\n", mywota_pin_map.project_name);
        LOG_DEBUG_HW("[HW_ADAPTER]   Board: %s\\r\\n", mywota_pin_map.board_revision);
    } else {
        USB_Log_Printf("[HW_ADAPTER] ✗ Failed to register hardware adapter\\r\\n");
    }
    
    return result;
}
