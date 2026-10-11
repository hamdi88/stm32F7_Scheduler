/*
 * scheduler.h
 *
 *  Created on: Jan 12, 2025
 *      Author: hosni
 *
 *  Cooperative time-triggered scheduler (SysTick based).
 *  Supervision support: per task run counter, execution time budget,
 *  overrun notification (task too long / ticks lost).
 */

#ifndef SCHEDULER_SCHEDULER_H_
#define SCHEDULER_SCHEDULER_H_

#include "stm32f7xx.h"
#include "main.h"

//defines//
#define Du16Schd_TasksMaxNb     32
#define Du16Schd_TickMs     1   //1ms
#define Dpu32Schd_NullPtr ((void (*)(void)) 0)

//more ticks than this waiting in the dispatcher = scheduler is late
#define Du32Schd_MaxPendingTicks  2UL

//Typedefs
typedef struct
{
  void (*pTask)(void);       //task function, NULL = free slot
  uint32_t u32Delay;         //ticks before the next run
  uint32_t u32Period;        //ticks between two runs
  uint32_t u32RunCount;      //number of runs since the task was added
  uint32_t u32BudgetCycles;  //max execution time in CPU cycles, 0 = not checked
  uint32_t u32MaxCycles;     //longest execution time measured (CPU cycles)
}TstSchd_Task;

typedef void (*TpSchd_task_fun)(void) ;

//overrun notification: Fu8Type = SCHD_OVERRUN_xxx,
//Fu16TaskIdx = task slot (task overrun), Fu32Value = cycles or pending ticks
typedef void (*TpSchd_OverrunCb)(uint8_t Fu8Type, uint16_t Fu16TaskIdx, uint32_t Fu32Value);

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

enum
{
  SCHD_OVERRUN_TASK = 0,  //a task exceeded its budget
  SCHD_OVERRUN_TICK = 1   //ticks accumulated, scheduler late
};
/******************************************************************************/
//function desc: init task table, SysTick (disabled) and the DWT cycle counter
//inputs: Fu16TicksMs -> tick period in ms
//outputs: SCHD_OK / SCHD_ERR
/******************************************************************************/
int16_t s16Schd_Init(uint16_t Fu16TicksMs);


/******************************************************************************/
//function desc: enable/disable scheduler timer
//inputs: Fu8Enable, 1->enable, 0->disable , else->No action
//outputs:
/******************************************************************************/
void vSchd_Enable(uint8_t Fu8Enable);

/******************************************************************************/
//function desc: run the tasks due for the next pending tick, sleep if none
//inputs:
//outputs:
/******************************************************************************/
void vSchd_Dispatcher(void);


/******************************************************************************/
//function desc: add a periodic task, default budget = one tick
//inputs: Fpu32_Func -> task, Fu32Delay -> ticks before first run,
//        Fu32Period -> ticks between runs (> 0)
//outputs: SCHD_OK / SCHD_ERR
/******************************************************************************/
int16_t s16Schd_AddTask(TpSchd_task_fun Fpu32_Func, uint32_t Fu32Delay, uint32_t Fu32Period);

/******************************************************************************/
//function desc: change the execution time budget of a task
//inputs: Fpu32_Func -> task, Fu32BudgetUs -> budget in us, 0 = not checked
//outputs: SCHD_OK / SCHD_ERR (task not found)
/******************************************************************************/
int16_t s16Schd_SetTaskBudget(TpSchd_task_fun Fpu32_Func, uint32_t Fu32BudgetUs);

/******************************************************************************/
//function desc: register the overrun notification (NULL to remove)
//inputs: Fp_Callback -> function called from the dispatcher
//outputs:
/******************************************************************************/
void vSchd_RegisterOverrunCallback(TpSchd_OverrunCb Fp_Callback);

/******************************************************************************/
//function desc: read access to a task slot (supervision, diagnostics)
//inputs: Fu16Index -> slot 0..Du16Schd_TasksMaxNb-1
//outputs: task, NULL if the slot is free or out of range
/******************************************************************************/
const TstSchd_Task *pstSchd_GetTask(uint16_t Fu16Index);
#endif /* SCHEDULER_SCHEDULER_H_ */
