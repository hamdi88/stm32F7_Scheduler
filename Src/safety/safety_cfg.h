/*
 * safety_cfg.h
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  Project configuration of the safety platform.
 *  This is the only safety file a project is expected to edit.
 */

#ifndef SAFETY_SAFETY_CFG_H_
#define SAFETY_SAFETY_CFG_H_

/* General -------------------------------------------------------------------*/

//period of vSafety_Task in the scheduler (ms), all runtime checks are paced by it
#define Du32SafetyCfg_TaskPeriodMs        10UL

//reaction to a fault: 0 -> stay latched in safe state, 1 -> reset the MCU
#define Du8SafetyCfg_ResetOnFault         0U

//after this many fault resets in a row the system stays latched in safe state
#define Du8SafetyCfg_MaxFaultResets       3U

//fault-free run time after which the "faults in a row" counter is cleared (ms)
#define Du32SafetyCfg_StableRunMs         60000UL

//number of fault records kept in the no-init RAM log (survives resets)
#define Du8SafetyCfg_FaultLogSize         8U

/* CPU register test ---------------------------------------------------------*/

#define Du8SafetyCfg_CpuTestEnable        1U

/* RAM test (March C-, transparent) ------------------------------------------*/

#define Du8SafetyCfg_RamTestEnable        1U

//bytes tested per vSafety_Task call (interrupts off during one block)
//320KB / 256B * 10ms -> full RAM covered every ~13s
#define Du32SafetyCfg_RamBlockSize        256UL

/* Flash CRC test ------------------------------------------------------------*/

#define Du8SafetyCfg_FlashTestEnable      1U

//bytes of flash checked per vSafety_Task call (multiple of 4)
#define Du32SafetyCfg_FlashStepBytes      1024UL

//1 -> accept an image whose CRC was not patched by tools/flash_crc.py
//(development only, must be 0 in a released product)
#define Du8SafetyCfg_FlashAllowUnpatched  1U

/* Clock monitoring ----------------------------------------------------------*/

#define Du8SafetyCfg_ClockTestEnable      1U

//1 -> the PLL must run from the HSE crystal (HSI fallback is a fault)
#define Du8SafetyCfg_RequireHse           1U

//period of the SYSCLK vs LSI frequency cross-check (ms)
#define Du32SafetyCfg_ClockCheckPeriodMs  1000UL

//LSI datasheet limits: the check only detects gross clock failures
#define Du32SafetyCfg_LsiMinHz            17000UL
#define Du32SafetyCfg_LsiMaxHz            47000UL

/* Watchdog (IWDG with window) -----------------------------------------------*/
//IWDG clock = LSI / 4 -> 4.25kHz..11.75kHz (8kHz nominal)
//refresh every Du32SafetyCfg_TaskPeriodMs (10ms):
//  timeout = 200 ticks       -> 17ms..47ms   (> 10ms in all cases)
//  window  = 200 - 170 ticks -> refresh earlier than 2.5ms..7ms resets the MCU

//IWDG_PR register value: 0 -> /4, 1 -> /8, 2 -> /16 ...
#define Du32SafetyCfg_IwdgPrescaler       0UL
#define Du32SafetyCfg_IwdgReload          200UL
#define Du32SafetyCfg_IwdgWindow          170UL

//1 -> watchdog is frozen while the core is halted by the debugger
#define Du8SafetyCfg_WdgFreezeInDebug     1U

/* Task alive supervision ----------------------------------------------------*/
//every scheduler task must run (window / period) times per window, +/- tolerance
//the window must be at least 2x the longest task period

#define Du32SafetyCfg_FlowWindowMs        1000UL
#define Du32SafetyCfg_FlowTolerance       1UL

/* MPU -----------------------------------------------------------------------*/

#define Du8SafetyCfg_MpuEnable            1U

//no-access region at the bottom of the stack, catches stack overflow
#define Du32SafetyCfg_StackGuardSize      32UL

//no-access region at address 0 (ITCM RAM), catches NULL pointer accesses
#define Du32SafetyCfg_NullGuardSize       16384UL

//size of the .dma_buffer region (non-cacheable), must match the linker script
#define Du32SafetyCfg_DmaRegionSize       32768UL

#endif /* SAFETY_SAFETY_CFG_H_ */
