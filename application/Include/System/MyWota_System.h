/**
 * @file MyWota_System.h
 * @brief MyWota project-specific system definitions
 */

#ifndef MYWOTA_SYSTEM_H
#define MYWOTA_SYSTEM_H

#include "System_Core.h"
#include "Hardware_Access.h"

/* Watchdog Task IDs - Application-specific tasks (System Core uses 0-9) */
#define SYSTEM_TASK_ID_LCD_DISPLAY          (SYS_TASK_ID_APP_START + 0)
#define SYSTEM_TASK_ID_DISPENSER            (SYS_TASK_ID_APP_START + 1)
#define SYSTEM_TASK_ID_BUZZER_POLLING       (SYS_TASK_ID_APP_START + 2)
#define SYSTEM_TASK_ID_MIFARE_POLLING       (SYS_TASK_ID_APP_START + 3)
#define SYSTEM_TASK_ID_IO_EXPANDER          (SYS_TASK_ID_APP_START + 4)
#define SYSTEM_TASK_ID_RS485                SYS_TASK_ID_RS485

/* Backward compatibility for shared drivers that use old names */
#define SYSTEM_TASK_ID_SD_LOGGER            SYS_TASK_ID_SD_LOGGER
#define SYSTEM_TASK_ID_USB_CDC              SYS_TASK_ID_USB_CDC
#define SYSTEM_TASK_ID_USB_COMMAND_HANDLER  SYS_TASK_ID_USB_COMMAND
#define SYSTEM_TASK_ID_RTC                  SYS_TASK_ID_RTC

/* GPIO Pin Positions */
typedef enum {
    EXP1_I2C_A0_POS = 0,
    EXP1_I2C_A1_POS,
    EXP1_I2C_A2_POS,
    MUX_ADD_SO_POS, // Corrected typo SO -> S0 if found, but keeping for compatibility
    MUX_ADD_S1_POS,
    MUX_ADD_S2_POS,
    MUX_COM_POS,
    RELAY_CONTROL_0_POS,
    RELAY_CONTROL_1_POS,
    RELAY_CONTROL_2_POS,
    RELAY_CONTROL_3_POS,
    RELAY_CONTROL_4_POS,
    RELAY_CONTROL_5_POS,
    RELAY_CONTROL_6_POS,
    RELAY_CONTROL_7_POS,
    LED_TEST_ONLY_SINK_POS,
    UI_BUTTON_DISPENSER_0_POS,
    UI_BUTTON_DISPENSER_4_POS,
    SPI_LCD_CHIP_SELECT_POS,
    SPI_LCD_DATA_COMMAND_POS,
    SPI_LCD_RESET_POS,
} GPIO_PIN_POSTIONS;

/* Assigned IO Definitions */
#define EXP1_I2C_A0             USER_GPIO_ID_x(A,4)
#define EXP1_I2C_A1             USER_GPIO_ID_x(A,15)
#define EXP1_I2C_A2             USER_GPIO_ID_x(B,2)
#define MUX_ADD_S0              USER_GPIO_ID_x(C,13)
#define MUX_ADD_S1              USER_GPIO_ID_x(C,14)
#define MUX_ADD_S2              USER_GPIO_ID_x(C,15)
#define MUX_COM                 USER_GPIO_ID_x(A,1)

/* Relay Control mapping to IO Expander 1 */
#define RELAY_CONTROL_0         GPIO_EXP1_EXT_0
#define RELAY_CONTROL_1         GPIO_EXP1_EXT_1
#define RELAY_CONTROL_2         GPIO_EXP1_EXT_2
#define RELAY_CONTROL_3         GPIO_EXP1_EXT_3
#define RELAY_CONTROL_4         GPIO_EXP1_EXT_4
#define RELAY_CONTROL_5         GPIO_EXP1_EXT_5
#define RELAY_CONTROL_6         GPIO_EXP1_EXT_6
#define RELAY_CONTROL_7         GPIO_EXP1_EXT_7

/* UI Buttons mapping to IO Expander 2 */
#define UI_BUTTON_DISPENSER_0   GPIO_EXP2_EXT_0
#define UI_BUTTON_DISPENSER_1   GPIO_EXP2_EXT_1
#define UI_BUTTON_DISPENSER_2   GPIO_EXP2_EXT_2
#define UI_BUTTON_DISPENSER_3   GPIO_EXP2_EXT_3
#define UI_BUTTON_DISPENSER_4   GPIO_EXP2_EXT_4
#define UI_BUTTON_DISPENSER_5   GPIO_EXP2_EXT_5

/* LCD Hardware Control */
#define SPI_LCD_CHIP_SELECT     GPIO_EXP2_EXT_1
#define SPI_LCD_DATA_COMMAND    GPIO_EXP2_EXT_2
#define SPI_LCD_RESET           GPIO_EXP2_EXT_3

#endif /* MYWOTA_SYSTEM_H */
