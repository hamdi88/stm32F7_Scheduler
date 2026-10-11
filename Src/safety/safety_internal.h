/*
 * safety_internal.h
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  Functions and symbols shared between the safety modules,
 *  not part of the public API.
 */

#ifndef SAFETY_SAFETY_INTERNAL_H_
#define SAFETY_SAFETY_INTERNAL_H_


/* Includes ------------------------------------------------------------------*/

#include "safety.h"

/* Public Macros -------------------------------------------------------------*/

//result of a test step
#define Du8Safety_TestPass        0U
#define Du8Safety_TestFail        1U
#define Du8Safety_TestBusy        2U   //runtime test not finished yet

//value of the flash CRC word before tools/flash_crc.py patches it
#define Du32Safety_CrcUnpatched   0xFFFFFFFFUL

//IWDG keys
#define Du32Safety_IwdgKeyStart   0x0000CCCCUL
#define Du32Safety_IwdgKeyReload  0x0000AAAAUL
#define Du32Safety_IwdgKeyUnlock  0x00005555UL

/* Linker symbols (STM32F746NGHX_FLASH.ld) -----------------------------------*/

extern uint32_t _estack;          //top of the stack (end of RAM)
extern uint32_t _Min_Stack_Size;  //stack size, the symbol address is the value
extern uint32_t _sdma_buffer;     //start of the .dma_buffer section
extern uint32_t _edma_buffer;     //end of the .dma_buffer section
extern uint32_t _sflash_crc;      //address of the flash CRC word (end of image)

/* Public Function Prototypes ------------------------------------------------*/

/* safety.c */
//record a fault with CPU context and enter safe state (fault handlers)
void vSafety_ReportCpuFault(const TstSafety_FaultRecord *Fpst_Record) __attribute__((noreturn));
//busy wait using the DWT cycle counter
void vSafety_DelayUs(uint32_t Fu32Us);

/* safety_cpu_test.s */
//test R0-R12 and APSR flags, returns 0 on success
uint32_t u32SafetyCpu_TestRegisters(void);

/* safety_selftest.c */
uint8_t u8SafetySelftest_Cpu(void);
uint8_t u8SafetySelftest_RamFull(uint32_t *Fpu32FailAddr);
uint8_t u8SafetySelftest_RamStep(uint32_t *Fpu32FailAddr);
uint8_t u8SafetySelftest_FlashFull(uint32_t *Fpu32Crc);
uint8_t u8SafetySelftest_FlashStep(uint32_t *Fpu32Crc);

/* safety_clock.c */
uint8_t u8SafetyClock_Init(void);
uint8_t u8SafetyClock_CheckBlocking(uint32_t *Fpu32Measured);
uint8_t u8SafetyClock_Step(uint32_t *Fpu32Measured);

/* safety_flow.c */
void vSafetyFlow_StartWatchdog(void);
void vSafetyFlow_RefreshWatchdog(void);
uint8_t u8SafetyFlow_IsWatchdogStarted(void);
void vSafetyFlow_StartSupervision(void);
uint8_t u8SafetyFlow_Supervise(uint32_t *Fpu32TaskIdx);

/* safety_fault.c */
void vSafetyFault_EnableExceptions(void);

/* safety_mpu.c */
uint8_t u8SafetyMpu_Init(void);

#endif /* SAFETY_SAFETY_INTERNAL_H_ */
