/*
 * safety_selftest.c
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  CPU, RAM and flash self-tests.
 *
 *  RAM: transparent March C- (content saved, tested, restored) one block at a
 *  time with interrupts disabled. Tested range: RAM start up to the stack.
 *  Not tested: the stack (in use), the .dma_buffer section (written by DMA
 *  at any time). The backup buffer itself is tested destructively.
 *
 *  Flash: CRC32 (hardware CRC unit, poly 0x04C11DB7, init 0xFFFFFFFF) of the
 *  image from the flash start to the CRC word placed at the end of the image
 *  by the linker. tools/flash_crc.py writes the expected value after link.
 */


/* Includes ------------------------------------------------------------------*/

#include "safety_internal.h"
#include "stm32f7xx_ll_bus.h"

/* Private Macros ------------------------------------------------------------*/

#define Du32SafetyRam_Start       0x20000000UL   //STM32F746 RAM origin (DTCM)
#define Du32SafetyRam_BlockWords  (Du32SafetyCfg_RamBlockSize / 4U)
#define Du32SafetyRam_Pattern1    0x00000000UL   //March background 1
#define Du32SafetyRam_Pattern2    0x55555555UL   //March background 2 (checkerboard)

#define Du32SafetyFlash_Start     0x08000000UL   //STM32F746 flash origin
#define Du32SafetyFlash_CrcPoly   0x04C11DB7UL
#define Du32SafetyFlash_CrcInit   0xFFFFFFFFUL

/* Private Types -------------------------------------------------------------*/


/* Private Variables ---------------------------------------------------------*/

//expected CRC of the flash image, last word of the image, patched after link
const uint32_t u32SafetyFlash_StoredCrc __attribute__((section(".flash_crc"), used)) = Du32Safety_CrcUnpatched;

//copy of the RAM block under test, aligned so it is exactly one test block
static uint32_t au32SafetyRam_Backup[Du32SafetyRam_BlockWords] __attribute__((aligned(Du32SafetyCfg_RamBlockSize)));

//next RAM block tested by u8SafetySelftest_RamStep
static uint32_t u32SafetyRam_Addr = Du32SafetyRam_Start;

//next flash address fed to the CRC unit by u8SafetySelftest_FlashStep
static uint32_t u32SafetyFlash_Addr = Du32SafetyFlash_Start;

/* Private Function Prototypes -----------------------------------------------*/

static uint32_t u32SafetyRam_End(void);
static uint8_t u8SafetyRam_IsExcluded(uint32_t Fu32Addr);
static uint8_t u8SafetyRam_March(volatile uint32_t *Fpu32Block, uint32_t Fu32Words, uint32_t Fu32Bg);
static uint8_t u8SafetyRam_TestBlock(uint32_t Fu32Addr);
static void vSafetyFlash_CrcReset(void);
static void vSafetyFlash_CrcFeed(uint32_t Fu32Addr, uint32_t Fu32Bytes);
static uint8_t u8SafetyFlash_Compare(uint32_t Fu32Crc);

/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: CPU register and flag test
//inputs:
//outputs: Du8Safety_TestPass / Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetySelftest_Cpu(void)
{
  return (0U == u32SafetyCpu_TestRegisters()) ? Du8Safety_TestPass : Du8Safety_TestFail;
}

/******************************************************************************/
//function desc: full RAM test (startup)
//inputs: Fpu32FailAddr -> address of the failing block
//outputs: Du8Safety_TestPass / Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetySelftest_RamFull(uint32_t *Fpu32FailAddr)
{
  uint8_t u8Ret = Du8Safety_TestPass;  //test result
  uint32_t u32End = u32SafetyRam_End(); //first address not tested
  uint32_t u32Addr;                     //block under test

  for (u32Addr = Du32SafetyRam_Start; (u32Addr < u32End) && (Du8Safety_TestPass == u8Ret); u32Addr += Du32SafetyCfg_RamBlockSize)
    {
      if (!u8SafetyRam_IsExcluded(u32Addr))
        {
          u8Ret = u8SafetyRam_TestBlock(u32Addr);
          *Fpu32FailAddr = u32Addr;
        }
    }

  return u8Ret;
}

/******************************************************************************/
//function desc: test the next RAM block (runtime)
//inputs: Fpu32FailAddr -> address of the failing block
//outputs: Du8Safety_TestPass (cycle complete), Du8Safety_TestBusy, Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetySelftest_RamStep(uint32_t *Fpu32FailAddr)
{
  uint8_t u8Ret = Du8Safety_TestBusy;   //test result
  uint32_t u32End = u32SafetyRam_End(); //first address not tested

  //skip regions that must not be tested
  while ((u32SafetyRam_Addr < u32End) && u8SafetyRam_IsExcluded(u32SafetyRam_Addr))
    {
      u32SafetyRam_Addr += Du32SafetyCfg_RamBlockSize;
    }

  if (u32SafetyRam_Addr >= u32End)
    {
      //whole RAM covered, start a new cycle
      u32SafetyRam_Addr = Du32SafetyRam_Start;
      u8Ret = Du8Safety_TestPass;
    }
  else
    {
      *Fpu32FailAddr = u32SafetyRam_Addr;
      if (Du8Safety_TestPass != u8SafetyRam_TestBlock(u32SafetyRam_Addr))
        {
          u8Ret = Du8Safety_TestFail;
        }
      u32SafetyRam_Addr += Du32SafetyCfg_RamBlockSize;
    }

  return u8Ret;
}

/******************************************************************************/
//function desc: full flash CRC check (startup)
//inputs: Fpu32Crc -> computed CRC
//outputs: Du8Safety_TestPass / Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetySelftest_FlashFull(uint32_t *Fpu32Crc)
{
  uint32_t u32End = (uint32_t)&_sflash_crc;  //first address not covered (CRC word)

  vSafetyFlash_CrcReset();
  vSafetyFlash_CrcFeed(Du32SafetyFlash_Start, u32End - Du32SafetyFlash_Start);
  *Fpu32Crc = CRC->DR;

  //the runtime check restarts from the beginning
  u32SafetyFlash_Addr = Du32SafetyFlash_Start;

  return u8SafetyFlash_Compare(*Fpu32Crc);
}

/******************************************************************************/
//function desc: CRC of the next flash chunk (runtime)
//inputs: Fpu32Crc -> computed CRC when the cycle completes
//outputs: Du8Safety_TestPass (cycle complete), Du8Safety_TestBusy, Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetySelftest_FlashStep(uint32_t *Fpu32Crc)
{
  uint8_t u8Ret = Du8Safety_TestBusy;        //test result
  uint32_t u32End = (uint32_t)&_sflash_crc;  //first address not covered (CRC word)
  uint32_t u32Bytes;                         //bytes fed in this step

  if (Du32SafetyFlash_Start == u32SafetyFlash_Addr)
    {
      vSafetyFlash_CrcReset();
    }

  u32Bytes = u32End - u32SafetyFlash_Addr;
  if (u32Bytes > Du32SafetyCfg_FlashStepBytes)
    {
      u32Bytes = Du32SafetyCfg_FlashStepBytes;
    }
  vSafetyFlash_CrcFeed(u32SafetyFlash_Addr, u32Bytes);
  u32SafetyFlash_Addr += u32Bytes;

  if (u32SafetyFlash_Addr >= u32End)
    {
      *Fpu32Crc = CRC->DR;
      u8Ret = u8SafetyFlash_Compare(*Fpu32Crc);
      u32SafetyFlash_Addr = Du32SafetyFlash_Start;
    }

  return u8Ret;
}

/* Private Function Definitions ----------------------------------------------*/

//end of the tested RAM range: bottom of the stack, aligned down to a block
static uint32_t u32SafetyRam_End(void)
{
  uint32_t u32StackBottom = (uint32_t)&_estack - (uint32_t)&_Min_Stack_Size; //lowest stack address

  return u32StackBottom & ~(Du32SafetyCfg_RamBlockSize - 1U);
}

//1 if the block overlaps the DMA section (DMA may write it at any time)
static uint8_t u8SafetyRam_IsExcluded(uint32_t Fu32Addr)
{
  uint32_t u32DmaStart = (uint32_t)&_sdma_buffer;  //start of the DMA section
  uint32_t u32DmaEnd = (uint32_t)&_edma_buffer;    //end of the DMA section

  return ((Fu32Addr + Du32SafetyCfg_RamBlockSize > u32DmaStart) && (Fu32Addr < u32DmaEnd)) ? 1U : 0U;
}

//March C- with background Fu32Bg:
//up(w0) up(r0,w1) up(r1,w0) down(r0,w1) down(r1,w0) down(r0)
static uint8_t u8SafetyRam_March(volatile uint32_t *Fpu32Block, uint32_t Fu32Words, uint32_t Fu32Bg)
{
  uint32_t u32Inv = ~Fu32Bg;  //inverted background
  uint32_t u32Index;          //word index

  for (u32Index = 0; u32Index < Fu32Words; u32Index++)
    {
      Fpu32Block[u32Index] = Fu32Bg;
    }
  for (u32Index = 0; u32Index < Fu32Words; u32Index++)
    {
      if (Fpu32Block[u32Index] != Fu32Bg) { return Du8Safety_TestFail; }
      Fpu32Block[u32Index] = u32Inv;
    }
  for (u32Index = 0; u32Index < Fu32Words; u32Index++)
    {
      if (Fpu32Block[u32Index] != u32Inv) { return Du8Safety_TestFail; }
      Fpu32Block[u32Index] = Fu32Bg;
    }
  for (u32Index = Fu32Words; u32Index > 0U; u32Index--)
    {
      if (Fpu32Block[u32Index - 1U] != Fu32Bg) { return Du8Safety_TestFail; }
      Fpu32Block[u32Index - 1U] = u32Inv;
    }
  for (u32Index = Fu32Words; u32Index > 0U; u32Index--)
    {
      if (Fpu32Block[u32Index - 1U] != u32Inv) { return Du8Safety_TestFail; }
      Fpu32Block[u32Index - 1U] = Fu32Bg;
    }
  for (u32Index = Fu32Words; u32Index > 0U; u32Index--)
    {
      if (Fpu32Block[u32Index - 1U] != Fu32Bg) { return Du8Safety_TestFail; }
    }

  return Du8Safety_TestPass;
}

//transparent test of one block: save, March with 2 backgrounds, restore
static uint8_t u8SafetyRam_TestBlock(uint32_t Fu32Addr)
{
  volatile uint32_t *pu32Block = (volatile uint32_t *)Fu32Addr;  //block under test
  uint8_t u8Ret;          //test result
  uint32_t u32Primask;    //interrupt state at entry
  uint32_t u32Index;      //word index

  if (Fu32Addr == (uint32_t)au32SafetyRam_Backup)
    {
      //the backup buffer holds nothing between calls: destructive test
      u8Ret = u8SafetyRam_March(pu32Block, Du32SafetyRam_BlockWords, Du32SafetyRam_Pattern1);
      if (Du8Safety_TestPass == u8Ret)
        {
          u8Ret = u8SafetyRam_March(pu32Block, Du32SafetyRam_BlockWords, Du32SafetyRam_Pattern2);
        }
    }
  else
    {
      //nobody may use the block while its content is destroyed
      u32Primask = __get_PRIMASK();
      __disable_irq();

      for (u32Index = 0; u32Index < Du32SafetyRam_BlockWords; u32Index++)
        {
          au32SafetyRam_Backup[u32Index] = pu32Block[u32Index];
        }
      __DMB();  //backup complete before the block is overwritten (compiler + CPU barrier)

      u8Ret = u8SafetyRam_March(pu32Block, Du32SafetyRam_BlockWords, Du32SafetyRam_Pattern1);
      if (Du8Safety_TestPass == u8Ret)
        {
          u8Ret = u8SafetyRam_March(pu32Block, Du32SafetyRam_BlockWords, Du32SafetyRam_Pattern2);
        }
      __DMB();  //March finished before the content is restored

      for (u32Index = 0; u32Index < Du32SafetyRam_BlockWords; u32Index++)
        {
          pu32Block[u32Index] = au32SafetyRam_Backup[u32Index];
        }

      __set_PRIMASK(u32Primask);
    }

  return u8Ret;
}

//enable and reset the hardware CRC unit (CRC-32/MPEG-2 settings)
static void vSafetyFlash_CrcReset(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);
  CRC->INIT = Du32SafetyFlash_CrcInit;
  CRC->POL = Du32SafetyFlash_CrcPoly;
  CRC->CR = CRC_CR_RESET;  //32-bit polynomial, no bit reversal, load INIT
}

//feed Fu32Bytes (multiple of 4) of flash to the CRC unit
static void vSafetyFlash_CrcFeed(uint32_t Fu32Addr, uint32_t Fu32Bytes)
{
  const volatile uint32_t *pu32Word = (const volatile uint32_t *)Fu32Addr;  //word read
  uint32_t u32Count = Fu32Bytes / 4U;  //words left
  uint32_t u32Index;                   //word index

  for (u32Index = 0; u32Index < u32Count; u32Index++)
    {
      CRC->DR = pu32Word[u32Index];
    }
}

//compare with the stored CRC (read through volatile: patched after compile)
static uint8_t u8SafetyFlash_Compare(uint32_t Fu32Crc)
{
  uint32_t u32Stored = *(const volatile uint32_t *)&_sflash_crc;  //expected CRC
  uint8_t u8Ret = Du8Safety_TestFail;                             //test result

  if (Fu32Crc == u32Stored)
    {
      u8Ret = Du8Safety_TestPass;
    }
#if (Du8SafetyCfg_FlashAllowUnpatched == 1U)
  else if (Du32Safety_CrcUnpatched == u32Stored)
    {
      u8Ret = Du8Safety_TestPass;  //development image, CRC not patched
    }
#endif

  return u8Ret;
}
