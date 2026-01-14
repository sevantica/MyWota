/**
 ******************************************************************************
 * @file    RS485_Task.h
 * @brief   RS485 Communication Task Header
 * 
 * @attention
 * Copyright (c) Sevantica 2025
 * 
 ******************************************************************************
 */

#ifndef APPLICATION_INCLUDE_RS485_TASK_H_
#define APPLICATION_INCLUDE_RS485_TASK_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include "RS485_Protocol.h"

/* Public Functions ----------------------------------------------------------*/

/**
 * @brief Send RS485 frame (thread-safe)
 * @param frame Frame to send
 */
void RS485_SendFrame(const RS485_Frame_t *frame);

/**
 * @brief Start RS485 communication task
 * @details Creates the RS485 task that handles multi-drop protocol communication
 */
void Task_Start_RS485_Task(void);

/**
 * @brief Get RS485 task handle
 * @return Task handle or NULL if not created
 */
TaskHandle_t RS485_Task_GetHandle(void);

/**
 * @brief Check if RS485 is ready
 * @return true if initialized and ready, false otherwise
 */
bool RS485_Task_IsReady(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_INCLUDE_RS485_TASK_H_ */
