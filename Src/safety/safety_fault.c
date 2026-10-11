/*
 * safety_fault.c
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  CPU fault exception handlers.
 *  Each handler is a small assembly entry that:
 *   1. finds the stacked exception frame (MSP or PSP)
 *   2. moves SP back to the top of RAM: the faulting stack may be
 *      overflowed or corrupted, the handler never returns anyway
 *   3. jumps to vSafetyFault_Handler which records PC, LR, fault status
 *      registers and the fault address, then enters the safe state.
 *
 *  Note: a stack overflow hitting the MPU guard while the CPU pushes an
 *  exception frame can lock up the core; the IWDG then resets the MCU and
 *  the next boot logs SAFETY_FAULT_WATCHDOG_RESET.
 */


/* Includes ------------------------------------------------------------------*/

#include <string.h>
#include "safety_internal.h"

/* Private Macros ------------------------------------------------------------*/

#define Du32SafetyFault_RamStart  0x20000000UL  //STM32F746 RAM origin
#define Du32SafetyFault_FrameSize 32UL          //basic exception frame (8 words)
#define Du8SafetyFault_FramePc    6U            //stacked PC word index
#define Du8SafetyFault_FrameLr    5U            //stacked LR word index

//assembly entry shared by every fault handler, Fcode = fault code
#define DSafetyFault_Entry(Fcode)                    \
  __asm volatile (                                   \
    "tst lr, #4               \n" /* EXC_RETURN bit2: 0 = MSP, 1 = PSP */ \
    "ite eq                   \n"                    \
    "mrseq r0, msp            \n" /* r0 = exception frame              */ \
    "mrsne r0, psp            \n"                    \
    "mov r1, lr               \n" /* r1 = EXC_RETURN                   */ \
    "movw r2, %0              \n" /* r2 = fault code                   */ \
    "ldr r3, =_estack         \n" /* fresh stack at the top of RAM     */ \
    "mov sp, r3               \n"                    \
    "b vSafetyFault_Handler   \n"                    \
    : : "i" (Fcode))

/* Private Types -------------------------------------------------------------*/


/* Private Variables ---------------------------------------------------------*/


/* Private Function Prototypes -----------------------------------------------*/

void vSafetyFault_Handler(uint32_t *Fpu32Frame, uint32_t Fu32ExcReturn, uint32_t Fu32Code) __attribute__((noreturn, used));
static uint8_t u8SafetyFault_FrameValid(const uint32_t *Fpu32Frame);

/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: enable MemManage, BusFault and UsageFault exceptions (instead
//               of everything escalating to HardFault), trap division by zero
//inputs:
//outputs:
/******************************************************************************/
void vSafetyFault_EnableExceptions(void)
{
  NVIC_SetPriority(MemoryManagement_IRQn, 0);
  NVIC_SetPriority(BusFault_IRQn, 0);
  NVIC_SetPriority(UsageFault_IRQn, 0);

  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
  SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;
  __DSB();
  __ISB();
}

/******************************************************************************/
//function desc: exception handlers (override the weak startup handlers)
//inputs:
//outputs:
/******************************************************************************/
__attribute__((naked)) void HardFault_Handler(void)
{
  DSafetyFault_Entry(SAFETY_FAULT_HARDFAULT);
}

__attribute__((naked)) void MemManage_Handler(void)
{
  DSafetyFault_Entry(SAFETY_FAULT_MEMMANAGE);
}

__attribute__((naked)) void BusFault_Handler(void)
{
  DSafetyFault_Entry(SAFETY_FAULT_BUSFAULT);
}

__attribute__((naked)) void UsageFault_Handler(void)
{
  DSafetyFault_Entry(SAFETY_FAULT_USAGEFAULT);
}

/******************************************************************************/
//function desc: NMI: clock security system (HSE failure) or unexpected NMI
//inputs:
//outputs:
/******************************************************************************/
void NMI_Handler(void)
{
  if (RCC->CIR & RCC_CIR_CSSF)
    {
      RCC->CIR |= RCC_CIR_CSSC;  //clear, otherwise the NMI fires again
      vSafety_ReportFault(SAFETY_FAULT_CLOCK_HSE, 0);
    }
  vSafety_ReportFault(SAFETY_FAULT_NMI, 0);
}

/******************************************************************************/
//function desc: common C part of the fault handlers, runs on a fresh stack
//inputs: Fpu32Frame -> stacked frame, Fu32ExcReturn -> EXC_RETURN,
//        Fu32Code -> fault code from the entry stub
//outputs:
/******************************************************************************/
void vSafetyFault_Handler(uint32_t *Fpu32Frame, uint32_t Fu32ExcReturn, uint32_t Fu32Code)
{
  TstSafety_FaultRecord stRecord;  //record sent to the fault log
  uint32_t u32Cfsr;                //configurable fault status
  uint32_t u32Mmfar;               //MemManage fault address
  uint32_t u32GuardStart;          //start of the stack guard region
  uint32_t u32GuardEnd;            //end of the stack guard region

  __disable_irq();
  //the frame may sit in the stack guard: MPU off so it can be read
  MPU->CTRL = 0;
  __DSB();
  __ISB();

  memset(&stRecord, 0, sizeof(stRecord));
  u32Cfsr = SCB->CFSR;
  u32Mmfar = SCB->MMFAR;
  stRecord.u16Code = (uint16_t)Fu32Code;
  stRecord.u32Info = Fu32ExcReturn;
  stRecord.u32Cfsr = u32Cfsr;
  stRecord.u32Hfsr = SCB->HFSR;

  if (u32Cfsr & SCB_CFSR_MMARVALID_Msk)
    {
      stRecord.u32FaultAddr = u32Mmfar;
    }
  else if (u32Cfsr & SCB_CFSR_BFARVALID_Msk)
    {
      stRecord.u32FaultAddr = SCB->BFAR;
    }

  //stack overflow: stacking error or access inside the stack guard
  u32GuardStart = ((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size) & ~(Du32SafetyCfg_StackGuardSize - 1U);
  u32GuardEnd = u32GuardStart + Du32SafetyCfg_StackGuardSize;
  if ((u32Cfsr & SCB_CFSR_MSTKERR_Msk) ||
      ((u32Cfsr & SCB_CFSR_MMARVALID_Msk) && (u32Mmfar >= u32GuardStart) && (u32Mmfar < u32GuardEnd)))
    {
      stRecord.u16Code = SAFETY_FAULT_STACK_OVERFLOW;
    }

  //PC and LR only if the frame pointer is plausible
  if (u8SafetyFault_FrameValid(Fpu32Frame))
    {
      stRecord.u32Pc = Fpu32Frame[Du8SafetyFault_FramePc];
      stRecord.u32Lr = Fpu32Frame[Du8SafetyFault_FrameLr];
    }

  //clear the sticky status bits for the next run
  SCB->CFSR = u32Cfsr;
  SCB->HFSR = SCB->HFSR;

  vSafety_ReportCpuFault(&stRecord);
}

/* Private Function Definitions ----------------------------------------------*/

//1 if the whole frame is inside RAM (a corrupted SP would fault again)
static uint8_t u8SafetyFault_FrameValid(const uint32_t *Fpu32Frame)
{
  uint32_t u32Addr = (uint32_t)Fpu32Frame;  //frame address

  return ((u32Addr >= Du32SafetyFault_RamStart) &&
          ((u32Addr + Du32SafetyFault_FrameSize) <= (uint32_t)&_estack) &&
          (0U == (u32Addr & 3U))) ? 1U : 0U;
}
