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

#ifndef APPLICATION_INCLUDE_PN532_TASK_H_
#define APPLICATION_INCLUDE_PN532_TASK_H_

/*Includes ----------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"

/*Typedefs -----------------------------------------------------------*/

/*Defines ------------------------------------------------------------*/

/*Macros -------------------------------------------------------------*/

/*Extern Variables ---------------------------------------------------*/

/*Function Prototypes ------------------------------------------------*/
void Task_Start_PN532_Driver_Task(void);
TaskHandle_t task_get_handle_PN532_Driver_Task(void);

#endif /* APPLICATION_INCLUDE_PN532_TASK_H_ */
