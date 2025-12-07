# ILI9488 LCD Driver for LVGL

This driver provides support for the ILI9488 TFT LCD controller in LVGL applications.

## Features

- **Resolution**: 320x480 pixels
- **Color Support**: 16.7M colors (24-bit RGB)
- **Interface**: SPI and 8080-I 8-/9-/16-/18-bit parallel interface
- **Built-in Frame Buffer**: 320×480×18 bits
- **Pixel Format**: RGB565 (16-bit), RGB666 (18-bit) support
- **MIPI DCS Compliant**: Uses generic MIPI LCD driver underneath

## Hardware Requirements

The ILI9488 controller requires the following connections:

### SPI Interface (Recommended)
- **MOSI**: SPI data line
- **SCK**: SPI clock line  
- **CS**: Chip select (active low)
- **DC**: Data/Command select (low=command, high=data)
- **RST**: Reset (active low)
- **Power**: VCC (3.3V), GND

### Optional Pins
- **BL**: Backlight control (PWM capable)
- **MISO**: For reading display memory (rarely used)

## Configuration

### 1. Enable the Driver

In your `lv_conf.h` file, set:

```c
#define LV_USE_ILI9488       1
#define LV_USE_GENERIC_MIPI  1  // Required for all MIPI drivers
```

### 2. Include the Header

```c
#include "lvgl.h"
#include "src/drivers/display/ili9488/lv_ili9488.h"
```

## Usage

### Basic Initialization

```c
#include "lv_ili9488.h"

// Define your platform-specific callback functions
static void my_send_cmd(lv_display_t * disp, const uint8_t * cmd, size_t cmd_size, 
                       const uint8_t * param, size_t param_size);
static void my_send_color(lv_display_t * disp, const uint8_t * cmd, size_t cmd_size, 
                         uint8_t * param, size_t param_size);

void setup_display(void)
{
    // Create display with ILI9488 driver
    lv_display_t * disp = lv_ili9488_create(320, 480, 
                                           LV_LCD_FLAG_NONE,
                                           my_send_cmd, 
                                           my_send_color);
}
```

### Advanced Configuration

```c
void setup_display_advanced(void)
{
    // Create display with custom flags
    lv_display_t * disp = lv_ili9488_create(320, 480, 
                                           LV_LCD_FLAG_MIRROR_X | LV_LCD_FLAG_BGR,
                                           my_send_cmd, 
                                           my_send_color);
    
    // Set display gap if needed (for displays with offset)
    lv_ili9488_set_gap(disp, 0, 0);
    
    // Enable color inversion if needed
    lv_ili9488_set_invert(disp, false);
    
    // Set gamma curve
    lv_ili9488_set_gamma_curve(disp, LV_LCD_GAMMA_2_2);
}
```

## Platform Integration

The driver requires two platform-specific callback functions:

### Command Callback
```c
static void my_send_cmd(lv_display_t * disp, const uint8_t * cmd, size_t cmd_size, 
                       const uint8_t * param, size_t param_size)
{
    // 1. Set CS low
    // 2. Set DC low (command mode)
    // 3. Send command bytes via SPI
    // 4. Set DC high (data mode) 
    // 5. Send parameter bytes via SPI
    // 6. Set CS high
}
```

### Color Data Callback
```c
static void my_send_color(lv_display_t * disp, const uint8_t * cmd, size_t cmd_size, 
                         uint8_t * param, size_t param_size)
{
    // 1. Set CS low
    // 2. Set DC low (command mode)
    // 3. Send command bytes via SPI
    // 4. Set DC high (data mode)
    // 5. Send pixel data via SPI (preferably using DMA)
    // 6. Set CS high
    
    // Note: This function should use DMA for better performance
    // as it transfers large amounts of pixel data
}
```

## Available Flags

The driver supports the following configuration flags:

- `LV_LCD_FLAG_NONE`: Default configuration
- `LV_LCD_FLAG_MIRROR_X`: Horizontal mirror
- `LV_LCD_FLAG_MIRROR_Y`: Vertical mirror  
- `LV_LCD_FLAG_BGR`: BGR color order (instead of RGB)
- `LV_LCD_FLAG_RGB666`: 18-bit RGB666 mode (when supported by interface)

## Performance Tips

1. **Use DMA**: Implement DMA transfer in the `send_color` callback for better performance
2. **Optimize SPI Speed**: Use the highest SPI clock rate supported by your hardware
3. **Buffer Management**: Use LVGL's built-in double buffering for smoother animations
4. **Partial Updates**: LVGL will automatically use partial screen updates when possible

## Common Issues

### Display Not Working
- Check wiring connections (especially CS, DC, RST pins)
- Verify SPI configuration (mode, speed, bit order)
- Ensure power supply is stable

### Wrong Colors
- Try `LV_LCD_FLAG_BGR` flag if colors appear swapped
- Check SPI bit order configuration
- Verify pixel format settings

### Poor Performance
- Implement DMA in the `send_color` callback
- Increase SPI clock speed if possible
- Enable LVGL's performance optimizations in `lv_conf.h`

## Example Implementation

See `ili9488_example.c` for a complete STM32 HAL implementation example.

## API Reference

### Functions

- `lv_ili9488_create()`: Create ILI9488 display
- `lv_ili9488_set_gap()`: Set display offset
- `lv_ili9488_set_invert()`: Control color inversion
- `lv_ili9488_set_gamma_curve()`: Set gamma correction
- `lv_ili9488_send_cmd_list()`: Send custom command sequence

### Typical Display Specifications

- **Size**: 3.5 inch
- **Resolution**: 320×480 pixels
- **Colors**: 16.7M (262K with dithering)
- **Viewing Angle**: 160°
- **Interface**: 4-wire SPI
- **Supply Voltage**: 2.8V to 3.3V
- **Operating Temperature**: -20°C to +70°C

## License

This driver is part of the LVGL project and follows the same license terms.
