/*
 * scheduler.c
 *
 *  Created on: Jan 12, 2025
 *      Author: hosni
 */


#include "scheduler.h"

//tasks array
TstSchd_Task tstSchd_Tasks[Du16Schd_TasksMaxNb];
uint32_t u32Schd_Ticks = 0;

/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
int16_t s16Schd_Init(uint16_t Fu16TicksMs)
{
  int16_t s16Ret;
  uint16_t u16Count;
  for (u16Count = 0; u16Count < Du16Schd_TasksMaxNb; u16Count++)
    {
      tstSchd_Tasks[u16Count].pTask = Dpu32Schd_NullPtr;
    }

  if (SysTick_Config(Fu16TicksMs*(SystemCoreClock/1000)) == 1)
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
      //TODO: check if disabling Irq is needed
      SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk ;
    }
  else if (SCHD_DISABLE == Fu8Enable)
    {
      SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk ;
    }
}

/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
int16_t s16Schd_AddTask(TpSchd_task_fun Fpu32_Func, uint32_t Fu32Delay, uint32_t Fu32Period)
{
  int16_t s16Ret;
  uint16_t u16Index;

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
          tstSchd_Tasks[u16Index].pTask = Fpu32_Func;
          tstSchd_Tasks[u16Index].u32Delay = Fu32Delay;
          tstSchd_Tasks[u16Index].u32Period= Fu32Period;
          s16Ret = SCHD_OK;
        }

    }

  return s16Ret;
}
/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
void vSchd_Dispatcher(void)
{
  uint32_t u32Ticks;
  uint16_t u16Count;

  __disable_irq();
  u32Ticks = u32Schd_Ticks;
  __enable_irq();

  if (u32Ticks > 0)
    {
      //tick is reached , time to check tasks to run
      for (u16Count=0; u16Count < Du16Schd_TasksMaxNb; u16Count++)
        {
          if (tstSchd_Tasks[u16Count].u32Delay > 0)
            {
              tstSchd_Tasks[u16Count].u32Delay--;
            }
          if ((0u == tstSchd_Tasks[u16Count].u32Delay) && (tstSchd_Tasks[u16Count].pTask != Dpu32Schd_NullPtr))
            {
              //run task function
              tstSchd_Tasks[u16Count].pTask();
              tstSchd_Tasks[u16Count].u32Delay = tstSchd_Tasks[u16Count].u32Period;
              __disable_irq();
              u32Schd_Ticks--; //decrement u32Schd_Ticks
              __enable_irq();
            }
        }

      __WFI();
    }

}


void SysTick_Handler(void)
{
 u32Schd_Ticks++;
 if (u32Schd_Ticks > 1)
   {
     //TODO: handle tasks taking over the more than one tick to finish

   }
}
