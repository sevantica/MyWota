/**
 * @file lv_ili9488.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_ili9488.h"

#if LV_USE_ILI9488

/*********************
 *      DEFINES
 *********************/

/* ILI9488 specific commands from provided driver */
#define CMD_SWRESET                      0x01
#define CMD_SLPOUT                       0x11
#define CMD_DISPON                       0x29
#define CMD_MADCTL                       0x36
#define CMD_PIXFMT                       0x3A
#define CMD_FRMCTR1                      0xB1
#define CMD_INVCTR                       0xB4
#define CMD_DFUNCTR                      0xB6
#define CMD_PWCTR1                       0xC0
#define CMD_PWCTR2                       0xC1
#define CMD_VMCTR1                       0xC5
#define CMD_GMCTRP1                      0xE0
#define CMD_GMCTRN1                      0xE1
#define CMD_ADJCTR3                      0xF7

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC CONSTANTS
 **********************/

/* Initialization sequence based on the working lcd_basic_init function */
static const uint8_t init_cmd_list[] = {
    /* Software Reset */
    CMD_SWRESET, 0,
    LV_LCD_CMD_DELAY_MS, 5,

    /* Positive Gamma Control - exact values from standalone driver */
    CMD_GMCTRP1, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,

    /* Negative Gamma Control - exact values from standalone driver */
    CMD_GMCTRN1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,

    /* Power Control 1 (Vreg1out, Verg2out) - exact values from standalone driver */
    CMD_PWCTR1, 2, 0x17, 0x15,
    LV_LCD_CMD_DELAY_MS, 5,

    /* Power Control 2 (VGH,VGL) - exact value from standalone driver */
    CMD_PWCTR2, 1, 0x41,
    LV_LCD_CMD_DELAY_MS, 5,

    /* Power Control 3 (Vcom) - exact values from standalone driver */
    CMD_VMCTR1, 3, 0x00, 0x12, 0x80,
    LV_LCD_CMD_DELAY_MS, 5,

    /* Interface Pixel Format (24 bit for SPI) - exact value from standalone driver */
    CMD_PIXFMT, 1, 0x66,

    /* Interface Mode Control (SDO NOT USE) - from standalone driver */
    0xB0, 1, 0x80,

    /* Frame Rate Control (60Hz) - exact value from standalone driver */
    CMD_FRMCTR1, 1, 0xA0,

    /* Display Inversion Control (2-dot) - exact value from standalone driver */
    CMD_INVCTR, 1, 0x02,

    /* Display Function Control RGB/MCU Interface Control - exact values from standalone driver */
    CMD_DFUNCTR, 2, 0x02, 0x02,

    /* Set Image Function (Disable 24 bit data) - exact value from standalone driver */
    0xE9, 1, 0x00,

    /* Adjust Control (D7 stream, loose) - exact values from standalone driver */
    CMD_ADJCTR3, 4, 0xA9, 0x51, 0x2C, 0x82,
    LV_LCD_CMD_DELAY_MS, 5,

    /* Exit Sleep - exact timing from standalone driver */
    CMD_SLPOUT, 0,
    LV_LCD_CMD_DELAY_MS, 120,

    /* Display On - exact timing from standalone driver */
    CMD_DISPON, 0,
    LV_LCD_CMD_DELAY_MS, 5,

    /* Memory Access Control: Set for proper color display (based on reference driver) */
    CMD_MADCTL, 1, 0x68, /* RGB color mode, right-then-down orientation (matches reference) */

    LV_LCD_CMD_EOF
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_display_t * lv_ili9488_create(uint32_t hor_res, uint32_t ver_res, lv_lcd_flag_t flags,
                                 lv_ili9488_send_cmd_cb_t send_cmd_cb, lv_ili9488_send_color_cb_t send_color_cb)
{
    lv_display_t * disp = lv_lcd_generic_mipi_create(hor_res, ver_res, flags, send_cmd_cb, send_color_cb);
    
    if (disp == NULL) {
        return NULL;
    }
    
    /* Send ILI9488 specific initialization commands */
    //lv_ili9488_send_cmd_list(disp, init_cmd_list);
    
    return disp;
}

void lv_ili9488_set_gap(lv_display_t * disp, uint16_t x, uint16_t y)
{
    lv_lcd_generic_mipi_set_gap(disp, x, y);
}

void lv_ili9488_set_invert(lv_display_t * disp, bool invert)
{
    lv_lcd_generic_mipi_set_invert(disp, invert);
}

void lv_ili9488_set_gamma_curve(lv_display_t * disp, uint8_t gamma)
{
    lv_lcd_generic_mipi_set_gamma_curve(disp, gamma);
}

void lv_ili9488_send_cmd_list(lv_display_t * disp, const uint8_t * cmd_list)
{
    lv_lcd_generic_mipi_send_cmd_list(disp, cmd_list);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_USE_ILI9488*/
