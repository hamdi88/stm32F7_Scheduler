/*
 * scheduler.c
 *
 *  Created on: Jan 12, 2025
 *      Author: hosni
 */


#include "scheduler.h"

//tasks array
TstSchd_Task tstSchd_Tasks[Du16Schd_TasksMaxNb];
//ticks counted by SysTick and not processed yet by the dispatcher
volatile uint32_t u32Schd_Ticks = 0;
//overrun notification, NULL = none
static TpSchd_OverrunCb pSchd_OverrunCb = 0;

/******************************************************************************/
//function desc: init task table, SysTick (disabled) and the DWT cycle counter
//inputs: Fu16TicksMs -> tick period in ms
//outputs: SCHD_OK / SCHD_ERR
/******************************************************************************/
int16_t s16Schd_Init(uint16_t Fu16TicksMs)
{
  int16_t s16Ret;     //result
  uint16_t u16Count;  //task slot index
  for (u16Count = 0; u16Count < Du16Schd_TasksMaxNb; u16Count++)
    {
      tstSchd_Tasks[u16Count].pTask = Dpu32Schd_NullPtr;
    }

  //DWT cycle counter measures the task execution time
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55UL;  //unlock DWT on Cortex-M7
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  //SysTick_Config returns 0 on success
  if (SysTick_Config(Fu16TicksMs*(SystemCoreClock/1000)) == 0)
    {
      //disable systick for now
      SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk ;
      s16Ret = SCHD_OK;
    }
  else
    {
      s16Ret = SCHD_ERR;
    }
  return s16Ret;
}

/******************************************************************************/
//function desc: enable/disable scheduler timer
//inputs: Fu8Enable, 1->enable, 0->disable , else->No action
//outputs:
/******************************************************************************/
void vSchd_Enable(uint8_t Fu8Enable)
{
  if (SCHD_ENABLE == Fu8Enable)
    {
      SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk ;
    }
  else if (SCHD_DISABLE == Fu8Enable)
    {
      SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk ;
    }
}

/******************************************************************************/
//function desc: add a periodic task, default budget = one tick
//inputs: Fpu32_Func -> task, Fu32Delay -> ticks before first run,
//        Fu32Period -> ticks between runs (> 0)
//outputs: SCHD_OK / SCHD_ERR
/******************************************************************************/
int16_t s16Schd_AddTask(TpSchd_task_fun Fpu32_Func, uint32_t Fu32Delay, uint32_t Fu32Period)
{
  int16_t s16Ret;     //result
  uint16_t u16Index;  //task slot index

  if (Fpu32_Func == Dpu32Schd_NullPtr)
    {
      s16Ret = SCHD_ERR;

    }
  else if(0u == Fu32Period)
  {
      //OneShot task not accepted
      s16Ret = SCHD_ERR;
  }
  else
    {
      u16Index = 0;
      while ((u16Index < Du16Schd_TasksMaxNb) && (tstSchd_Tasks[u16Index].pTask != Dpu32Schd_NullPtr))
        {
          u16Index++;
        }
      if (u16Index >= Du16Schd_TasksMaxNb)
        {
          s16Ret = SCHD_ERR;
        }
      else
        {
          tstSchd_Tasks[u16Index].u32Delay = Fu32Delay;
          tstSchd_Tasks[u16Index].u32Period= Fu32Period;
          tstSchd_Tasks[u16Index].u32RunCount = 0;
          tstSchd_Tasks[u16Index].u32BudgetCycles = Du16Schd_TickMs * (SystemCoreClock / 1000UL);
          tstSchd_Tasks[u16Index].u32MaxCycles = 0;
          //written last: the slot becomes active only when fully set
          tstSchd_Tasks[u16Index].pTask = Fpu32_Func;
          s16Ret = SCHD_OK;
        }

    }

  return s16Ret;
}

/******************************************************************************/
//function desc: change the execution time budget of a task
//inputs: Fpu32_Func -> task, Fu32BudgetUs -> budget in us, 0 = not checked
//outputs: SCHD_OK / SCHD_ERR (task not found)
/******************************************************************************/
int16_t s16Schd_SetTaskBudget(TpSchd_task_fun Fpu32_Func, uint32_t Fu32BudgetUs)
{
  int16_t s16Ret = SCHD_ERR;  //result, error until the task is found
  uint16_t u16Index;          //task slot index

  for (u16Index = 0; u16Index < Du16Schd_TasksMaxNb; u16Index++)
    {
      if ((Fpu32_Func != Dpu32Schd_NullPtr) && (tstSchd_Tasks[u16Index].pTask == Fpu32_Func))
        {
          tstSchd_Tasks[u16Index].u32BudgetCycles = Fu32BudgetUs * (SystemCoreClock / 1000000UL);
          s16Ret = SCHD_OK;
        }
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: register the overrun notification (NULL to remove)
//inputs: Fp_Callback -> function called from the dispatcher
//outputs:
/******************************************************************************/
void vSchd_RegisterOverrunCallback(TpSchd_OverrunCb Fp_Callback)
{
  pSchd_OverrunCb = Fp_Callback;
}

/******************************************************************************/
//function desc: read access to a task slot (supervision, diagnostics)
//inputs: Fu16Index -> slot 0..Du16Schd_TasksMaxNb-1
//outputs: task, NULL if the slot is free or out of range
/******************************************************************************/
const TstSchd_Task *pstSchd_GetTask(uint16_t Fu16Index)
{
  const TstSchd_Task *pstTask = 0;  //result, NULL by default

  if ((Fu16Index < Du16Schd_TasksMaxNb) && (tstSchd_Tasks[Fu16Index].pTask != Dpu32Schd_NullPtr))
    {
      pstTask = &tstSchd_Tasks[Fu16Index];
    }

  return pstTask;
}

/******************************************************************************/
//function desc: run the tasks due for the next pending tick, sleep if none
//               one call processes exactly one tick
//inputs:
//outputs:
/******************************************************************************/
void vSchd_Dispatcher(void)
{
  uint32_t u32Pending;  //ticks waiting when the dispatcher was called
  uint16_t u16Count;    //task slot index
  uint32_t u32Start;    //DWT counter before the task call
  uint32_t u32Cycles;   //task execution time in CPU cycles

  __disable_irq();
  u32Pending = u32Schd_Ticks;
  if (0u == u32Pending)
    {
      //no tick to process: sleep. WFI wakes up on the pending SysTick even
      //with interrupts masked, so no tick can be missed between test and sleep
      __WFI();
      __enable_irq();
      return;
    }
  u32Schd_Ticks--;
  __enable_irq();

  //tasks took more than a tick: real time is lost
  if ((u32Pending > Du32Schd_MaxPendingTicks) && (pSchd_OverrunCb != 0))
    {
      pSchd_OverrunCb(SCHD_OVERRUN_TICK, 0, u32Pending);
    }

  for (u16Count=0; u16Count < Du16Schd_TasksMaxNb; u16Count++)
    {
      if (tstSchd_Tasks[u16Count].pTask == Dpu32Schd_NullPtr)
        {
          continue;
        }
      if (tstSchd_Tasks[u16Count].u32Delay > 0)
        {
          tstSchd_Tasks[u16Count].u32Delay--;
        }
      if (0u == tstSchd_Tasks[u16Count].u32Delay)
        {
          //run task function and measure its execution time
          u32Start = DWT->CYCCNT;
          tstSchd_Tasks[u16Count].pTask();
          u32Cycles = DWT->CYCCNT - u32Start;

          tstSchd_Tasks[u16Count].u32RunCount++;
          tstSchd_Tasks[u16Count].u32Delay = tstSchd_Tasks[u16Count].u32Period;
          if (u32Cycles > tstSchd_Tasks[u16Count].u32MaxCycles)
            {
              tstSchd_Tasks[u16Count].u32MaxCycles = u32Cycles;
            }

          if ((tstSchd_Tasks[u16Count].u32BudgetCycles != 0u) &&
              (u32Cycles > tstSchd_Tasks[u16Count].u32BudgetCycles) &&
              (pSchd_OverrunCb != 0))
            {
              pSchd_OverrunCb(SCHD_OVERRUN_TASK, u16Count, u32Cycles);
            }
        }
    }
}


void SysTick_Handler(void)
{
 //count the tick, the dispatcher processes it and detects accumulation
 u32Schd_Ticks++;
}
