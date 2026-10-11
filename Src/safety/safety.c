/*
 * safety.c
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  Safety platform core: startup sequence, runtime task, fault log,
 *  safe state management.
 */


/* Includes ------------------------------------------------------------------*/

#include <stddef.h>
#include <string.h>
#include "safety_internal.h"
#include "../scheduler/scheduler.h"

/* Private Macros ------------------------------------------------------------*/

#define Du32Safety_LogMagic       0x5AFE10C5UL   //marks a valid fault log

//every reset flag of RCC->CSR
#define Du32Safety_ResetFlags     (RCC_CSR_BORRSTF | RCC_CSR_PINRSTF | RCC_CSR_PORRSTF | \
                                   RCC_CSR_SFTRSTF | RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF | \
                                   RCC_CSR_LPWRRSTF)

/* Private Types -------------------------------------------------------------*/

//fault log kept in no-init RAM: not cleared by the startup code, survives resets
typedef struct
{
  uint32_t u32Magic;        //Du32Safety_LogMagic when the log is valid
  uint32_t u32Boot;         //boot counter
  uint32_t u32Head;         //index where the next record is written
  uint32_t u32Count;        //number of valid records
  uint32_t u32FaultsInRow;  //safe state entries without a stable run in between
  TstSafety_FaultRecord astRecord[Du8SafetyCfg_FaultLogSize]; //circular buffer
  uint32_t u32Check;        //checksum of all the fields above
} TstSafety_FaultLog;

/* Private Variables ---------------------------------------------------------*/

//fault log, placed in .noinit by the linker script
static TstSafety_FaultLog tstSafety_Log __attribute__((section(".noinit")));

//current safety state
static volatile TenSafety_State enSafety_State = SAFETY_STATE_INIT;

//RCC->CSR reset flags read at startup
static uint32_t u32Safety_ResetCause = 0;

//time since vSafety_Start (ms), incremented by vSafety_Task
static uint32_t u32Safety_TimeMs = 0;

//time since the last clock cross-check was started (ms)
static uint32_t u32Safety_ClockTimerMs = 0;

//1 while a clock cross-check measurement is in progress
static uint8_t u8Safety_ClockBusy = 0;

//1 once the safe state has been entered (protects against re-entry)
static volatile uint8_t u8Safety_InSafeState = 0;

/* Private Function Prototypes -----------------------------------------------*/

static void vSafety_DwtInit(void);
static uint32_t u32Safety_LogChecksum(void);
static void vSafety_LogInit(void);
static void vSafety_LogWrite(const TstSafety_FaultRecord *Fpst_Record);
static void vSafety_EnterSafeState(void) __attribute__((noreturn));
static void vSafety_SchedOverrun(uint8_t Fu8Type, uint16_t Fu16TaskIdx, uint32_t Fu32Value);

/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: startup sequence and startup self-tests
//inputs:
//outputs:
/******************************************************************************/
void vSafety_Init(void)
{
  uint32_t u32Value = 0;  //detail returned by a failing test (address, CRC...)

  enSafety_State = SAFETY_STATE_INIT;
  vSafety_DwtInit();

  //capture then clear the reset flags
  u32Safety_ResetCause = RCC->CSR & Du32Safety_ResetFlags;
  RCC->CSR |= RCC_CSR_RMVF;

  vSafety_LogInit();
  if (u32Safety_ResetCause & RCC_CSR_IWDGRSTF)
    {
      vSafety_LogFault(SAFETY_FAULT_WATCHDOG_RESET, u32Safety_ResetCause);
    }

  //too many fault resets in a row: stay in safe state, do not try again
  if (tstSafety_Log.u32FaultsInRow >= Du8SafetyCfg_MaxFaultResets)
    {
      vSafety_EnterSafeState();
    }

  vSafetyFault_EnableExceptions();

#if (Du8SafetyCfg_CpuTestEnable == 1U)
  if (Du8Safety_TestPass != u8SafetySelftest_Cpu())
    {
      vSafety_ReportFault(SAFETY_FAULT_CPU_TEST, 0);
    }
#endif

#if (Du8SafetyCfg_FlashTestEnable == 1U)
  if (Du8Safety_TestPass != u8SafetySelftest_FlashFull(&u32Value))
    {
      vSafety_ReportFault(SAFETY_FAULT_FLASH_CRC, u32Value);
    }
#endif

#if (Du8SafetyCfg_RamTestEnable == 1U)
  if (Du8Safety_TestPass != u8SafetySelftest_RamFull(&u32Value))
    {
      vSafety_ReportFault(SAFETY_FAULT_RAM_TEST, u32Value);
    }
#endif

#if (Du8SafetyCfg_ClockTestEnable == 1U)
  if (Du8Safety_TestPass != u8SafetyClock_Init())
    {
      vSafety_ReportFault(SAFETY_FAULT_CLOCK_HSE, 0);
    }
  if (Du8Safety_TestPass != u8SafetyClock_CheckBlocking(&u32Value))
    {
      vSafety_ReportFault(SAFETY_FAULT_CLOCK_FREQ, u32Value);
    }
#endif

#if (Du8SafetyCfg_MpuEnable == 1U)
  if (Du8Safety_TestPass != u8SafetyMpu_Init())
    {
      vSafety_ReportFault(SAFETY_FAULT_CONFIG, 1U);
    }
#endif
}

/******************************************************************************/
//function desc: start the watchdog and runtime supervision
//inputs:
//outputs:
/******************************************************************************/
void vSafety_Start(void)
{
  if (SAFETY_STATE_INIT == enSafety_State)
    {
      u32Safety_TimeMs = 0;
      u32Safety_ClockTimerMs = 0;
      u8Safety_ClockBusy = 0;

      vSchd_RegisterOverrunCallback(vSafety_SchedOverrun);
      vSafetyFlow_StartSupervision();
      vSafetyFlow_StartWatchdog();

      enSafety_State = SAFETY_STATE_RUN;
    }
}

/******************************************************************************/
//function desc: runtime self-tests, supervision and watchdog refresh
//inputs:
//outputs:
/******************************************************************************/
void vSafety_Task(void)
{
  uint32_t u32Value = 0;  //detail returned by a failing check
  uint8_t u8Result = 0;   //result of one test step

  if (SAFETY_STATE_RUN != enSafety_State)
    {
      return;
    }

  u32Safety_TimeMs += Du32SafetyCfg_TaskPeriodMs;

  //system ran long enough without fault: clear the "faults in a row" counter
  if ((u32Safety_TimeMs >= Du32SafetyCfg_StableRunMs) && (tstSafety_Log.u32FaultsInRow != 0U))
    {
      tstSafety_Log.u32FaultsInRow = 0;
      tstSafety_Log.u32Check = u32Safety_LogChecksum();
    }

#if (Du8SafetyCfg_CpuTestEnable == 1U)
  if (Du8Safety_TestPass != u8SafetySelftest_Cpu())
    {
      vSafety_ReportFault(SAFETY_FAULT_CPU_TEST, 0);
    }
#endif

#if (Du8SafetyCfg_RamTestEnable == 1U)
  if (Du8Safety_TestFail == u8SafetySelftest_RamStep(&u32Value))
    {
      vSafety_ReportFault(SAFETY_FAULT_RAM_TEST, u32Value);
    }
#endif

#if (Du8SafetyCfg_FlashTestEnable == 1U)
  if (Du8Safety_TestFail == u8SafetySelftest_FlashStep(&u32Value))
    {
      vSafety_ReportFault(SAFETY_FAULT_FLASH_CRC, u32Value);
    }
#endif

#if (Du8SafetyCfg_ClockTestEnable == 1U)
  u32Safety_ClockTimerMs += Du32SafetyCfg_TaskPeriodMs;
  if (u8Safety_ClockBusy || (u32Safety_ClockTimerMs >= Du32SafetyCfg_ClockCheckPeriodMs))
    {
      u8Result = u8SafetyClock_Step(&u32Value);
      if (Du8Safety_TestBusy == u8Result)
        {
          u8Safety_ClockBusy = 1;
        }
      else
        {
          u8Safety_ClockBusy = 0;
          u32Safety_ClockTimerMs = 0;
          if (Du8Safety_TestFail == u8Result)
            {
              vSafety_ReportFault(SAFETY_FAULT_CLOCK_FREQ, u32Value);
            }
        }
    }
#endif
  (void)u8Result;

  if (Du8Safety_TestPass != u8SafetyFlow_Supervise(&u32Value))
    {
      vSafety_ReportFault(SAFETY_FAULT_TASK_ALIVE, u32Value);
    }

  //every check passed: the system is alive and healthy
  vSafetyFlow_RefreshWatchdog();
}

/******************************************************************************/
//function desc: record a fault and enter the safe state
//inputs: Fu16Code -> fault code, Fu32Info -> fault specific value
//outputs:
/******************************************************************************/
void vSafety_ReportFault(uint16_t Fu16Code, uint32_t Fu32Info)
{
  __disable_irq();
  vSafety_LogFault(Fu16Code, Fu32Info);
  vSafety_EnterSafeState();
}

/******************************************************************************/
//function desc: record a fault, keep running
//inputs: Fu16Code -> fault code, Fu32Info -> fault specific value
//outputs:
/******************************************************************************/
void vSafety_LogFault(uint16_t Fu16Code, uint32_t Fu32Info)
{
  TstSafety_FaultRecord stRecord;  //record being built

  memset(&stRecord, 0, sizeof(stRecord));
  stRecord.u16Code = Fu16Code;
  stRecord.u32Info = Fu32Info;
  vSafety_LogWrite(&stRecord);
}

/******************************************************************************/
//function desc: record a CPU fault (from safety_fault.c) and enter safe state
//inputs: Fpst_Record -> record filled by the fault handler
//outputs:
/******************************************************************************/
void vSafety_ReportCpuFault(const TstSafety_FaultRecord *Fpst_Record)
{
  vSafety_LogWrite(Fpst_Record);
  vSafety_EnterSafeState();
}

/******************************************************************************/
//function desc: busy wait using the DWT cycle counter
//inputs: Fu32Us -> delay in microseconds
//outputs:
/******************************************************************************/
void vSafety_DelayUs(uint32_t Fu32Us)
{
  uint32_t u32Start = DWT->CYCCNT;                              //counter at entry
  uint32_t u32Cycles = Fu32Us * (SystemCoreClock / 1000000UL);  //cycles to wait

  while ((DWT->CYCCNT - u32Start) < u32Cycles)
    {
    }
}

/******************************************************************************/
//function desc: status getters
//inputs:
//outputs:
/******************************************************************************/
TenSafety_State enSafety_GetState(void)
{
  return enSafety_State;
}

uint32_t u32Safety_GetResetCause(void)
{
  return u32Safety_ResetCause;
}

/******************************************************************************/
//function desc: fault log access, index 0 is the most recent record
//inputs:
//outputs:
/******************************************************************************/
uint16_t u16Safety_GetFaultCount(void)
{
  return (uint16_t)tstSafety_Log.u32Count;
}

int16_t s16Safety_GetFault(uint16_t Fu16Index, TstSafety_FaultRecord *Fpst_Record)
{
  int16_t s16Ret = -1;  //result, -1 until the record is found
  uint32_t u32Pos;      //position of the record in the circular buffer

  if ((Fpst_Record != NULL) && (Fu16Index < tstSafety_Log.u32Count))
    {
      //head points to the next free slot, the most recent record is just before
      u32Pos = (tstSafety_Log.u32Head + Du8SafetyCfg_FaultLogSize - 1U - Fu16Index) % Du8SafetyCfg_FaultLogSize;
      *Fpst_Record = tstSafety_Log.astRecord[u32Pos];
      s16Ret = 0;
    }

  return s16Ret;
}

void vSafety_ClearFaultLog(void)
{
  uint32_t u32Boot = tstSafety_Log.u32Boot;  //boot counter is kept

  memset(&tstSafety_Log, 0, sizeof(tstSafety_Log));
  tstSafety_Log.u32Magic = Du32Safety_LogMagic;
  tstSafety_Log.u32Boot = u32Boot;
  tstSafety_Log.u32Check = u32Safety_LogChecksum();
}

/******************************************************************************/
//function desc: default application hook, does nothing
//inputs:
//outputs:
/******************************************************************************/
__attribute__((weak)) void vSafetyApp_EnterSafeState(void)
{
}

/* Private Function Definitions ----------------------------------------------*/

//enable the DWT cycle counter (delays, scheduler budgets)
static void vSafety_DwtInit(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55UL;  //unlock DWT on Cortex-M7
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

//complemented sum of every word of the log except the checksum itself
static uint32_t u32Safety_LogChecksum(void)
{
  const uint32_t *pu32Word = (const uint32_t *)&tstSafety_Log;  //word being added
  uint32_t u32Words = (offsetof(TstSafety_FaultLog, u32Check)) / 4U;  //words to add
  uint32_t u32Sum = 0;    //running sum
  uint32_t u32Index;      //loop index

  for (u32Index = 0; u32Index < u32Words; u32Index++)
    {
      u32Sum += pu32Word[u32Index];
    }

  return ~u32Sum;
}

//validate the log left by the previous run, start a new one if corrupted
static void vSafety_LogInit(void)
{
  if ((Du32Safety_LogMagic != tstSafety_Log.u32Magic) ||
      (u32Safety_LogChecksum() != tstSafety_Log.u32Check) ||
      (tstSafety_Log.u32Head >= Du8SafetyCfg_FaultLogSize) ||
      (tstSafety_Log.u32Count > Du8SafetyCfg_FaultLogSize))
    {
      //power-on or corrupted content
      memset(&tstSafety_Log, 0, sizeof(tstSafety_Log));
      tstSafety_Log.u32Magic = Du32Safety_LogMagic;
    }

  tstSafety_Log.u32Boot++;
  tstSafety_Log.u32Check = u32Safety_LogChecksum();
}

//append one record to the circular log
static void vSafety_LogWrite(const TstSafety_FaultRecord *Fpst_Record)
{
  TstSafety_FaultRecord *pstSlot = &tstSafety_Log.astRecord[tstSafety_Log.u32Head]; //slot written

  *pstSlot = *Fpst_Record;
  pstSlot->u16Boot = (uint16_t)tstSafety_Log.u32Boot;
  pstSlot->u32TimeMs = u32Safety_TimeMs;

  tstSafety_Log.u32Head = (tstSafety_Log.u32Head + 1U) % Du8SafetyCfg_FaultLogSize;
  if (tstSafety_Log.u32Count < Du8SafetyCfg_FaultLogSize)
    {
      tstSafety_Log.u32Count++;
    }
  tstSafety_Log.u32Check = u32Safety_LogChecksum();
}

//put the outputs in safe state then reset or stay latched forever
static void vSafety_EnterSafeState(void)
{
  __disable_irq();  //stops the scheduler and every application interrupt

  if (0U == u8Safety_InSafeState)
    {
      u8Safety_InSafeState = 1;
      enSafety_State = SAFETY_STATE_SAFE;
      tstSafety_Log.u32FaultsInRow++;
      tstSafety_Log.u32Check = u32Safety_LogChecksum();
    }

  vSafetyApp_EnterSafeState();

#if (Du8SafetyCfg_ResetOnFault == 1U)
  if (tstSafety_Log.u32FaultsInRow < Du8SafetyCfg_MaxFaultResets)
    {
      NVIC_SystemReset();
    }
#endif

  //latched: keep the outputs safe, keep the watchdog quiet so the
  //system does not reset and leave the safe state on its own
  SystemCoreClockUpdate();  //clock may have changed (CSS switched to HSI)
  while (1)
    {
      vSafetyApp_EnterSafeState();
      if (u8SafetyFlow_IsWatchdogStarted())
        {
          IWDG->KR = Du32Safety_IwdgKeyReload;
        }
      vSafety_DelayUs(Du32SafetyCfg_TaskPeriodMs * 1000UL);
    }
}

//scheduler overrun notification (task budget exceeded or ticks lost)
static void vSafety_SchedOverrun(uint8_t Fu8Type, uint16_t Fu16TaskIdx, uint32_t Fu32Value)
{
  if (SAFETY_STATE_RUN == enSafety_State)
    {
      if (SCHD_OVERRUN_TASK == Fu8Type)
        {
          vSafety_ReportFault(SAFETY_FAULT_TASK_OVERRUN, Fu16TaskIdx);
        }
      else
        {
          vSafety_ReportFault(SAFETY_FAULT_TICK_OVERRUN, Fu32Value);
        }
    }
}
