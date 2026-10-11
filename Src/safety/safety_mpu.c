/*
 * safety_mpu.c
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  MPU configuration (privileged default map kept as background for
 *  peripherals and system space). Higher region number wins on overlap.
 *
 *   region 0: flash 1MB        read-only, executable
 *   region 1: RAM 320KB        read/write, never executable
 *   region 2: .dma_buffer      read/write, never executable, non-cacheable
 *   region 3: stack guard      no access  -> stack overflow = MemManage
 *   region 4: address 0 (ITCM) no access  -> NULL pointer = MemManage
 */


/* Includes ------------------------------------------------------------------*/

#include "safety_internal.h"

/* Private Macros ------------------------------------------------------------*/

#define Du32SafetyMpu_FlashBase   0x08000000UL
#define Du32SafetyMpu_FlashSize   (1024UL * 1024UL)
#define Du32SafetyMpu_RamBase     0x20000000UL
#define Du32SafetyMpu_RamSize     (512UL * 1024UL)  //region size (power of 2)
#define Du8SafetyMpu_RamSrd       0xE0U             //disable subregions 5-7: 512KB -> 320KB
#define Du32SafetyMpu_NullBase    0x00000000UL
#define Du32SafetyMpu_RegionNb    8U                //regions on the Cortex-M7 of the F746

#define Du8SafetyMpu_RegFlash     0U
#define Du8SafetyMpu_RegRam       1U
#define Du8SafetyMpu_RegDma       2U
#define Du8SafetyMpu_RegStack     3U
#define Du8SafetyMpu_RegNull      4U
#define Du8SafetyMpu_RegUsed      5U

/* Private Types -------------------------------------------------------------*/


/* Private Variables ---------------------------------------------------------*/


/* Private Function Prototypes -----------------------------------------------*/

static uint8_t u8SafetyMpu_SizeCode(uint32_t Fu32Size, uint32_t *Fpu32Code);

/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: configure and enable the MPU
//inputs:
//outputs: Du8Safety_TestPass, Du8Safety_TestFail on a configuration error
/******************************************************************************/
uint8_t u8SafetyMpu_Init(void)
{
  uint8_t u8Ret = Du8Safety_TestPass;               //result
  uint32_t u32DmaBase = (uint32_t)&_sdma_buffer;    //start of the DMA section
  uint32_t u32DmaUsed = (uint32_t)&_edma_buffer - u32DmaBase; //bytes used by DMA buffers
  uint32_t u32GuardBase;                            //stack guard start
  uint32_t u32SizeFlash = 0;                        //RASR size code, flash region
  uint32_t u32SizeRam = 0;                          //RASR size code, RAM region
  uint32_t u32SizeDma = 0;                          //RASR size code, DMA region
  uint32_t u32SizeGuard = 0;                        //RASR size code, stack guard
  uint32_t u32SizeNull = 0;                         //RASR size code, NULL guard
  uint32_t u32Region;                               //region index

  u32GuardBase = ((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size) & ~(Du32SafetyCfg_StackGuardSize - 1U);

  //every size must be a power of 2 and every base aligned on its size
  if ((Du8Safety_TestPass != u8SafetyMpu_SizeCode(Du32SafetyMpu_FlashSize, &u32SizeFlash)) ||
      (Du8Safety_TestPass != u8SafetyMpu_SizeCode(Du32SafetyMpu_RamSize, &u32SizeRam)) ||
      (Du8Safety_TestPass != u8SafetyMpu_SizeCode(Du32SafetyCfg_DmaRegionSize, &u32SizeDma)) ||
      (Du8Safety_TestPass != u8SafetyMpu_SizeCode(Du32SafetyCfg_StackGuardSize, &u32SizeGuard)) ||
      (Du8Safety_TestPass != u8SafetyMpu_SizeCode(Du32SafetyCfg_NullGuardSize, &u32SizeNull)) ||
      (0U != (u32DmaBase & (Du32SafetyCfg_DmaRegionSize - 1U))) ||
      (u32DmaUsed > Du32SafetyCfg_DmaRegionSize))
    {
      u8Ret = Du8Safety_TestFail;
    }

  if (Du8Safety_TestPass == u8Ret)
    {
      ARM_MPU_Disable();

      //flash: read-only, executable, normal memory write-through
      ARM_MPU_SetRegion(ARM_MPU_RBAR(Du8SafetyMpu_RegFlash, Du32SafetyMpu_FlashBase),
                        ARM_MPU_RASR(0U, ARM_MPU_AP_RO, 0U, 0U, 1U, 0U, 0U, u32SizeFlash));

      //RAM: read/write, execute never, normal memory write-back
      ARM_MPU_SetRegion(ARM_MPU_RBAR(Du8SafetyMpu_RegRam, Du32SafetyMpu_RamBase),
                        ARM_MPU_RASR(1U, ARM_MPU_AP_FULL, 1U, 0U, 1U, 1U, Du8SafetyMpu_RamSrd, u32SizeRam));

      //DMA buffers: read/write, execute never, shareable non-cacheable
      ARM_MPU_SetRegion(ARM_MPU_RBAR(Du8SafetyMpu_RegDma, u32DmaBase),
                        ARM_MPU_RASR(1U, ARM_MPU_AP_FULL, 1U, 1U, 0U, 0U, 0U, u32SizeDma));

      //stack guard: no access
      ARM_MPU_SetRegion(ARM_MPU_RBAR(Du8SafetyMpu_RegStack, u32GuardBase),
                        ARM_MPU_RASR(1U, ARM_MPU_AP_NONE, 0U, 0U, 0U, 0U, 0U, u32SizeGuard));

      //NULL guard: no access
      ARM_MPU_SetRegion(ARM_MPU_RBAR(Du8SafetyMpu_RegNull, Du32SafetyMpu_NullBase),
                        ARM_MPU_RASR(1U, ARM_MPU_AP_NONE, 0U, 0U, 0U, 0U, 0U, u32SizeNull));

      for (u32Region = Du8SafetyMpu_RegUsed; u32Region < Du32SafetyMpu_RegionNb; u32Region++)
        {
          ARM_MPU_ClrRegion(u32Region);
        }

      //background: privileged default memory map for everything else
      ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);
    }

  return u8Ret;
}

/* Private Function Definitions ----------------------------------------------*/

//RASR SIZE field = log2(size) - 1, size must be a power of 2 >= 32 bytes
static uint8_t u8SafetyMpu_SizeCode(uint32_t Fu32Size, uint32_t *Fpu32Code)
{
  uint8_t u8Ret = Du8Safety_TestFail;  //result
  uint32_t u32Log2;                    //log2 of the size

  if ((Fu32Size >= 32U) && (0U == (Fu32Size & (Fu32Size - 1U))))
    {
      u32Log2 = 31U - (uint32_t)__CLZ(Fu32Size);
      *Fpu32Code = u32Log2 - 1U;
      u8Ret = Du8Safety_TestPass;
    }

  return u8Ret;
}
