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

/* UI Getter Functions - UI polls these instead of receiving events */
uint32_t Dispenser_GetRequestedAmountML(void);     /* Amount requested to dispense */
uint32_t Dispenser_GetDispensedAmountML(void);     /* Amount dispensed in current session */
uint32_t Dispenser_GetDispensedSessionML(void);    /* Total dispensed in this card session */
bool Dispenser_IsValveOpen(void);                  /* Returns true if valve is currently open */
bool Dispenser_IsDispensing(void);                 /* Returns true if actively dispensing */

#endif /* APPLICATION_INCLUDE_DISPENSER_CONTROL_H_ */
