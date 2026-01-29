/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * RTC Task Header
 *
 ******************************************************************************
 */

#ifndef RTC_TASK_H
#define RTC_TASK_H

#include "FreeRTOS.h"
#include "task.h"

/**
 * @brief Start RTC task
 */
void Task_Start_RTC_Task(void);

/**
 * @brief Get RTC task handle
 * @return Task handle or NULL if not started
 */
TaskHandle_t RTC_Task_GetHandle(void);

#endif /* RTC_TASK_H */
