/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 */

/**
 * @file MyWota_IO_Expander_Adapter.c
 * @brief MyWota IO Expander Adapter
 * @details Configures IO Expander Service for MyWota hardware
 */

/* Includes ------------------------------------------------------------------*/
#include "MyWota_IO_Expander_Adapter.h"
#include "System Services/IO_Expander_Service_Interface.h"
#include "USB_Logging.h"

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_IO_ADAPTER_EN    1

#if LOG_DEBUG_IO_ADAPTER_EN
    #define LOG_DEBUG_IO_ADAPTER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_IO_ADAPTER(...)
#endif

/*===========================================================================*/
/*                       MyWota Pin Configuration                            */
/*===========================================================================*/

/**
 * @brief MyWota CAT9555 pin configuration
 * @note Matches hardware v2.0 schematic
 */
static const IO_Expander_Pin_Config_t mywota_pin_configs[] = {
    // Pin 0-2: Inputs (was keypad/unused)
    {.pin = 0, .is_input = true, .initial_state = false, .enable_pullup = true}, 
    {.pin = 1, .is_input = true, .initial_state = false, .enable_pullup = true},
    {.pin = 2, .is_input = true, .initial_state = false, .enable_pullup = true},

    // Pin 3: Main Relay (OUTPUT) - Default OFF
    {.pin = 3, .is_input = false, .initial_state = false, .enable_pullup = false},

    // Pins 4-8: Keypad (Unused) - Inputs with pullups safe
    {.pin = 4, .is_input = true, .initial_state = false, .enable_pullup = true},
    {.pin = 5, .is_input = true, .initial_state = false, .enable_pullup = true},
    {.pin = 6, .is_input = true, .initial_state = false, .enable_pullup = true},
    {.pin = 7, .is_input = true, .initial_state = false, .enable_pullup = true},
    {.pin = 8, .is_input = true, .initial_state = false, .enable_pullup = true},

    // Pin 9-10: Relay Sense (INPUT)
    {.pin = 9, .is_input = true, .initial_state = false, .enable_pullup = true},
    {.pin = 10, .is_input = true, .initial_state = false, .enable_pullup = true},

    // Pin 11: Buzzer (OUTPUT)
    {.pin = 11, .is_input = false, .initial_state = false, .enable_pullup = false},

    // Pin 12: User Button (INPUT, Active Low)
    {.pin = 12, .is_input = true, .initial_state = false, .enable_pullup = true},

    // Pin 13: Status LED (OUTPUT)
    {.pin = 13, .is_input = false, .initial_state = false, .enable_pullup = false},
    
    // Pin 14-15: Spare (INPUT)
    {.pin = 14, .is_input = true, .initial_state = false, .enable_pullup = true},
    {.pin = 15, .is_input = true, .initial_state = false, .enable_pullup = true}
};

#define MYWOTA_PIN_CONFIG_COUNT (sizeof(mywota_pin_configs) / sizeof(mywota_pin_configs[0]))

/*===========================================================================*/
/*                          Adapter Callbacks                                 */
/*===========================================================================*/

/**
 * @brief Handle input pin state changes
 */
static void mywota_on_input_changed(uint8_t pin, bool state)
{
    switch (pin) {
        case 12:  // User button (active low)
            if (!state) {  // Button pressed (active low)
                LOG_DEBUG_IO_ADAPTER("[IO_ADAPTER] User button PRESSED\r\n");
            } else {
                LOG_DEBUG_IO_ADAPTER("[IO_ADAPTER] User button RELEASED\r\n");
            }
            break;
            
        case 9:  // Relay sense 1
            LOG_DEBUG_IO_ADAPTER("[IO_ADAPTER] Relay Sense 1: %s\r\n", state ? "HIGH" : "LOW");
            break;
            
        case 10:  // Relay sense 2
            LOG_DEBUG_IO_ADAPTER("[IO_ADAPTER] Relay Sense 2: %s\r\n", state ? "HIGH" : "LOW");
            break;
            
        default:
            break;
    }
}

/*===========================================================================*/
/*                          Interface Definition                              */
/*===========================================================================*/

static const IO_Expander_Service_Interface_t mywota_io_service_interface = {
    .config = {
        .i2c_address = 0x27,  // CAT9555 address - MUST match Hardware_Adapter and System_Core
        .i2c_instance = 0,     // I2C0
        .poll_interval_ms = 50,  // Poll every 50ms
        .debounce_count = 3,     // 3 consistent reads for debouncing
        .pin_configs = mywota_pin_configs,
        .pin_config_count = MYWOTA_PIN_CONFIG_COUNT
    },
    .callbacks = {
        .on_input_changed = mywota_on_input_changed
    }
};

/*===========================================================================*/
/*                          Public Functions                                  */
/*===========================================================================*/

/**
 * @brief Initialize MyWota IO Expander adapter
 */
bool MyWota_IO_Expander_Adapter_Init(void)
{
    LOG_DEBUG_IO_ADAPTER("[IO_ADAPTER] Initializing MyWota IO Expander Service...\r\n");
    
    // Initialize the service with MyWota configuration
    if (!IO_Expander_Service_Init(&mywota_io_service_interface)) {
        USB_Log_Printf("[IO_ADAPTER] Failed to initialize IO Expander Service\r\n");
        return false;
    }
    
    // Start the service task
    IO_Expander_Service_Start();
    
    LOG_DEBUG_IO_ADAPTER("[✓] MyWota IO Expander initialized\r\n");
    
    return true;
}

bool MyWota_IO_Expander_SetMainRelay(bool enable)
{
    return IO_Expander_Service_SetOutput(3, enable);  // Pin 3 = Main Relay
}

bool MyWota_IO_Expander_SetStatusLED(bool enable)
{
    return IO_Expander_Service_SetOutput(13, enable);  // Pin 13 = Status LED
}

bool MyWota_IO_Expander_SetBuzzer(bool enable)
{
    return IO_Expander_Service_SetOutput(11, enable);  // Pin 11 = Buzzer
}

bool MyWota_IO_Expander_IsUserButtonPressed(void)
{
    bool state;
    if (IO_Expander_Service_GetInput(12, &state)) {
        return !state;  // Active low: Low = Pressed
    }
    return false;
}
