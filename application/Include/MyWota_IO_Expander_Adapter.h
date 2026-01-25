/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 */

#ifndef MYWOTA_IO_EXPANDER_ADAPTER_H_
#define MYWOTA_IO_EXPANDER_ADAPTER_H_

/**
 * @file MyWota_IO_Expander_Adapter.h
 * @brief MyWota IO Expander Adapter
 * @details Configures IO Expander Service for MyWota hardware
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MyWota IO Expander adapter (and Service)
 * @return true if initialized successfully
 */
bool MyWota_IO_Expander_Adapter_Init(void);

/**
 * @brief Control Main Relay (Pin 3)
 */
bool MyWota_IO_Expander_SetMainRelay(bool enable);

/**
 * @brief Control Status LED (Pin 13)
 */
bool MyWota_IO_Expander_SetStatusLED(bool enable);

/**
 * @brief Control Buzzer (Pin 11) - Direct control (normally used by Feedback Service)
 */
bool MyWota_IO_Expander_SetBuzzer(bool enable);

/**
 * @brief Check if User Button is pressed (Pin 12)
 * @return true if pressed (Active Low handled internally)
 */
bool MyWota_IO_Expander_IsUserButtonPressed(void);

#ifdef __cplusplus
}
#endif

#endif /* MYWOTA_IO_EXPANDER_ADAPTER_H_ */
