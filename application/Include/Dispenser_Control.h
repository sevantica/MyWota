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

#ifndef APPLICATION_INCLUDE_DISPENSER_CONTROL_H_
#define APPLICATION_INCLUDE_DISPENSER_CONTROL_H_

/*Includes ----------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"

/*Typedefs -----------------------------------------------------------*/


/*Defines ------------------------------------------------------------*/
#define		DISPENSER(i)			0 + i
/*Macros -------------------------------------------------------------*/


/*Extern Variables ---------------------------------------------------*/

void Task_Start_Dispenser_Control_Task();
TaskHandle_t task_get_handle_Dispenser_Control_Task();

#endif /* APPLICATION_INCLUDE_DISPENSER_CONTROL_H_ */
