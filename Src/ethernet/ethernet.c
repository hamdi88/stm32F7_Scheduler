/*
 * ethernet.c
 *
 *  Created on: Jun 8, 2026
 *      Author: hosni
 *
 *  STM32F7 Ethernet MAC + DMA driver, RMII, chained normal DMA descriptors.
 *  Every descriptor and buffer is 32 bytes aligned/padded so that D-Cache
 *  maintenance (when the D-Cache is enabled) never touches a shared line.
 */


/* Includes ------------------------------------------------------------------*/

#include <string.h>
#include "ethernet.h"
#include "ethernet_internal.h"
#include "lan8742.h"
#include "stm32f7xx_ll_bus.h"
#include "stm32f7xx_ll_gpio.h"

/* Private Macros ------------------------------------------------------------*/

#define Du32Eth_CacheLine         32U

//TX descriptor TDES0
#define Du32Eth_TxDesc_OWN        0x80000000UL
#define Du32Eth_TxDesc_IC         0x40000000UL  //interrupt on completion
#define Du32Eth_TxDesc_LS         0x20000000UL  //last segment
#define Du32Eth_TxDesc_FS         0x10000000UL  //first segment
#define Du32Eth_TxDesc_CIC_Full   0x00C00000UL  //IP header + payload checksum insertion
#define Du32Eth_TxDesc_TCH        0x00100000UL  //second address chained
#define Du32Eth_TxDesc_ES         0x00008000UL  //error summary
//TX descriptor TDES1
#define Du32Eth_TxDesc_TBS1       0x00001FFFUL

//RX descriptor RDES0
#define Du32Eth_RxDesc_OWN        0x80000000UL
#define Du32Eth_RxDesc_FL         0x3FFF0000UL  //frame length (includes CRC)
#define Du32Eth_RxDesc_FL_Pos     16U
#define Du32Eth_RxDesc_ES         0x00008000UL
#define Du32Eth_RxDesc_FS         0x00000200UL
#define Du32Eth_RxDesc_LS         0x00000100UL
//RX descriptor RDES1
#define Du32Eth_RxDesc_RCH        0x00004000UL
#define Du32Eth_RxDesc_RBS1       0x00001FFFUL

#define Du16Eth_CrcLen            4U
#define Du16Eth_HeaderLen         14U

//write-1-to-clear bits of DMASR
#define Du32Eth_DmaSrClearMask    0x0001E7FFUL

#define Du32Eth_SwResetTimeoutUs  10000UL
#define Du32Eth_MdioTimeoutUs     10000UL
#define Du32Eth_FlushTimeoutUs    10000UL
//errata ES0290: successive writes to some MAC/DMA registers need a delay
#define Du32Eth_RegWriteDelayUs   50UL

#define Du32Eth_IrqPriority       6U

/* Private Types -------------------------------------------------------------*/

typedef struct
{
  volatile uint32_t u32Status;  //TDES0 / RDES0
  volatile uint32_t u32Ctrl;    //TDES1 / RDES1
  volatile uint32_t u32Buf1;    //TDES2 / RDES2
  volatile uint32_t u32Next;    //TDES3 / RDES3 (chained mode)
  uint32_t au32Pad[4];          //pad to one cache line
} __attribute__((aligned(32))) TstEth_DmaDesc;

typedef enum
{
  ETH_STATE_UNINIT = 0,
  ETH_STATE_READY,     //initialized, MAC/DMA stopped
  ETH_STATE_STARTED
} TenEth_State;

/* Private Variables ---------------------------------------------------------*/

static TstEth_DmaDesc tstEth_RxDesc[Du16Eth_RxDescNb];
static TstEth_DmaDesc tstEth_TxDesc[Du16Eth_TxDescNb];
static uint8_t au8Eth_RxBuf[Du16Eth_RxDescNb][Du16Eth_BufferSize] __attribute__((aligned(32)));
static uint8_t au8Eth_TxBuf[Du16Eth_TxDescNb][Du16Eth_BufferSize] __attribute__((aligned(32)));

static uint16_t u16Eth_RxIdx;         //next RX descriptor to read
static uint16_t u16Eth_TxIdx;         //next TX descriptor to fill
static uint16_t u16Eth_TxReclaimIdx;  //oldest TX descriptor not reclaimed
static uint16_t u16Eth_TxPending;     //descriptors handed to DMA, not reclaimed

static TstEth_Config tstEth_Config;
static volatile TenEth_State enEth_State = ETH_STATE_UNINIT;
static TenEth_LinkState enEth_Link = ETH_LINK_DOWN;
static TenEth_Speed enEth_Speed = ETH_SPEED_100M;
static TenEth_Duplex enEth_Duplex = ETH_DUPLEX_FULL;
static volatile uint8_t u8Eth_FatalError;

static TstEth_Stats tstEth_Stats;
static volatile uint32_t u32Eth_RxBufUnavail;  //updated from IRQ
static volatile uint32_t u32Eth_FatalBusErr;   //updated from IRQ

static TpEth_RxCallback pEth_RxCallback;
static TpEth_TxDoneCallback pEth_TxDoneCallback;
static TpEth_LinkCallback pEth_LinkCallback;

/* Private Function Prototypes -----------------------------------------------*/

static void vEth_DwtInit(void);
static uint8_t u8Eth_TimedOut(uint32_t Fu32Start, uint32_t Fu32Us);
static void vEth_WriteReg(volatile uint32_t *Fpu32Reg, uint32_t Fu32Value);
static void vEth_CacheClean(const void *Fpv_Addr, uint32_t Fu32Size);
static void vEth_CacheInvalidate(const void *Fpv_Addr, uint32_t Fu32Size);
static void vEth_InitGpios(void);
static void vEth_DeInitGpios(void);
static void vEth_SetMdcClock(void);
static int16_t s16Eth_MdioWait(void);
static void vEth_MacConfig(void);
static void vEth_DmaConfig(void);
static void vEth_DescInit(void);
static void vEth_ApplyLink(TenEth_Speed FenSpeed, TenEth_Duplex FenDuplex);
static void vEth_HandleDmaStatus(uint32_t Fu32Sr);
static void vEth_TxReclaim(void);
static int16_t s16Eth_RxPeek(uint8_t **Fppu8Frame, uint16_t *Fpu16Len);
static void vEth_RxRelease(void);
static uint32_t u32Eth_Crc32(const uint8_t *Fpu8Data, uint32_t Fu32Len);
static void vEth_MacFromUid(uint8_t *Fpu8Mac);

/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: fill a config struct with the board defaults
//inputs: Fpst_Config -> struct to fill
//outputs:
/******************************************************************************/
void vEth_GetDefaultConfig(TstEth_Config *Fpst_Config)
{
  if (Fpst_Config != NULL)
    {
      memset(Fpst_Config, 0, sizeof(TstEth_Config));
      Fpst_Config->u8AutoNeg = 1U;
      Fpst_Config->enSpeed = ETH_SPEED_100M;
      Fpst_Config->enDuplex = ETH_DUPLEX_FULL;
      Fpst_Config->u8ChecksumOffload = 1U;
      Fpst_Config->u8UseIrq = 1U;
      Fpst_Config->u8PhyAddr = 0U;
    }
}

/******************************************************************************/
//function desc: init GPIOs, clocks, MAC, DMA, PHY and start the interface
//inputs: Fpst_Config -> configuration, NULL -> defaults
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Eth_Init(const TstEth_Config *Fpst_Config)
{
  int16_t s16Ret = ETH_OK;
  uint32_t u32Start;
  static const uint8_t au8ZeroMac[Du8Eth_MacAddrLen] = {0};

  if (Fpst_Config != NULL)
    {
      tstEth_Config = *Fpst_Config;
    }
  else
    {
      vEth_GetDefaultConfig(&tstEth_Config);
    }
  if (0 == memcmp(tstEth_Config.au8MacAddr, au8ZeroMac, Du8Eth_MacAddrLen))
    {
      vEth_MacFromUid(tstEth_Config.au8MacAddr);
    }

  //RM0385: HCLK must be at least 25 MHz for the Ethernet peripheral
  SystemCoreClockUpdate();
  if (SystemCoreClock < Du32Eth_MinHclkHz)
    {
      s16Ret = ETH_ERR_CLOCK;
    }

  if (ETH_OK == s16Ret)
    {
      vEth_DwtInit();
      vEth_InitGpios();

      //RMII selection must be done while the MAC is under reset
      LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);
      LL_AHB1_GRP1_ForceReset(LL_AHB1_GRP1_PERIPH_ETHMAC);
      SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
      (void)SYSCFG->PMC;
      LL_AHB1_GRP1_ReleaseReset(LL_AHB1_GRP1_PERIPH_ETHMAC);

      LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_ETHMAC);
      LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_ETHMACTX);
      LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_ETHMACRX);

      //software reset of MAC + DMA, completes only if the PHY REF_CLK runs
      ETH->DMABMR |= ETH_DMABMR_SR;
      u32Start = DWT->CYCCNT;
      while ((ETH->DMABMR & ETH_DMABMR_SR) && (ETH_OK == s16Ret))
        {
          if (u8Eth_TimedOut(u32Start, Du32Eth_SwResetTimeoutUs))
            {
              s16Ret = ETH_ERR_TIMEOUT;
            }
        }
    }

  if (ETH_OK == s16Ret)
    {
      vEth_SetMdcClock();
      s16Ret = s16Lan8742_Init(&tstEth_Config);
    }

  if (ETH_OK == s16Ret)
    {
      enEth_Link = ETH_LINK_DOWN;
      if (tstEth_Config.u8AutoNeg)
        {
          enEth_Speed = ETH_SPEED_100M;
          enEth_Duplex = ETH_DUPLEX_FULL;
        }
      else
        {
          enEth_Speed = tstEth_Config.enSpeed;
          enEth_Duplex = tstEth_Config.enDuplex;
        }

      vEth_MacConfig();
      vEth_DmaConfig();
      vEth_SetMacAddress(tstEth_Config.au8MacAddr);
      vEth_ResetStats();

      enEth_State = ETH_STATE_READY;
      s16Ret = s16Eth_Start();
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: stop the interface, power down the PHY, reset/clock-off the MAC
//inputs:
//outputs:
/******************************************************************************/
void vEth_DeInit(void)
{
  if (enEth_State != ETH_STATE_UNINIT)
    {
      (void)s16Eth_Stop();
      (void)s16Lan8742_PowerDown();
    }

  NVIC_DisableIRQ(ETH_IRQn);
  LL_AHB1_GRP1_ForceReset(LL_AHB1_GRP1_PERIPH_ETHMAC);
  LL_AHB1_GRP1_ReleaseReset(LL_AHB1_GRP1_PERIPH_ETHMAC);
  LL_AHB1_GRP1_DisableClock(LL_AHB1_GRP1_PERIPH_ETHMAC);
  LL_AHB1_GRP1_DisableClock(LL_AHB1_GRP1_PERIPH_ETHMACTX);
  LL_AHB1_GRP1_DisableClock(LL_AHB1_GRP1_PERIPH_ETHMACRX);
  vEth_DeInitGpios();

  enEth_Link = ETH_LINK_DOWN;
  enEth_State = ETH_STATE_UNINIT;
}

/******************************************************************************/
//function desc: start MAC + DMA (descriptor rings are re-initialized)
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Eth_Start(void)
{
  int16_t s16Ret = ETH_OK;
  uint32_t u32Start;

  if (ETH_STATE_READY != enEth_State)
    {
      s16Ret = ETH_ERR_STATE;
    }
  else
    {
      vEth_DescInit();
      u8Eth_FatalError = 0U;

      vEth_WriteReg(&ETH->MACCR, ETH->MACCR | ETH_MACCR_TE);
      vEth_WriteReg(&ETH->MACCR, ETH->MACCR | ETH_MACCR_RE);

      //flush transmit FIFO
      vEth_WriteReg(&ETH->DMAOMR, ETH->DMAOMR | ETH_DMAOMR_FTF);
      u32Start = DWT->CYCCNT;
      while ((ETH->DMAOMR & ETH_DMAOMR_FTF) && (ETH_OK == s16Ret))
        {
          if (u8Eth_TimedOut(u32Start, Du32Eth_FlushTimeoutUs))
            {
              s16Ret = ETH_ERR_TIMEOUT;
            }
        }

      vEth_WriteReg(&ETH->DMAOMR, ETH->DMAOMR | ETH_DMAOMR_ST);
      vEth_WriteReg(&ETH->DMAOMR, ETH->DMAOMR | ETH_DMAOMR_SR);

      ETH->DMASR = Du32Eth_DmaSrClearMask;
      if (tstEth_Config.u8UseIrq)
        {
          ETH->DMAIER = ETH_DMAIER_NISE | ETH_DMAIER_RIE | ETH_DMAIER_TIE |
                        ETH_DMAIER_AISE | ETH_DMAIER_FBEIE | ETH_DMAIER_RBUIE;
          NVIC_ClearPendingIRQ(ETH_IRQn);
          NVIC_SetPriority(ETH_IRQn, Du32Eth_IrqPriority);
          NVIC_EnableIRQ(ETH_IRQn);
        }
      else
        {
          ETH->DMAIER = 0U;
        }

      enEth_State = ETH_STATE_STARTED;
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: stop MAC + DMA, pending TX frames are dropped
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Eth_Stop(void)
{
  int16_t s16Ret = ETH_OK;
  uint32_t u32Start;

  if (ETH_STATE_STARTED != enEth_State)
    {
      s16Ret = ETH_ERR_STATE;
    }
  else
    {
      NVIC_DisableIRQ(ETH_IRQn);
      ETH->DMAIER = 0U;

      vEth_WriteReg(&ETH->DMAOMR, ETH->DMAOMR & ~ETH_DMAOMR_ST);
      vEth_WriteReg(&ETH->DMAOMR, ETH->DMAOMR & ~ETH_DMAOMR_SR);
      vEth_WriteReg(&ETH->MACCR, ETH->MACCR & ~ETH_MACCR_RE);

      vEth_WriteReg(&ETH->DMAOMR, ETH->DMAOMR | ETH_DMAOMR_FTF);
      u32Start = DWT->CYCCNT;
      while ((ETH->DMAOMR & ETH_DMAOMR_FTF) && (ETH_OK == s16Ret))
        {
          if (u8Eth_TimedOut(u32Start, Du32Eth_FlushTimeoutUs))
            {
              s16Ret = ETH_ERR_TIMEOUT;
            }
        }

      vEth_WriteReg(&ETH->MACCR, ETH->MACCR & ~ETH_MACCR_TE);
      ETH->DMASR = Du32Eth_DmaSrClearMask;

      enEth_State = ETH_STATE_READY;
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: queue one frame for transmission (frame is copied)
//inputs: Fpu8Frame -> DA+SA+Type+payload (no CRC), Fu16Len -> 14..1514 bytes
//outputs: ETH_OK, ETH_ERR_BUSY (ring full), ETH_ERR
/******************************************************************************/
int16_t s16Eth_Transmit(const uint8_t *Fpu8Frame, uint16_t Fu16Len)
{
  int16_t s16Ret = ETH_OK;
  TstEth_DmaDesc *pstDesc;
  uint32_t u32Status;

  if (ETH_STATE_STARTED != enEth_State)
    {
      s16Ret = ETH_ERR_STATE;
    }
  else if ((NULL == Fpu8Frame) || (Fu16Len < Du16Eth_HeaderLen) || (Fu16Len > Du16Eth_MaxPayloadFrameLen))
    {
      s16Ret = ETH_ERR;
    }
  else
    {
      vEth_TxReclaim();

      if (u16Eth_TxPending >= Du16Eth_TxDescNb)
        {
          tstEth_Stats.u32TxBusy++;
          s16Ret = ETH_ERR_BUSY;
        }
      else
        {
          pstDesc = &tstEth_TxDesc[u16Eth_TxIdx];

          memcpy(au8Eth_TxBuf[u16Eth_TxIdx], Fpu8Frame, Fu16Len);
          vEth_CacheClean(au8Eth_TxBuf[u16Eth_TxIdx], Fu16Len);

          u32Status = Du32Eth_TxDesc_TCH | Du32Eth_TxDesc_FS | Du32Eth_TxDesc_LS | Du32Eth_TxDesc_IC;
          if (tstEth_Config.u8ChecksumOffload)
            {
              u32Status |= Du32Eth_TxDesc_CIC_Full;
            }
          pstDesc->u32Ctrl = (uint32_t)Fu16Len & Du32Eth_TxDesc_TBS1;
          __DMB();
          pstDesc->u32Status = u32Status | Du32Eth_TxDesc_OWN;
          vEth_CacheClean(pstDesc, sizeof(TstEth_DmaDesc));
          __DSB();

          u16Eth_TxIdx = (uint16_t)((u16Eth_TxIdx + 1U) % Du16Eth_TxDescNb);
          u16Eth_TxPending++;

          //resume DMA if it suspended on an empty ring
          if (ETH->DMASR & ETH_DMASR_TBUS)
            {
              ETH->DMASR = ETH_DMASR_TBUS;
            }
          ETH->DMATPDR = 0U;
        }
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: poll for one received frame (copied into user buffer, no CRC)
//inputs: Fpu8Buf -> destination, Fu16BufSize -> its size, Fpu16Len -> frame len
//outputs: ETH_OK, ETH_ERR_NO_DATA, ETH_ERR_FRAME, ETH_ERR
/******************************************************************************/
int16_t s16Eth_Receive(uint8_t *Fpu8Buf, uint16_t Fu16BufSize, uint16_t *Fpu16Len)
{
  int16_t s16Ret;
  uint8_t *pu8Frame = NULL;
  uint16_t u16Len = 0;

  if (ETH_STATE_STARTED != enEth_State)
    {
      s16Ret = ETH_ERR_STATE;
    }
  else if ((NULL == Fpu8Buf) || (NULL == Fpu16Len))
    {
      s16Ret = ETH_ERR;
    }
  else
    {
      s16Ret = s16Eth_RxPeek(&pu8Frame, &u16Len);
      if (ETH_OK == s16Ret)
        {
          if (u16Len <= Fu16BufSize)
            {
              memcpy(Fpu8Buf, pu8Frame, u16Len);
              *Fpu16Len = u16Len;
              tstEth_Stats.u32RxFrames++;
            }
          else
            {
              tstEth_Stats.u32RxDropped++;
              s16Ret = ETH_ERR_FRAME;
            }
          vEth_RxRelease();
        }
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: scheduler task (1ms): RX delivery, TX reclaim, error recovery
//inputs:
//outputs:
/******************************************************************************/
void vEth_Process(void)
{
  uint16_t u16Count;
  int16_t s16Ret;
  uint8_t *pu8Frame = NULL;
  uint16_t u16Len = 0;

  if (ETH_STATE_STARTED == enEth_State)
    {
      if (!tstEth_Config.u8UseIrq)
        {
          vEth_HandleDmaStatus(ETH->DMASR);
        }

      //fatal bus error stops the DMA: restart the whole datapath
      if (u8Eth_FatalError)
        {
          (void)s16Eth_Stop();
          (void)s16Eth_Start();
        }

      vEth_TxReclaim();

      if (pEth_RxCallback != NULL)
        {
          //bounded loop so one call never takes more than a ring worth of frames
          for (u16Count = 0; u16Count < Du16Eth_RxDescNb; u16Count++)
            {
              s16Ret = s16Eth_RxPeek(&pu8Frame, &u16Len);
              if (ETH_ERR_NO_DATA == s16Ret)
                {
                  break;
                }
              if (ETH_OK == s16Ret)
                {
                  tstEth_Stats.u32RxFrames++;
                  pEth_RxCallback(pu8Frame, u16Len);
                  vEth_RxRelease();
                }
              //ETH_ERR_FRAME: descriptor already released by peek
            }
        }
    }
}

/******************************************************************************/
//function desc: scheduler task (~100ms): PHY link monitoring
//inputs:
//outputs:
/******************************************************************************/
void vEth_LinkTask(void)
{
  TenEth_LinkState enState = ETH_LINK_DOWN;
  TenEth_Speed enSpeed = enEth_Speed;
  TenEth_Duplex enDuplex = enEth_Duplex;

  if ((ETH_STATE_UNINIT != enEth_State) &&
      (ETH_OK == s16Lan8742_GetLink(&enState, &enSpeed, &enDuplex)))
    {
      if ((enState != enEth_Link) ||
          ((ETH_LINK_UP == enState) && ((enSpeed != enEth_Speed) || (enDuplex != enEth_Duplex))))
        {
          if (ETH_LINK_UP == enState)
            {
              vEth_ApplyLink(enSpeed, enDuplex);
              enEth_Speed = enSpeed;
              enEth_Duplex = enDuplex;
            }
          enEth_Link = enState;

          if (pEth_LinkCallback != NULL)
            {
              pEth_LinkCallback(enEth_Link, enEth_Speed, enEth_Duplex);
            }
        }
    }
}

/******************************************************************************/
//function desc: callbacks registration (NULL to unregister)
//inputs:
//outputs:
/******************************************************************************/
void vEth_RegisterRxCallback(TpEth_RxCallback Fp_Callback)
{
  pEth_RxCallback = Fp_Callback;
}

void vEth_RegisterTxDoneCallback(TpEth_TxDoneCallback Fp_Callback)
{
  pEth_TxDoneCallback = Fp_Callback;
}

void vEth_RegisterLinkCallback(TpEth_LinkCallback Fp_Callback)
{
  pEth_LinkCallback = Fp_Callback;
}

/******************************************************************************/
//function desc: link information
//inputs:
//outputs:
/******************************************************************************/
TenEth_LinkState enEth_GetLinkState(void)
{
  return enEth_Link;
}

TenEth_Speed enEth_GetSpeed(void)
{
  return enEth_Speed;
}

TenEth_Duplex enEth_GetDuplex(void)
{
  return enEth_Duplex;
}

/******************************************************************************/
//function desc: MAC address get / set
//inputs: Fpu8Mac -> 6 bytes
//outputs:
/******************************************************************************/
void vEth_SetMacAddress(const uint8_t *Fpu8Mac)
{
  if (Fpu8Mac != NULL)
    {
      memcpy(tstEth_Config.au8MacAddr, Fpu8Mac, Du8Eth_MacAddrLen);
      ETH->MACA0HR = ((uint32_t)Fpu8Mac[5] << 8) | (uint32_t)Fpu8Mac[4];
      ETH->MACA0LR = ((uint32_t)Fpu8Mac[3] << 24) | ((uint32_t)Fpu8Mac[2] << 16) |
                     ((uint32_t)Fpu8Mac[1] << 8) | (uint32_t)Fpu8Mac[0];
    }
}

void vEth_GetMacAddress(uint8_t *Fpu8Mac)
{
  if (Fpu8Mac != NULL)
    {
      memcpy(Fpu8Mac, tstEth_Config.au8MacAddr, Du8Eth_MacAddrLen);
    }
}

/******************************************************************************/
//function desc: address filtering
//inputs:
//outputs:
/******************************************************************************/
void vEth_SetPromiscuous(uint8_t Fu8Enable)
{
  if (Fu8Enable)
    {
      vEth_WriteReg(&ETH->MACFFR, ETH->MACFFR | ETH_MACFFR_PM);
    }
  else
    {
      vEth_WriteReg(&ETH->MACFFR, ETH->MACFFR & ~ETH_MACFFR_PM);
    }
}

void vEth_SetAllMulticast(uint8_t Fu8Enable)
{
  if (Fu8Enable)
    {
      vEth_WriteReg(&ETH->MACFFR, ETH->MACFFR | ETH_MACFFR_PAM);
    }
  else
    {
      vEth_WriteReg(&ETH->MACFFR, ETH->MACFFR & ~ETH_MACFFR_PAM);
    }
}

int16_t s16Eth_SetMulticastList(const uint8_t (*Fpau8List)[Du8Eth_MacAddrLen], uint16_t Fu16Count)
{
  int16_t s16Ret = ETH_OK;
  uint32_t au32Hash[2] = {0U, 0U};
  uint32_t u32Bit;
  uint16_t u16Index;

  if ((NULL == Fpau8List) && (Fu16Count > 0U))
    {
      s16Ret = ETH_ERR;
    }
  else
    {
      //hash index = 6 MSBs of the bit-reversed IEEE CRC32 of the address
      for (u16Index = 0; u16Index < Fu16Count; u16Index++)
        {
          u32Bit = __RBIT(u32Eth_Crc32(Fpau8List[u16Index], Du8Eth_MacAddrLen)) >> 26;
          au32Hash[u32Bit >> 5] |= (1UL << (u32Bit & 0x1FU));
        }
      ETH->MACHTLR = au32Hash[0];
      ETH->MACHTHR = au32Hash[1];

      if (Fu16Count > 0U)
        {
          vEth_WriteReg(&ETH->MACFFR, ETH->MACFFR | ETH_MACFFR_HM);
        }
      else
        {
          vEth_WriteReg(&ETH->MACFFR, ETH->MACFFR & ~ETH_MACFFR_HM);
        }
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: raw PHY register access through MDIO
//inputs: Fu8Reg -> 0..31
//outputs: ETH_OK or ETH_ERR_TIMEOUT
/******************************************************************************/
int16_t s16Eth_PhyRead(uint8_t Fu8Reg, uint16_t *Fpu16Value)
{
  int16_t s16Ret;
  uint32_t u32Reg;

  if (NULL == Fpu16Value)
    {
      return ETH_ERR;
    }

  s16Ret = s16Eth_MdioWait();
  if (ETH_OK == s16Ret)
    {
      u32Reg = ETH->MACMIIAR & ETH_MACMIIAR_CR;  //keep MDC clock range
      u32Reg |= ((uint32_t)tstEth_Config.u8PhyAddr << ETH_MACMIIAR_PA_Pos) & ETH_MACMIIAR_PA;
      u32Reg |= ((uint32_t)Fu8Reg << ETH_MACMIIAR_MR_Pos) & ETH_MACMIIAR_MR;
      u32Reg |= ETH_MACMIIAR_MB;                   //MW = 0 -> read
      ETH->MACMIIAR = u32Reg;
      s16Ret = s16Eth_MdioWait();
    }
  if (ETH_OK == s16Ret)
    {
      *Fpu16Value = (uint16_t)ETH->MACMIIDR;
    }

  return s16Ret;
}

int16_t s16Eth_PhyWrite(uint8_t Fu8Reg, uint16_t Fu16Value)
{
  int16_t s16Ret;
  uint32_t u32Reg;

  s16Ret = s16Eth_MdioWait();
  if (ETH_OK == s16Ret)
    {
      ETH->MACMIIDR = Fu16Value;
      u32Reg = ETH->MACMIIAR & ETH_MACMIIAR_CR;
      u32Reg |= ((uint32_t)tstEth_Config.u8PhyAddr << ETH_MACMIIAR_PA_Pos) & ETH_MACMIIAR_PA;
      u32Reg |= ((uint32_t)Fu8Reg << ETH_MACMIIAR_MR_Pos) & ETH_MACMIIAR_MR;
      u32Reg |= ETH_MACMIIAR_MW | ETH_MACMIIAR_MB;
      ETH->MACMIIAR = u32Reg;
      s16Ret = s16Eth_MdioWait();
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: PHY loopback
//inputs: Fu8Enable -> 1 on, 0 off
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Eth_SetPhyLoopback(uint8_t Fu8Enable)
{
  int16_t s16Ret = ETH_ERR_STATE;

  if (ETH_STATE_UNINIT != enEth_State)
    {
      s16Ret = s16Lan8742_SetLoopback(Fu8Enable);
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: statistics
//inputs:
//outputs:
/******************************************************************************/
void vEth_GetStats(TstEth_Stats *Fpst_Stats)
{
  if (Fpst_Stats != NULL)
    {
      *Fpst_Stats = tstEth_Stats;
      Fpst_Stats->u32RxBufUnavail = u32Eth_RxBufUnavail;
      Fpst_Stats->u32FatalBusErr = u32Eth_FatalBusErr;
      if (ETH_STATE_UNINIT != enEth_State)
        {
          Fpst_Stats->u32MmcTxGood = ETH->MMCTGFCR;
          Fpst_Stats->u32MmcTxSingleCol = ETH->MMCTGFSCCR;
          Fpst_Stats->u32MmcTxMultiCol = ETH->MMCTGFMSCCR;
          Fpst_Stats->u32MmcRxCrcErr = ETH->MMCRFCECR;
          Fpst_Stats->u32MmcRxAlignErr = ETH->MMCRFAECR;
          Fpst_Stats->u32MmcRxGoodUnicast = ETH->MMCRGUFCR;
        }
    }
}

void vEth_ResetStats(void)
{
  memset(&tstEth_Stats, 0, sizeof(tstEth_Stats));
  u32Eth_RxBufUnavail = 0U;
  u32Eth_FatalBusErr = 0U;
  ETH->MMCCR |= ETH_MMCCR_CR;  //self clearing counters reset
}

/******************************************************************************/
//function desc: busy wait based on the DWT cycle counter
//inputs: Fu32Us -> delay in microseconds
//outputs:
/******************************************************************************/
void vEth_DelayUs(uint32_t Fu32Us)
{
  uint32_t u32Start = DWT->CYCCNT;

  while (!u8Eth_TimedOut(u32Start, Fu32Us))
    {
    }
}

/******************************************************************************/
//function desc: Ethernet global interrupt (DMA events)
//inputs:
//outputs:
/******************************************************************************/
void ETH_IRQHandler(void)
{
  vEth_HandleDmaStatus(ETH->DMASR);
}

/* Private Function Definitions ----------------------------------------------*/

static void vEth_DwtInit(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55UL;  //unlock DWT on Cortex-M7
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint8_t u8Eth_TimedOut(uint32_t Fu32Start, uint32_t Fu32Us)
{
  uint32_t u32Cycles = Fu32Us * (SystemCoreClock / 1000000UL);

  return ((DWT->CYCCNT - Fu32Start) >= u32Cycles) ? 1U : 0U;
}

static void vEth_WriteReg(volatile uint32_t *Fpu32Reg, uint32_t Fu32Value)
{
  *Fpu32Reg = Fu32Value;
  Fu32Value = *Fpu32Reg;
  vEth_DelayUs(Du32Eth_RegWriteDelayUs);
  *Fpu32Reg = Fu32Value;
}

static void vEth_CacheClean(const void *Fpv_Addr, uint32_t Fu32Size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
  uint32_t u32Addr;
  if (SCB->CCR & SCB_CCR_DC_Msk)
    {
      u32Addr = (uint32_t)Fpv_Addr & ~(Du32Eth_CacheLine - 1U);
      Fu32Size += (uint32_t)Fpv_Addr - u32Addr;
      SCB_CleanDCache_by_Addr((uint32_t *)u32Addr, (int32_t)Fu32Size);
    }
#else
  (void)Fpv_Addr;
  (void)Fu32Size;
#endif
}

static void vEth_CacheInvalidate(const void *Fpv_Addr, uint32_t Fu32Size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
  uint32_t u32Addr;
  if (SCB->CCR & SCB_CCR_DC_Msk)
    {
      u32Addr = (uint32_t)Fpv_Addr & ~(Du32Eth_CacheLine - 1U);
      Fu32Size += (uint32_t)Fpv_Addr - u32Addr;
      SCB_InvalidateDCache_by_Addr((uint32_t *)u32Addr, (int32_t)Fu32Size);
    }
#else
  (void)Fpv_Addr;
  (void)Fu32Size;
#endif
}

static void vEth_InitGpios(void)
{
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOC);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOG);

  GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
  GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
  GPIO_InitStruct.Alternate = LL_GPIO_AF_11;

  //REF_CLK (PA1), MDIO (PA2), CRS_DV (PA7)
  GPIO_InitStruct.Pin = LL_GPIO_PIN_1 | LL_GPIO_PIN_2 | LL_GPIO_PIN_7;
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  //MDC (PC1), RXD0 (PC4), RXD1 (PC5)
  GPIO_InitStruct.Pin = LL_GPIO_PIN_1 | LL_GPIO_PIN_4 | LL_GPIO_PIN_5;
  LL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  //TX_EN (PG11), TXD0 (PG13), TXD1 (PG14)
  GPIO_InitStruct.Pin = LL_GPIO_PIN_11 | LL_GPIO_PIN_13 | LL_GPIO_PIN_14;
  LL_GPIO_Init(GPIOG, &GPIO_InitStruct);
}

static void vEth_DeInitGpios(void)
{
  LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Mode = LL_GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;

  GPIO_InitStruct.Pin = LL_GPIO_PIN_1 | LL_GPIO_PIN_2 | LL_GPIO_PIN_7;
  LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = LL_GPIO_PIN_1 | LL_GPIO_PIN_4 | LL_GPIO_PIN_5;
  LL_GPIO_Init(GPIOC, &GPIO_InitStruct);
  GPIO_InitStruct.Pin = LL_GPIO_PIN_11 | LL_GPIO_PIN_13 | LL_GPIO_PIN_14;
  LL_GPIO_Init(GPIOG, &GPIO_InitStruct);
}

//MDC must stay <= 2.5 MHz: pick the HCLK divider from the current HCLK
static void vEth_SetMdcClock(void)
{
  uint32_t u32Cr;
  uint32_t u32Hclk = SystemCoreClock;

  if (u32Hclk < 35000000UL)
    {
      u32Cr = 2U;   //HCLK/16
    }
  else if (u32Hclk < 60000000UL)
    {
      u32Cr = 3U;   //HCLK/26
    }
  else if (u32Hclk < 100000000UL)
    {
      u32Cr = 0U;   //HCLK/42
    }
  else if (u32Hclk < 150000000UL)
    {
      u32Cr = 1U;   //HCLK/62
    }
  else
    {
      u32Cr = 4U;   //HCLK/102
    }

  ETH->MACMIIAR = (ETH->MACMIIAR & ~ETH_MACMIIAR_CR) | (u32Cr << ETH_MACMIIAR_CR_Pos);
}

static int16_t s16Eth_MdioWait(void)
{
  int16_t s16Ret = ETH_OK;
  uint32_t u32Start = DWT->CYCCNT;

  while ((ETH->MACMIIAR & ETH_MACMIIAR_MB) && (ETH_OK == s16Ret))
    {
      if (u8Eth_TimedOut(u32Start, Du32Eth_MdioTimeoutUs))
        {
          s16Ret = ETH_ERR_TIMEOUT;
        }
    }

  return s16Ret;
}

static void vEth_MacConfig(void)
{
  uint32_t u32Maccr = 0U;

  if (tstEth_Config.u8ChecksumOffload)
    {
      u32Maccr |= ETH_MACCR_IPCO;
    }
  if (ETH_SPEED_100M == enEth_Speed)
    {
      u32Maccr |= ETH_MACCR_FES;
    }
  if (ETH_DUPLEX_FULL == enEth_Duplex)
    {
      u32Maccr |= ETH_MACCR_DM;
    }
  vEth_WriteReg(&ETH->MACCR, u32Maccr);

  //perfect DA filtering, broadcast accepted, hash table empty
  vEth_WriteReg(&ETH->MACFFR, 0U);
  ETH->MACHTHR = 0U;
  ETH->MACHTLR = 0U;

  //no flow control
  vEth_WriteReg(&ETH->MACFCR, 0U);
  ETH->MACVLANTR = 0U;

  //mask MAC (timestamp, PMT) and MMC interrupts, they are not handled
  ETH->MACIMR = ETH_MACIMR_TSTIM | ETH_MACIMR_PMTIM;
  ETH->MMCRIMR = ETH_MMCRIMR_RGUFM | ETH_MMCRIMR_RFAEM | ETH_MMCRIMR_RFCEM;
  ETH->MMCTIMR = ETH_MMCTIMR_TGFM | ETH_MMCTIMR_TGFMSCM | ETH_MMCTIMR_TGFSCM;
}

static void vEth_DmaConfig(void)
{
  //store and forward on both paths (needed for TX checksum insertion),
  //operate on second frame
  vEth_WriteReg(&ETH->DMAOMR, ETH_DMAOMR_RSF | ETH_DMAOMR_TSF | ETH_DMAOMR_OSF);

  //address aligned beats, fixed burst, 32 beats RX/TX, normal descriptors
  vEth_WriteReg(&ETH->DMABMR, ETH_DMABMR_AAB | ETH_DMABMR_FB | ETH_DMABMR_USP |
                              (32UL << ETH_DMABMR_RDP_Pos) | (32UL << ETH_DMABMR_PBL_Pos));
  ETH->DMAIER = 0U;
}

static void vEth_DescInit(void)
{
  uint16_t u16Index;

  for (u16Index = 0; u16Index < Du16Eth_RxDescNb; u16Index++)
    {
      tstEth_RxDesc[u16Index].u32Ctrl = Du32Eth_RxDesc_RCH | (Du16Eth_BufferSize & Du32Eth_RxDesc_RBS1);
      tstEth_RxDesc[u16Index].u32Buf1 = (uint32_t)au8Eth_RxBuf[u16Index];
      tstEth_RxDesc[u16Index].u32Next = (uint32_t)&tstEth_RxDesc[(u16Index + 1U) % Du16Eth_RxDescNb];
      tstEth_RxDesc[u16Index].u32Status = Du32Eth_RxDesc_OWN;
    }

  for (u16Index = 0; u16Index < Du16Eth_TxDescNb; u16Index++)
    {
      tstEth_TxDesc[u16Index].u32Ctrl = 0U;
      tstEth_TxDesc[u16Index].u32Buf1 = (uint32_t)au8Eth_TxBuf[u16Index];
      tstEth_TxDesc[u16Index].u32Next = (uint32_t)&tstEth_TxDesc[(u16Index + 1U) % Du16Eth_TxDescNb];
      tstEth_TxDesc[u16Index].u32Status = Du32Eth_TxDesc_TCH;
    }

  vEth_CacheClean(tstEth_RxDesc, sizeof(tstEth_RxDesc));
  vEth_CacheClean(tstEth_TxDesc, sizeof(tstEth_TxDesc));
  vEth_CacheInvalidate(au8Eth_RxBuf, sizeof(au8Eth_RxBuf));
  __DSB();

  u16Eth_RxIdx = 0U;
  u16Eth_TxIdx = 0U;
  u16Eth_TxReclaimIdx = 0U;
  u16Eth_TxPending = 0U;

  ETH->DMARDLAR = (uint32_t)tstEth_RxDesc;
  ETH->DMATDLAR = (uint32_t)tstEth_TxDesc;
}

static void vEth_ApplyLink(TenEth_Speed FenSpeed, TenEth_Duplex FenDuplex)
{
  uint32_t u32Maccr = ETH->MACCR & ~(ETH_MACCR_FES | ETH_MACCR_DM);

  if (ETH_SPEED_100M == FenSpeed)
    {
      u32Maccr |= ETH_MACCR_FES;
    }
  if (ETH_DUPLEX_FULL == FenDuplex)
    {
      u32Maccr |= ETH_MACCR_DM;
    }
  vEth_WriteReg(&ETH->MACCR, u32Maccr);
}

//shared by the IRQ handler and the polling mode
static void vEth_HandleDmaStatus(uint32_t Fu32Sr)
{
  if (Fu32Sr & ETH_DMASR_RBUS)
    {
      u32Eth_RxBufUnavail++;
    }
  if (Fu32Sr & ETH_DMASR_FBES)
    {
      u32Eth_FatalBusErr++;
      u8Eth_FatalError = 1U;
    }
  //RX/TX completion only wakes the CPU from WFI, work is done in vEth_Process
  ETH->DMASR = Fu32Sr & Du32Eth_DmaSrClearMask;
}

static void vEth_TxReclaim(void)
{
  TstEth_DmaDesc *pstDesc;
  uint8_t u8Reclaimed = 0U;

  while (u16Eth_TxPending > 0U)
    {
      pstDesc = &tstEth_TxDesc[u16Eth_TxReclaimIdx];
      vEth_CacheInvalidate(pstDesc, sizeof(TstEth_DmaDesc));
      if (pstDesc->u32Status & Du32Eth_TxDesc_OWN)
        {
          break;
        }
      if (pstDesc->u32Status & Du32Eth_TxDesc_ES)
        {
          tstEth_Stats.u32TxErrors++;
        }
      else
        {
          tstEth_Stats.u32TxFrames++;
        }
      u16Eth_TxReclaimIdx = (uint16_t)((u16Eth_TxReclaimIdx + 1U) % Du16Eth_TxDescNb);
      u16Eth_TxPending--;
      u8Reclaimed = 1U;
    }

  if (u8Reclaimed && (pEth_TxDoneCallback != NULL))
    {
      pEth_TxDoneCallback();
    }
}

//returns a pointer into the DMA buffer, must be followed by vEth_RxRelease
//on ETH_OK; errored frames are released here
static int16_t s16Eth_RxPeek(uint8_t **Fppu8Frame, uint16_t *Fpu16Len)
{
  int16_t s16Ret = ETH_OK;
  TstEth_DmaDesc *pstDesc = &tstEth_RxDesc[u16Eth_RxIdx];
  uint32_t u32Status;
  uint32_t u32Len;

  vEth_CacheInvalidate(pstDesc, sizeof(TstEth_DmaDesc));
  u32Status = pstDesc->u32Status;

  if (u32Status & Du32Eth_RxDesc_OWN)
    {
      s16Ret = ETH_ERR_NO_DATA;
    }
  else
    {
      u32Len = (u32Status & Du32Eth_RxDesc_FL) >> Du32Eth_RxDesc_FL_Pos;
      if ((u32Status & Du32Eth_RxDesc_ES) ||
          ((u32Status & (Du32Eth_RxDesc_FS | Du32Eth_RxDesc_LS)) != (Du32Eth_RxDesc_FS | Du32Eth_RxDesc_LS)) ||
          (u32Len < (Du16Eth_HeaderLen + Du16Eth_CrcLen)))
        {
          tstEth_Stats.u32RxErrors++;
          vEth_RxRelease();
          s16Ret = ETH_ERR_FRAME;
        }
      else
        {
          u32Len -= Du16Eth_CrcLen;
          vEth_CacheInvalidate(au8Eth_RxBuf[u16Eth_RxIdx], u32Len);
          *Fppu8Frame = au8Eth_RxBuf[u16Eth_RxIdx];
          *Fpu16Len = (uint16_t)u32Len;
        }
    }

  return s16Ret;
}

static void vEth_RxRelease(void)
{
  TstEth_DmaDesc *pstDesc = &tstEth_RxDesc[u16Eth_RxIdx];

  pstDesc->u32Status = Du32Eth_RxDesc_OWN;
  vEth_CacheClean(pstDesc, sizeof(TstEth_DmaDesc));
  __DSB();

  u16Eth_RxIdx = (uint16_t)((u16Eth_RxIdx + 1U) % Du16Eth_RxDescNb);

  //resume RX DMA if it suspended because the ring was full
  if (ETH->DMASR & ETH_DMASR_RBUS)
    {
      ETH->DMASR = ETH_DMASR_RBUS;
    }
  ETH->DMARPDR = 0U;
}

//IEEE 802.3 CRC32 (reflected, poly 0xEDB88320, final complement)
static uint32_t u32Eth_Crc32(const uint8_t *Fpu8Data, uint32_t Fu32Len)
{
  uint32_t u32Crc = 0xFFFFFFFFUL;
  uint32_t u32Index;
  uint8_t u8Bit;

  for (u32Index = 0; u32Index < Fu32Len; u32Index++)
    {
      u32Crc ^= Fpu8Data[u32Index];
      for (u8Bit = 0; u8Bit < 8U; u8Bit++)
        {
          u32Crc = (u32Crc >> 1) ^ (0xEDB88320UL & (0UL - (u32Crc & 1UL)));
        }
    }

  return ~u32Crc;
}

//locally administered unicast address built from the 96-bit device UID
static void vEth_MacFromUid(uint8_t *Fpu8Mac)
{
  const uint8_t *pu8Uid = (const uint8_t *)UID_BASE;
  uint32_t u32Hash = u32Eth_Crc32(pu8Uid, 12U);

  Fpu8Mac[0] = 0x02U;
  Fpu8Mac[1] = pu8Uid[0] ^ pu8Uid[4] ^ pu8Uid[8];
  Fpu8Mac[2] = (uint8_t)(u32Hash >> 24);
  Fpu8Mac[3] = (uint8_t)(u32Hash >> 16);
  Fpu8Mac[4] = (uint8_t)(u32Hash >> 8);
  Fpu8Mac[5] = (uint8_t)u32Hash;
}
