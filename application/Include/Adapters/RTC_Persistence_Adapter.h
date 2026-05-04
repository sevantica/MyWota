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

#ifndef APPLICATION_INCLUDE_RTC_PERSISTENCE_ADAPTER_H_
#define APPLICATION_INCLUDE_RTC_PERSISTENCE_ADAPTER_H_

/**
 * @file RTC_Persistence_Adapter.h
 * @brief MyWota RTC Persistence Adapter - SD Card Storage
 * @details Implements RTC time persistence using SD card for MyWota project
 */

#include "RTC_Persistence_Interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize RTC persistence adapter
 * @details Registers SD card-based persistence with RTC core
 * @return RTC_PERSIST_OK on success
 */
RTC_Persist_Result_t RTC_Persistence_Adapter_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_INCLUDE_RTC_PERSISTENCE_ADAPTER_H_ */
