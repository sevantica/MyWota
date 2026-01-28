/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 * ******************************************************************************
 */

/**
 * @file MyWota_Hardware_Adapter.c
 * @brief MyWota Hardware Configuration Adapter
 * @details Defines all hardware pin mappings for MyWota dispenser controller
 */

/* Includes ------------------------------------------------------------------*/
#include "MyWota_Hardware_Adapter.h"
#include "System_Config.h"
#include "USB_Logging.h"
#include "RP2040_HAL.h"
#include "Hardware_Access.h"
#include "tusb.h"

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_HW_ADAPTER_EN    1

#if LOG_DEBUG_HW_ADAPTER_EN
    #define LOG_DEBUG_HW(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_HW(...)
#endif

/* Peripheral Baudrates */
#define SPI0_DEFAULT_BAUDRATE       62500000    /* 62.5 MHz (RP2040 max) */
#define I2C0_DEFAULT_BAUDRATE       400000      /* 400 kHz for NFC/RFID */
#define I2C1_DEFAULT_BAUDRATE       400000      /* 400 kHz */
#define UART0_DEFAULT_BAUDRATE      115200      /* RS485 Bus */
#define UART1_DEFAULT_BAUDRATE      115200      /* Extra / Debug */

/*===========================================================================*/
/*                      MyWota Hardware Pin Map                           */
/*===========================================================================*/

static const HW_Pin_Map_t mywota_pin_map = {
    /* SPI0 - Shared by LCD and SD Card */
    .spi_bus0 = {
        .spi_instance = 0,
        .sck_pin = 18,
        .mosi_pin = 19,
        .miso_pin = 16,
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
        .sda_pin = I2C_0_SDA_PIN,
        .scl_pin = I2C_0_SCL_PIN,
        .default_baudrate = I2C0_DEFAULT_BAUDRATE
    },
    
    /* I2C1 - Secondary */
    .i2c_bus1 = {
        .i2c_instance = 1,
        .sda_pin = 26,
        .scl_pin = 27,
        .default_baudrate = I2C1_DEFAULT_BAUDRATE
    },
    
    /* UART0 - RS485 */
    .uart_bus0 = {
        .uart_instance = 0,
        .tx_pin = 0,
        .rx_pin = 1,
        .default_baudrate = UART0_DEFAULT_BAUDRATE
    },
    
    /* UART1 - Extra */
    .uart_bus1 = {
        .uart_instance = 1,
        .tx_pin = 4,
        .rx_pin = 5,
        .default_baudrate = UART1_DEFAULT_BAUDRATE
    },
    
    /* LCD Display */
    .lcd = {
        .dc_pin = 2,
        .reset_pin = 3,
        .backlight_pin = 7,
        .cs_pin = 17,
        .spi_instance = 0,
        .backlight_inverted = true
    },
    
    /* NFC/RFID (PN532) */
    .nfc = {
        .reset_pin = PCD_RST_PIN,     /* GPIO 14 - Fixed from 22 to avoid flow sensor conflict */
        .irq_pin = 0xFF,
        .i2c_instance = 0,
        .i2c_address = 0x24
    },
    
    /* SD Card (shared SPI0) */
    .sd_card = {
        .cs_pin = 13,
        .spi_instance = 0,
        .card_detect_pin = 0xFF
    },
    
    /* RS485 Transceiver */
    .rs485 = {
        .uart_instance = 0,
        .de_pin = RS485_DATA_EN_PIN,  /* GPIO 9 */
        .default_baudrate = UART0_DEFAULT_BAUDRATE
    },
    
    /* IO Expander (CAT9555) */
    .io_expander = {
        .i2c_instance = 0,
        .i2c_address = 0x27,          /* MyWota uses 0x4E (8-bit) -> 0x27 (7-bit) */
        .interrupt_pin = EXP_INTR_PIN /* GPIO 8 */
    },
    
    /* Application GPIO */
    .app_gpio = {
        .status_led_pin = SYSTEM_COMM_LED_PIN, /* GPIO 6 */
        .pico_led_pin = 25,
        .light_sensor_pin = LIGHT_SENSOR_PIN,
        .flow_sensor_pin = FLOW_SENSOR_PIN,    /* GPIO 22 */
        .valve_control_pin = 0xFF   /* Controlled via IO Expander */
    },
    
    .project_name = "MyWota",
    .board_revision = "1.0.0"
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
    LOG_DEBUG_HW("[HW_ADAPTER] MyWota hardware initialization...\r\n");
    
    /* Initialize TinyUSB stack first */
    tusb_init();
    
    const HW_Pin_Map_t* pins = &mywota_pin_map;

    /* 1. Initialize Peripheral Buses */

    /* SPI 0 - Shared by LCD and SD */
    HAL_SPI_0_Init(pins->spi_bus0.default_baudrate, 
                   pins->spi_bus0.sck_pin, 
                   pins->spi_bus0.mosi_pin, 
                   pins->spi_bus0.miso_pin);

    /* I2C 0 - NFC and IO Expander */
    HAL_I2C_0_Init(pins->i2c_bus0.default_baudrate, 
                   pins->i2c_bus0.sda_pin, 
                   pins->i2c_bus0.scl_pin);

#if LOG_DEBUG_HW_ADAPTER_EN
    /* 1.1 Pre-Init NFC Reset to ensure it's awake for scanning */
    if (pins->nfc.reset_pin != 0xFF) {
        HAL_GPIO_Init_Output(pins->nfc.reset_pin);
        /* active low reset pulse */
        HAL_GPIO_Write(pins->nfc.reset_pin, 0);
        busy_wait_ms(10);
        HAL_GPIO_Write(pins->nfc.reset_pin, 1);
        /* Wait for chip to wake up (PN532 takes ~2ms to wake from standby, but safety margin) */
        busy_wait_ms(10);
    }

    /* Debug Scan I2C0 */
    LOG_DEBUG_HW("[HW_ADAPTER] Scanning I2C0 Bus...\r\n");
    int found_count = 0;
    uint8_t dummy_rx;
    
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        /* Read 1 byte to probe */
        if (HAL_I2C_0_Read(addr, &dummy_rx, 1, false) >= 0) {
            LOG_DEBUG_HW("[HW_ADAPTER]   - I2C Device Found at 0x%02X\r\n", addr);
            found_count++;
        }
    }
    
    if (found_count == 0) {
        LOG_DEBUG_HW("[HW_ADAPTER]   - No I2C devices found!\r\n");
    } else {
        LOG_DEBUG_HW("[HW_ADAPTER]   - Scan Complete. Found %d devices.\r\n", found_count);
    }

    /* Verify critical devices presence */
    if (HAL_I2C_0_Read(pins->nfc.i2c_address, &dummy_rx, 1, false) < 0) {
        LOG_DEBUG_HW("[HW_ADAPTER] CRITICAL: NFC (0x%02X) NOT detected!\r\n", pins->nfc.i2c_address);
    }
    
    if (HAL_I2C_0_Read(pins->io_expander.i2c_address, &dummy_rx, 1, false) < 0) {
        LOG_DEBUG_HW("[HW_ADAPTER] CRITICAL: IO Expander (0x%02X) NOT detected!\r\n", pins->io_expander.i2c_address);
    }
#endif

    /* UART 0 - RS485 */
    HAL_UART_Init(pins->uart_bus0.uart_instance, 
                  pins->uart_bus0.default_baudrate, 
                  pins->uart_bus0.tx_pin, 
                  pins->uart_bus0.rx_pin, 
                  pins->rs485.de_pin);

    /* 2. Initialize Application GPIOs */
    if (pins->app_gpio.status_led_pin != 0xFF) {
        HAL_GPIO_Init_Output(pins->app_gpio.status_led_pin);
    }
    
    if (pins->app_gpio.flow_sensor_pin != 0xFF) {
        HAL_GPIO_Init_Input(pins->app_gpio.flow_sensor_pin, true, false); 
    }

    /* 3. Module Specific Hardware Init */
    
    /* NFC Reset Pin */
    if (pins->nfc.reset_pin != 0xFF) {
        HAL_GPIO_Init_Output(pins->nfc.reset_pin);
        HAL_GPIO_Write(pins->nfc.reset_pin, 1); // Keep high (active)
    }

    /* LCD Pins */
    HAL_GPIO_Init_Output(pins->lcd.dc_pin);
    HAL_GPIO_Init_Output(pins->lcd.reset_pin);
    HAL_GPIO_Init_Output(pins->lcd.cs_pin);
    HAL_GPIO_Write(pins->lcd.cs_pin, 1); // Inactive
    
    if (pins->lcd.backlight_pin != 0xFF) {
        HAL_PWM_Init(pins->lcd.backlight_pin, 1000); // 1kHz PWM
        HAL_PWM_SetPercent(pins->lcd.backlight_pin, pins->lcd.backlight_inverted ? 0 : 100);
    }

    /* SD Card */
    HAL_GPIO_Init_Output(pins->sd_card.cs_pin);
    HAL_GPIO_Write(pins->sd_card.cs_pin, 1); // Inactive

    LOG_DEBUG_HW("[HW_ADAPTER] ✓ MyWota hardware initialized\r\n");
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
        LOG_DEBUG_HW("[HW_ADAPTER] ✓ MyWota hardware adapter registered\r\n");
    } else {
        USB_Log_Printf("[HW_ADAPTER] ✗ Failed to register hardware adapter\r\n");
    }
    
    return result;
}
