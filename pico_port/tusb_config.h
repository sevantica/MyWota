/**
 ******************************************************************************
 * @file    tusb_config.h
 * @brief   TinyUSB Configuration for CDC + MSC Composite Device
 * @details Configures TinyUSB to provide:
 *          - CDC (Communications Device Class) for serial terminal
 *          - MSC (Mass Storage Class) for SD card access
 * 
 * @attention
 * Copyright (c) Sevantica 2025
 * Based on TinyUSB examples (MIT License)
 * 
 ******************************************************************************
 */

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+

// RHPort number used for device
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

// RHPort max operational speed
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

// RHPort mode (must be defined before including tusb.h)
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#endif

//--------------------------------------------------------------------+
// Common Configuration
//--------------------------------------------------------------------+

// Defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU          OPT_MCU_RP2040
#endif

// Use FreeRTOS
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_FREERTOS
#endif

// Debug level (0=none, 1=error, 2=warn, 3=info)
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

// Enable Device stack
#define CFG_TUD_ENABLED       1

// Default is max speed that hardware controller could support with on-chip PHY
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

// Memory alignment for USB buffers
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------+
// DEVICE CONFIGURATION
//--------------------------------------------------------------------+

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//--------------------------------------------------------------------+
// CLASS CONFIGURATION
//--------------------------------------------------------------------+

// Enable CDC (Serial) and MSC (Mass Storage)
#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               1
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

// CDC FIFO size of TX and RX (larger buffers for better throughput)
#define CFG_TUD_CDC_RX_BUFSIZE    512
#define CFG_TUD_CDC_TX_BUFSIZE    512

// CDC Endpoint transfer buffer size
#define CFG_TUD_CDC_EP_BUFSIZE    64

// MSC Buffer size for Device Mass storage
#define CFG_TUD_MSC_EP_BUFSIZE    512

//--------------------------------------------------------------------+
// FreeRTOS Configuration
//--------------------------------------------------------------------+

// When using FreeRTOS, these should be defined
#if CFG_TUSB_OS == OPT_OS_FREERTOS
#include "FreeRTOS.h"
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
