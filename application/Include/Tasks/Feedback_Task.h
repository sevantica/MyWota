/**
 * @file Feedback_Task.h
 * @brief Application Task for Feedback (Sound) Logic
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 */

#ifndef FEEDBACK_TASK_H
#define FEEDBACK_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

/**
 * @brief Initialize and start the Feedback Polling Task
 */
void Feedback_Task_Start(void);

/** @brief Get feedback task handle. */
TaskHandle_t Feedback_Task_GetHandle(void);

#endif /* FEEDBACK_TASK_H */
