/*
 * safety.h
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  Generic safety platform for STM32F7 bare-metal projects.
 *
 *  Provides:
 *   - startup self-tests: CPU registers, RAM (March C-), flash CRC, clock
 *   - runtime self-tests run piece by piece from vSafety_Task
 *   - windowed watchdog refreshed only when every scheduler task is alive
 *   - scheduler task overrun / tick overrun detection
 *   - CPU fault handlers (HardFault, MemManage, BusFault, UsageFault, NMI)
 *   - MPU: read-only flash, no-execute RAM, stack guard, NULL guard
 *   - fault log kept in no-init RAM across resets, reset cause
 *   - one safe state entry point, the project defines what "safe" means
 *     by implementing vSafetyApp_EnterSafeState()
 *
 *  Usage in main():
 *    vMain_SystemClockConfig();
 *    vSafety_Init();                                   // startup tests
 *    ... init drivers, s16Schd_Init(), add tasks ...
 *    s16Schd_AddTask(vSafety_Task, 0, Du32SafetyCfg_TaskPeriodMs);
 *    vSafety_Start();                                  // watchdog + supervision
 *    vSchd_Enable(SCHD_ENABLE);
 */

#ifndef SAFETY_SAFETY_H_
#define SAFETY_SAFETY_H_


/* Includes ------------------------------------------------------------------*/

#include <stdint.h>
#include "stm32f7xx.h"
#include "safety_cfg.h"

/* Public Macros -------------------------------------------------------------*/

//first fault code free for the application (vSafety_ReportFault)
#define Du16Safety_FaultAppBase   0x0100U

/* Public Types --------------------------------------------------------------*/

typedef enum
{
  SAFETY_STATE_INIT = 0,  //startup tests running
  SAFETY_STATE_RUN,       //normal operation
  SAFETY_STATE_SAFE       //fault detected, outputs in safe state (latched)
} TenSafety_State;

//platform fault codes, application codes start at Du16Safety_FaultAppBase
enum
{
  SAFETY_FAULT_NONE = 0,
  SAFETY_FAULT_CPU_TEST,        //CPU register test failed
  SAFETY_FAULT_RAM_TEST,        //RAM March test failed, info = address
  SAFETY_FAULT_FLASH_CRC,       //flash CRC mismatch, info = computed CRC
  SAFETY_FAULT_CLOCK_HSE,       //HSE missing/failed (CSS), info = 0
  SAFETY_FAULT_CLOCK_FREQ,      //SYSCLK vs LSI out of range, info = measured
  SAFETY_FAULT_WATCHDOG_RESET,  //previous run ended by watchdog (log only)
  SAFETY_FAULT_TASK_ALIVE,      //task ran too few/too many times, info = task idx
  SAFETY_FAULT_TASK_OVERRUN,    //task exceeded its budget, info = task idx
  SAFETY_FAULT_TICK_OVERRUN,    //scheduler late by several ticks, info = ticks
  SAFETY_FAULT_HARDFAULT,
  SAFETY_FAULT_MEMMANAGE,
  SAFETY_FAULT_BUSFAULT,
  SAFETY_FAULT_USAGEFAULT,
  SAFETY_FAULT_STACK_OVERFLOW,  //MPU stack guard hit
  SAFETY_FAULT_NMI,             //unexpected NMI
  SAFETY_FAULT_CONFIG,          //platform misconfiguration, info = detail
};

//one entry of the fault log
typedef struct
{
  uint16_t u16Code;       //fault code
  uint16_t u16Boot;       //boot number when the fault happened
  uint32_t u32Info;       //fault specific value (address, task index...)
  uint32_t u32TimeMs;     //time since vSafety_Start (ms)
  uint32_t u32Pc;         //stacked PC (CPU faults only)
  uint32_t u32Lr;         //stacked LR (CPU faults only)
  uint32_t u32Cfsr;       //SCB->CFSR (CPU faults only)
  uint32_t u32Hfsr;       //SCB->HFSR (CPU faults only)
  uint32_t u32FaultAddr;  //MMFAR or BFAR when valid (CPU faults only)
} TstSafety_FaultRecord;

/* Public Function Prototypes ------------------------------------------------*/

/******************************************************************************/
//function desc: startup sequence: reset cause, fault log, fault exceptions,
//               CPU/flash/RAM/clock self-tests, MPU. Call right after the
//               clock configuration. Never returns if a test fails.
//inputs:
//outputs:
/******************************************************************************/
void vSafety_Init(void);

/******************************************************************************/
//function desc: start the watchdog and runtime supervision. Call once all
//               scheduler tasks are added, just before enabling the scheduler.
//inputs:
//outputs:
/******************************************************************************/
void vSafety_Start(void);

/******************************************************************************/
//function desc: scheduler task, period Du32SafetyCfg_TaskPeriodMs: runtime
//               self-tests, task alive supervision, watchdog refresh
//inputs:
//outputs:
/******************************************************************************/
void vSafety_Task(void);

/******************************************************************************/
//function desc: record a fault and enter the safe state (never returns)
//inputs: Fu16Code -> fault code, Fu32Info -> fault specific value
//outputs:
/******************************************************************************/
void vSafety_ReportFault(uint16_t Fu16Code, uint32_t Fu32Info) __attribute__((noreturn));

/******************************************************************************/
//function desc: record an event in the fault log without entering safe state
//inputs: Fu16Code -> fault code, Fu32Info -> fault specific value
//outputs:
/******************************************************************************/
void vSafety_LogFault(uint16_t Fu16Code, uint32_t Fu32Info);

/******************************************************************************/
//function desc: status getters
//inputs:
//outputs: current safety state / RCC->CSR reset flags captured at startup
/******************************************************************************/
TenSafety_State enSafety_GetState(void);
uint32_t u32Safety_GetResetCause(void);

/******************************************************************************/
//function desc: fault log access, index 0 is the most recent record
//inputs: Fu16Index -> record index, Fpst_Record -> destination
//outputs: number of records / 0 on success, -1 if index is out of range
/******************************************************************************/
uint16_t u16Safety_GetFaultCount(void);
int16_t s16Safety_GetFault(uint16_t Fu16Index, TstSafety_FaultRecord *Fpst_Record);
void vSafety_ClearFaultLog(void);

/******************************************************************************/
//function desc: APPLICATION HOOK, put every output in its safe state
//               (de-energize relays, stop motors, close valves...).
//               Called from fault/interrupt context and repeatedly while
//               latched: must be short, must not block, must not use the
//               scheduler. Default (weak) implementation does nothing.
//inputs:
//outputs:
/******************************************************************************/
void vSafetyApp_EnterSafeState(void);

#endif /* SAFETY_SAFETY_H_ */
