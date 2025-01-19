/*
 * scheduler.h
 *
 *  Created on: Jan 12, 2025
 *      Author: hosni
 */

#ifndef SCHEDULER_SCHEDULER_H_
#define SCHEDULER_SCHEDULER_H_

#include "stm32f7xx.h"
#include "main.h"

//defines//
#define Du16Schd_TasksMaxNb     32
#define Du16Schd_TickMs     1   //1ms
#define Dpu32Schd_NullPtr ((void (*)(void)) 0)

//Typedefs
typedef struct
{
  void (*pTask)(void);
  uint32_t u32Delay;
  uint32_t u32Period;
}TstSchd_Task;

typedef void (*TpSchd_task_fun)(void) ;
enum
{
  SCHD_OK = 0,
  SCHD_ERR = -1,
};

enum
{
  SCHD_ENABLE = 1,
  SCHD_DISABLE = 0
};
/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
int16_t s16Schd_Init(uint16_t Fu16TicksMs);


/******************************************************************************/
//function desc: enable/disable scheduler timer
//inputs: Fu8Enable, 1->enable, 0->disable , else->No action
//outputs:
/******************************************************************************/
void vSchd_Enable(uint8_t Fu8Enable);

/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
void vSchd_Dispatcher(void);


/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
int16_t s16Schd_AddTask(TpSchd_task_fun Fpu32_Func, uint32_t Fu32Delay, uint32_t Fu32Period);
#endif /* SCHEDULER_SCHEDULER_H_ */
