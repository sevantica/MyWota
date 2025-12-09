/*
  * @attention
  * Copyright (c) Sevantica 2025.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
*/

#ifndef APPLICATION_INCLUDE_LCD_DISPLAY_DRIVER_H_
#define APPLICATION_INCLUDE_LCD_DISPLAY_DRIVER_H_

/*Includes ----------------------------------------------------------*/

#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"

/*Typedefs -----------------------------------------------------------*/


/*Defines ------------------------------------------------------------*/


/*Macros -------------------------------------------------------------*/


/*Extern Variables ---------------------------------------------------*/

void Task_Start_LCD_Display_Driver_Task();
TaskHandle_t task_get_handle_LCD_Display_Driver_Task();

/* LCD Backlight Control Functions */
void lcd_backlight_on(uint8_t brightness);  /* brightness: 0-100 with gamma correction */
void lcd_backlight_off(void);
void lcd_backlight_set(uint8_t brightness_percent);  /* brightness_percent: 0-100 */

/* LCD Test Functions - Direct hardware access without LVGL */
void lcd_fill_screen_red(void);
void lcd_fill_screen_black(void);
void lcd_color_test(void);
void lcd_test_init(void);
void lcd_basic_init(void);
void lcd_read_registers(void);
void lcd_diagnostic_test(void);
void lcd_test_spi_communication(void);
void lcd_hardware_reset_and_init(void);
void lcd_test_orientations(void);
void lcd_set_orientation(uint8_t madctl_value);
void lcd_draw_red_box_100x100(uint16_t bottom_left_x, uint16_t bottom_left_y);
void lcd_rotate_display(uint8_t rotation);

/* Simple test function to call from main or other tasks */
void lcd_hardware_test(void);
void lcd_basic_init(void);

/* UI Update Functions - Generic */
bool ui_set_visibility(lv_obj_t * obj, bool visible);
bool ui_set_label_text(lv_obj_t * label, const char * text);
bool ui_set_bar_value(lv_obj_t * bar, int32_t value, lv_anim_enable_t anim);
bool ui_set_obj_style_bg_color(lv_obj_t * obj, lv_color_t color, lv_style_selector_t selector);

#endif /* APPLICATION_INCLUDE_LCD_DISPLAY_DRIVER_H_ */
