/*
 * task_iwdt.h
 *
 *  Created on: Jan 19, 2025
 *      Author: hosni
 */

#ifndef TASKS_TASK_IWDT_H_
#define TASKS_TASK_IWDT_H_

#include "stm32f7xx.h"

//DEFINES
#define Du32iWDT_ClkPrescaler         3U          //LSI is 32KHz, 3--> /32 to have 1ms
#define Du32iWDT_StartKey       0x0000CCCC
#define Du32iWDT_ReloadKey      0x0000AAAA
#define Du32iWDT_UnlockKey      0x00005555

#endif /* TASKS_TASK_IWDT_H_ */


/******************************************************************************/
//function desc: setup Iwdt
//inputs: Fu32ReloadMs-> iwdt timeout in ms
//outputs:
/******************************************************************************/
void vTaskIwdt_init(uint32_t Fu32ReloadMs);


/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
void vTaskIWDT_Reload(void);
