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
 * @file BigYellow_IO_Expander_Adapter.c
 * @brief BigYellow IO Expander Pin Mapping Adapter
 * @details Defines CAT9555 pin assignments for BigYellow hardware
 */

/* Includes ------------------------------------------------------------------*/
#include "MyWota_IO_Expander_Adapter.h"
#include "CAT9555_Driver.h"
#include "IO_Expander_Control.h"
#include "USB_Logging.h"

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_IO_ADAPTER_EN    0

#if LOG_DEBUG_IO_ADAPTER_EN
    #define LOG_DEBUG_IO_ADAPTER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_IO_ADAPTER(...)
#endif

/*===========================================================================*/
/*                       MyWota Pin Mapping                                   */
/*===========================================================================*/

/**
 * @brief MyWota CAT9555 pin mapping table
 * @note Matches hardware v2.0 schematic
 */
static const IO_Expander_Pin_Entry_t mywota_pin_map[] = {
    /* Pin 0: Main relay control */
    {
        .physical_pin = CAT9555_PIN_0,
        .func = IO_EXP_FUNC_RELAY,
        .index = 0,
        .is_output = true,
        .active_low = false,
        .name = "Main Relay"
    },
    
    /* Pins 1-4: Keypad rows */
    {
        .physical_pin = CAT9555_PIN_1,
        .func = IO_EXP_FUNC_KEYPAD_ROW,
        .index = 0,
        .is_output = true,
        .active_low = false,
        .name = "Keypad Row A"
    },
    {
        .physical_pin = CAT9555_PIN_2,
        .func = IO_EXP_FUNC_KEYPAD_ROW,
        .index = 1,
        .is_output = true,
        .active_low = false,
        .name = "Keypad Row B"
    },
    {
        .physical_pin = CAT9555_PIN_3,
        .func = IO_EXP_FUNC_KEYPAD_ROW,
        .index = 2,
        .is_output = true,
        .active_low = false,
        .name = "Keypad Row C"
    },
    {
        .physical_pin = CAT9555_PIN_4,
        .func = IO_EXP_FUNC_KEYPAD_ROW,
        .index = 3,
        .is_output = true,
        .active_low = false,
        .name = "Keypad Row D"
    },
    
    /* Pins 5-8: Keypad columns */
    {
        .physical_pin = CAT9555_PIN_5,
        .func = IO_EXP_FUNC_KEYPAD_COL,
        .index = 0,
        .is_output = false,
        .active_low = false,
        .name = "Keypad Col A"
    },
    {
        .physical_pin = CAT9555_PIN_6,
        .func = IO_EXP_FUNC_KEYPAD_COL,
        .index = 1,
        .is_output = false,
        .active_low = false,
        .name = "Keypad Col B"
    },
    {
        .physical_pin = CAT9555_PIN_7,
        .func = IO_EXP_FUNC_KEYPAD_COL,
        .index = 2,
        .is_output = false,
        .active_low = false,
        .name = "Keypad Col C"
    },
    {
        .physical_pin = CAT9555_PIN_8,
        .func = IO_EXP_FUNC_KEYPAD_COL,
        .index = 3,
        .is_output = false,
        .active_low = false,
        .name = "Keypad Col D"
    },
    
    /* Pins 9-10: Relay sense/feedback */
    {
        .physical_pin = CAT9555_PIN_9,
        .func = IO_EXP_FUNC_SENSE,
        .index = 0,
        .is_output = false,
        .active_low = false,
        .name = "Relay Sense 1"
    },
    {
        .physical_pin = CAT9555_PIN_10,
        .func = IO_EXP_FUNC_SENSE,
        .index = 1,
        .is_output = false,
        .active_low = false,
        .name = "Relay Sense 2"
    },
    
    /* Pin 11: Buzzer */
    {
        .physical_pin = CAT9555_PIN_11,
        .func = IO_EXP_FUNC_BUZZER,
        .index = 0,
        .is_output = true,
        .active_low = false,
        .name = "Buzzer"
    },
    
    /* Pin 12: User button */
    {
        .physical_pin = CAT9555_PIN_12,
        .func = IO_EXP_FUNC_BUTTON,
        .index = 0,
        .is_output = false,
        .active_low = true,  /* Button is active low (pressed = 0) */
        .name = "User Button"
    },
    
    /* Pin 13: Status LED */
    {
        .physical_pin = CAT9555_PIN_13,
        .func = IO_EXP_FUNC_LED,
        .index = 0,
        .is_output = true,
        .active_low = false,
        .name = "Status LED"
    },
    
    /* Pins 14-15: Spare inputs */
    {
        .physical_pin = CAT9555_PIN_14,
        .func = IO_EXP_FUNC_SPARE,
        .index = 0,
        .is_output = false,
        .active_low = false,
        .name = "Spare Input 1"
    },
    {
        .physical_pin = CAT9555_PIN_15,
        .func = IO_EXP_FUNC_SPARE,
        .index = 1,
        .is_output = false,
        .active_low = false,
        .name = "Spare Input 2"
    }
};

#define MYWOTA_PIN_MAP_COUNT (sizeof(mywota_pin_map) / sizeof(mywota_pin_map[0]))

/*===========================================================================*/
/*                          Interface Callbacks                               */
/*===========================================================================*/

/**
 * @brief Get MyWota pin map
 */
static const IO_Expander_Pin_Entry_t* mywota_get_pin_map(size_t* count)
{
    if (count != NULL) {
        *count = MYWOTA_PIN_MAP_COUNT;
    }
    return mywota_pin_map;
}

/**
 * @brief Initialize CAT9555 hardware
 */
static IO_Expander_Interface_Result_t mywota_init_io_expander(void)
{
    /* CAT9555 is already initialized in IO_Expander_Control_Init() */
    /* This callback is here for interface completeness */
    return IO_EXP_INTF_OK;
}

/**
 * @brief Write to CAT9555 pin
 */
static IO_Expander_Interface_Result_t mywota_write_pin(uint8_t physical_pin, bool state)
{
    IO_Expander_State_t io_state = state ? IO_EXP_STATE_HIGH : IO_EXP_STATE_LOW;
    IO_Expander_Control_Status_t status = IO_Expander_WritePin(physical_pin, io_state);
    
    return (status == IO_EXP_CTRL_OK) ? IO_EXP_INTF_OK : IO_EXP_INTF_ERROR;
}

/**
 * @brief Read from CAT9555 pin
 */
static IO_Expander_Interface_Result_t mywota_read_pin(uint8_t physical_pin, bool* state)
{
    if (state == NULL) {
        return IO_EXP_INTF_ERROR;
    }
    
    IO_Expander_State_t io_state;
    IO_Expander_Control_Status_t status = IO_Expander_GetPinState(physical_pin, &io_state);
    
    if (status == IO_EXP_CTRL_OK) {
        *state = (io_state == IO_EXP_STATE_HIGH);
        return IO_EXP_INTF_OK;
    }
    
    return IO_EXP_INTF_ERROR;
}

/*===========================================================================*/
/*                          Interface Definition                              */
/*===========================================================================*/

static const IO_Expander_Interface_t mywota_io_interface = {
    .get_pin_map = mywota_get_pin_map,
    .init = mywota_init_io_expander,
    .write_pin = mywota_write_pin,
    .read_pin = mywota_read_pin,
    .expander_name = "CAT9555",
    .project_name = "MyWota",
    .total_pins = 16
};

/*===========================================================================*/
/*                          Public Functions                                  */
/*===========================================================================*/

/**
 * @brief Initialize MyWota IO Expander adapter
 */
IO_Expander_Interface_Result_t MyWota_IO_Expander_Adapter_Init(void)
{
    LOG_DEBUG_IO_ADAPTER("[IO_ADAPTER] Registering MyWota IO Expander (%zu pins)...\r\n", 
                          MYWOTA_PIN_MAP_COUNT);
    
    IO_Expander_Interface_Result_t result = IO_Exp_RegisterInterface(&mywota_io_interface);
    
    if (result == IO_EXP_INTF_OK) {
        LOG_DEBUG_IO_ADAPTER("[IO_ADAPTER] MyWota IO Expander adapter registered\r\n");
    } else {
        USB_Log_Printf("[IO_ADAPTER] Failed to register IO Expander adapter\r\n");
    }
    
    return result;
}
