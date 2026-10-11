/*
 * safety_flow.c
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  Program flow supervision and watchdog:
 *   - alive supervision: over each window every scheduler task must have run
 *     (window / period) times, +/- tolerance. Detects dead, starved or
 *     runaway tasks without any code inside the tasks.
 *   - IWDG with window: refreshed by vSafety_Task only when every check
 *     passed. Too late -> reset (system stuck), too early -> reset (runaway
 *     loop refreshing the watchdog).
 */


/* Includes ------------------------------------------------------------------*/

#include "safety_internal.h"
#include "../scheduler/scheduler.h"

/* Private Macros ------------------------------------------------------------*/

//IWDG prescaler division factor: 4, 8, 16 ...
#define Du32SafetyFlow_IwdgDiv          (4UL << Du32SafetyCfg_IwdgPrescaler)

//shortest time before the window opens (slowest LSI) + 5% margin, in us
//a refresh requested before this delay is skipped to avoid an early-refresh reset
#define Du32SafetyFlow_MinRefreshUs     ((((Du32SafetyCfg_IwdgReload - Du32SafetyCfg_IwdgWindow) * \
                                           Du32SafetyFlow_IwdgDiv * 1000000UL) / Du32SafetyCfg_LsiMinHz) * 105UL / 100UL)

/* Private Types -------------------------------------------------------------*/


/* Private Variables ---------------------------------------------------------*/

//1 once the IWDG is running (it cannot be stopped)
static uint8_t u8SafetyFlow_WdgStarted = 0;

//DWT counter when the watchdog was started
static uint32_t u32SafetyFlow_StartTime = 0;

//1 until the first refresh after start has been done
static uint8_t u8SafetyFlow_FirstRefresh = 0;

//1 once the alive supervision is running
static uint8_t u8SafetyFlow_Supervising = 0;

//1 until the first supervision call, which opens the first window
static uint8_t u8SafetyFlow_FirstWindow = 0;

//time elapsed in the current supervision window (ms)
static uint32_t u32SafetyFlow_ElapsedMs = 0;

//task run counters at the start of the current window
static uint32_t au32SafetyFlow_LastCount[Du16Schd_TasksMaxNb];

/* Private Function Prototypes -----------------------------------------------*/


/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: start the IWDG with window (cannot be stopped afterwards)
//inputs:
//outputs:
/******************************************************************************/
void vSafetyFlow_StartWatchdog(void)
{
#if (Du8SafetyCfg_WdgFreezeInDebug == 1U)
  DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
#endif

  IWDG->KR = Du32Safety_IwdgKeyStart;   //start, enables LSI
  IWDG->KR = Du32Safety_IwdgKeyUnlock;  //unlock PR, RLR, WINR
  IWDG->PR = Du32SafetyCfg_IwdgPrescaler;
  IWDG->RLR = Du32SafetyCfg_IwdgReload;
  while (0U != IWDG->SR)
    {
      //wait until PR and RLR are taken into account
    }
  IWDG->WINR = Du32SafetyCfg_IwdgWindow;  //writing WINR also reloads the counter

  u32SafetyFlow_StartTime = DWT->CYCCNT;
  u8SafetyFlow_FirstRefresh = 1;
  u8SafetyFlow_WdgStarted = 1;
}

/******************************************************************************/
//function desc: refresh the watchdog. Only the first refresh after start is
//               delayed until the window is open (the first vSafety_Task call
//               may come right after the start). Any later refresh outside
//               the window resets the MCU, as intended.
//inputs:
//outputs:
/******************************************************************************/
void vSafetyFlow_RefreshWatchdog(void)
{
  uint32_t u32Elapsed;  //time since the watchdog start (cycles)
  uint32_t u32Min;      //time before the window is surely open (cycles)

  if (u8SafetyFlow_WdgStarted)
    {
      if (u8SafetyFlow_FirstRefresh)
        {
          u32Elapsed = DWT->CYCCNT - u32SafetyFlow_StartTime;
          u32Min = Du32SafetyFlow_MinRefreshUs * (SystemCoreClock / 1000000UL);
          if (u32Elapsed < u32Min)
            {
              return;  //too early, the next vSafety_Task call will refresh
            }
          u8SafetyFlow_FirstRefresh = 0;
        }
      IWDG->KR = Du32Safety_IwdgKeyReload;
    }
}

/******************************************************************************/
//function desc: watchdog state
//inputs:
//outputs: 1 if the IWDG is running
/******************************************************************************/
uint8_t u8SafetyFlow_IsWatchdogStarted(void)
{
  return u8SafetyFlow_WdgStarted;
}

/******************************************************************************/
//function desc: arm the supervision. The first window opens at the first
//               u8SafetyFlow_Supervise call, so that every window is measured
//               between two calls of the same task (exactly window ms apart)
//inputs:
//outputs:
/******************************************************************************/
void vSafetyFlow_StartSupervision(void)
{
  u32SafetyFlow_ElapsedMs = 0;
  u8SafetyFlow_FirstWindow = 1;
  u8SafetyFlow_Supervising = 1;
}

/******************************************************************************/
//function desc: alive supervision, call every Du32SafetyCfg_TaskPeriodMs
//inputs: Fpu32TaskIdx -> index of the failing task
//outputs: Du8Safety_TestPass / Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetyFlow_Supervise(uint32_t *Fpu32TaskIdx)
{
  uint8_t u8Ret = Du8Safety_TestPass;  //result
  uint16_t u16Index;                   //task slot
  const TstSchd_Task *pstTask;         //task in the slot
  uint32_t u32Runs;                    //runs during the window
  uint32_t u32Min;                     //minimum expected runs
  uint32_t u32Max;                     //maximum expected runs

  if (!u8SafetyFlow_Supervising)
    {
      return Du8Safety_TestPass;
    }

  //first call: snapshot the run counters, this opens the first window
  if (u8SafetyFlow_FirstWindow)
    {
      for (u16Index = 0; u16Index < Du16Schd_TasksMaxNb; u16Index++)
        {
          pstTask = pstSchd_GetTask(u16Index);
          au32SafetyFlow_LastCount[u16Index] = (pstTask != NULL) ? pstTask->u32RunCount : 0U;
        }
      u32SafetyFlow_ElapsedMs = 0;
      u8SafetyFlow_FirstWindow = 0;
      return Du8Safety_TestPass;
    }

  u32SafetyFlow_ElapsedMs += Du32SafetyCfg_TaskPeriodMs;
  if (u32SafetyFlow_ElapsedMs < Du32SafetyCfg_FlowWindowMs)
    {
      return Du8Safety_TestPass;
    }
  u32SafetyFlow_ElapsedMs = 0;

  //end of window: compare every task with its expected run count
  for (u16Index = 0; u16Index < Du16Schd_TasksMaxNb; u16Index++)
    {
      pstTask = pstSchd_GetTask(u16Index);
      if ((NULL == pstTask) || (0U == pstTask->u32Period))
        {
          continue;
        }

      u32Runs = pstTask->u32RunCount - au32SafetyFlow_LastCount[u16Index];
      au32SafetyFlow_LastCount[u16Index] = pstTask->u32RunCount;

      u32Min = Du32SafetyCfg_FlowWindowMs / (pstTask->u32Period * Du16Schd_TickMs);
      u32Min = (u32Min > Du32SafetyCfg_FlowTolerance) ? (u32Min - Du32SafetyCfg_FlowTolerance) : 0U;
      u32Max = ((Du32SafetyCfg_FlowWindowMs + (pstTask->u32Period * Du16Schd_TickMs) - 1U) /
                (pstTask->u32Period * Du16Schd_TickMs)) + Du32SafetyCfg_FlowTolerance;

      if (((u32Runs < u32Min) || (u32Runs > u32Max)) && (Du8Safety_TestPass == u8Ret))
        {
          *Fpu32TaskIdx = u16Index;
          u8Ret = Du8Safety_TestFail;
        }
    }

  return u8Ret;
}

/* Private Function Definitions ----------------------------------------------*/
