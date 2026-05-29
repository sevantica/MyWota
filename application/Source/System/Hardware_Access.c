/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 * ******************************************************************************
 */

#include "Hardware_Access.h"
#include "MyWota_Hardware_Adapter.h"
#include "Pico_HAL.h"

/* 
 * This file is intentionally mostly empty to match the CCH project pattern. 
 * Hardware initialization logic has been moved to:
 * 1. Platforms/Pico_HAL.c (Universal driver logic in drivers library)
 * 2. MyWota_Hardware_Adapter.c (Project-specific pin mapping and init sequence)
 *
 * Direct access functions for application-specific GPIOs are implemented 
 * using the registered hardware interface.
 */

App_GPIO_Pins_t Get_App_GPIO_Pins(void)
{
    const HW_Pin_Map_t* pins = HW_GetPinMap();
    App_GPIO_Pins_t app_pins = {0};
    
    if (pins) {
        app_pins.system_comm_led_pin = pins->app_gpio.status_led_pin;
        app_pins.exp_intr_pin = pins->io_expander.interrupt_pin;
        app_pins.rs485_data_en_pin = pins->rs485.de_pin;
        app_pins.light_sensor_pin = pins->app_gpio.light_sensor_pin;
        app_pins.flow_sensor_pin = pins->app_gpio.flow_sensor_pin;
        app_pins.valve_control_pin = pins->app_gpio.valve_control_pin;
    }
    
    return app_pins;
}

void Hardware_LED_On(void)
{
    const HW_Pin_Map_t* pins = HW_GetPinMap();
    if (pins && pins->app_gpio.status_led_pin != 0xFF) {
        HAL_GPIO_Write(pins->app_gpio.status_led_pin, 1);
    }
}

void Hardware_LED_Off(void)
{
    const HW_Pin_Map_t* pins = HW_GetPinMap();
    if (pins && pins->app_gpio.status_led_pin != 0xFF) {
        HAL_GPIO_Write(pins->app_gpio.status_led_pin, 0);
    }
}
