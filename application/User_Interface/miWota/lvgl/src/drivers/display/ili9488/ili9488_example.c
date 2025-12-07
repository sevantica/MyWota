/**
 * @file ili9488_example.c
 * @brief Generic example usage of ILI9488 LCD driver for LVGL
 * 
 * This example demonstrates how to create and configure an ILI9488 display
 * for use with LVGL graphics library.
 */

#include "lvgl.h"
#include "lv_ili9488.h"

#if LV_USE_ILI9488

/**
 * Example function to create and configure an ILI9488 display
 * This is a reference implementation that should be adapted to your specific hardware
 */
void ili9488_example_create_display(void)
{
    /* Create ILI9488 display object */
    /* Note: You need to provide your own SPI write callbacks */
    lv_display_t * disp = lv_ili9488_create(320, 480, LV_LCD_FLAG_NONE, NULL, NULL);
    
    if (disp) {
        /* Optional: Set gap configuration if needed for your display */
        lv_ili9488_set_gap(disp, 0, 0);
        
        /* Optional: Set invert configuration if needed */
        lv_ili9488_set_invert(disp, false);
        
        /* Optional: Set gamma curve if needed */
        lv_ili9488_set_gamma_curve(disp, 0);
        
        /* Display is now ready to use */
        LV_LOG_INFO("ILI9488 display created successfully");
    } else {
        LV_LOG_ERROR("Failed to create ILI9488 display");
    }
}

#else
void ili9488_example_create_display(void)
{
    LV_LOG_WARN("ILI9488 driver is not enabled (LV_USE_ILI9488 = 0)");
}
#endif /* LV_USE_ILI9488 */
