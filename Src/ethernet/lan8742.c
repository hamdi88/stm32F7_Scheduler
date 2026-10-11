/*
 * lan8742.c
 *
 *  Created on: Jun 8, 2026
 *      Author: hosni
 */


/* Includes ------------------------------------------------------------------*/

#include "lan8742.h"
#include "ethernet_internal.h"

/* Private Macros ------------------------------------------------------------*/

/* Private Types -------------------------------------------------------------*/


/* Private Variables ---------------------------------------------------------*/

static TstEth_Config tstLan8742_Config;

/* Private Function Prototypes -----------------------------------------------*/


/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: check PHY ID, soft reset, then start auto-neg or force mode
//inputs: Fpst_Config -> driver config (u8AutoNeg, enSpeed, enDuplex)
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_Init(const TstEth_Config *Fpst_Config)
{
  int16_t s16Ret;
  uint16_t u16Id1 = 0;
  uint16_t u16Id2 = 0;
  uint16_t u16Bcr = 0;
  uint32_t u32Elapsed;

  tstLan8742_Config = *Fpst_Config;

  s16Ret = s16Eth_PhyRead(Du8Lan8742_RegPHYID1, &u16Id1);
  if (ETH_OK == s16Ret)
    {
      s16Ret = s16Eth_PhyRead(Du8Lan8742_RegPHYID2, &u16Id2);
    }
  if ((ETH_OK == s16Ret) &&
      ((u16Id1 != Du16Lan8742_Id1) || ((u16Id2 & Du16Lan8742_Id2Mask) != Du16Lan8742_Id2)))
    {
      s16Ret = ETH_ERR_PHY;
    }

  //software reset, the bit self clears when done
  if (ETH_OK == s16Ret)
    {
      s16Ret = s16Eth_PhyWrite(Du8Lan8742_RegBCR, Du16Lan8742_BcrSoftReset);
    }
  u32Elapsed = 0;
  u16Bcr = Du16Lan8742_BcrSoftReset;
  while ((ETH_OK == s16Ret) && (u16Bcr & Du16Lan8742_BcrSoftReset))
    {
      vEth_DelayUs(1000);
      u32Elapsed++;
      s16Ret = s16Eth_PhyRead(Du8Lan8742_RegBCR, &u16Bcr);
      if ((ETH_OK == s16Ret) && (u32Elapsed >= Du32Lan8742_ResetTimeoutMs))
        {
          s16Ret = ETH_ERR_TIMEOUT;
        }
    }

  //mask all PHY interrupts (link is polled)
  if (ETH_OK == s16Ret)
    {
      s16Ret = s16Eth_PhyWrite(Du8Lan8742_RegIMR, 0U);
    }

  if (ETH_OK == s16Ret)
    {
      if (tstLan8742_Config.u8AutoNeg)
        {
          s16Ret = s16Eth_PhyWrite(Du8Lan8742_RegANAR, Du16Lan8742_AnarAll);
          if (ETH_OK == s16Ret)
            {
              s16Ret = s16Eth_PhyWrite(Du8Lan8742_RegBCR, Du16Lan8742_BcrAutoNegEn | Du16Lan8742_BcrRestartAN);
            }
        }
      else
        {
          u16Bcr = 0;
          if (ETH_SPEED_100M == tstLan8742_Config.enSpeed)
            {
              u16Bcr |= Du16Lan8742_BcrSpeed100;
            }
          if (ETH_DUPLEX_FULL == tstLan8742_Config.enDuplex)
            {
              u16Bcr |= Du16Lan8742_BcrFullDuplex;
            }
          s16Ret = s16Eth_PhyWrite(Du8Lan8742_RegBCR, u16Bcr);
        }
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: read current link state and resolved speed/duplex
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_GetLink(TenEth_LinkState *Fpen_State, TenEth_Speed *Fpen_Speed, TenEth_Duplex *Fpen_Duplex)
{
  int16_t s16Ret;
  uint16_t u16Bsr = 0;
  uint16_t u16Scsr = 0;

  *Fpen_State = ETH_LINK_DOWN;

  //link status bit is latched low: first read clears an old link fail event
  s16Ret = s16Eth_PhyRead(Du8Lan8742_RegBSR, &u16Bsr);
  if (ETH_OK == s16Ret)
    {
      s16Ret = s16Eth_PhyRead(Du8Lan8742_RegBSR, &u16Bsr);
    }

  if ((ETH_OK == s16Ret) && (u16Bsr & Du16Lan8742_BsrLinkUp))
    {
      if (tstLan8742_Config.u8AutoNeg)
        {
          s16Ret = s16Eth_PhyRead(Du8Lan8742_RegPHYSCSR, &u16Scsr);
          if ((ETH_OK == s16Ret) && (u16Scsr & Du16Lan8742_ScsrAutoDone))
            {
              switch (u16Scsr & Du16Lan8742_ScsrSpeedMask)
                {
                case Du16Lan8742_Scsr100Full:
                  *Fpen_Speed = ETH_SPEED_100M;
                  *Fpen_Duplex = ETH_DUPLEX_FULL;
                  break;
                case Du16Lan8742_Scsr100Half:
                  *Fpen_Speed = ETH_SPEED_100M;
                  *Fpen_Duplex = ETH_DUPLEX_HALF;
                  break;
                case Du16Lan8742_Scsr10Full:
                  *Fpen_Speed = ETH_SPEED_10M;
                  *Fpen_Duplex = ETH_DUPLEX_FULL;
                  break;
                default:
                  *Fpen_Speed = ETH_SPEED_10M;
                  *Fpen_Duplex = ETH_DUPLEX_HALF;
                  break;
                }
              *Fpen_State = ETH_LINK_UP;
            }
          //else: link up but negotiation not finished yet, report down
        }
      else
        {
          *Fpen_Speed = tstLan8742_Config.enSpeed;
          *Fpen_Duplex = tstLan8742_Config.enDuplex;
          *Fpen_State = ETH_LINK_UP;
        }
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: enable / disable PHY internal loopback
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_SetLoopback(uint8_t Fu8Enable)
{
  int16_t s16Ret;
  uint16_t u16Bcr = 0;

  s16Ret = s16Eth_PhyRead(Du8Lan8742_RegBCR, &u16Bcr);
  if (ETH_OK == s16Ret)
    {
      //clear self-clearing bits so they are not re-triggered
      u16Bcr &= (uint16_t)~(Du16Lan8742_BcrSoftReset | Du16Lan8742_BcrRestartAN);
      if (Fu8Enable)
        {
          u16Bcr |= Du16Lan8742_BcrLoopback;
        }
      else
        {
          u16Bcr &= (uint16_t)~Du16Lan8742_BcrLoopback;
        }
      s16Ret = s16Eth_PhyWrite(Du8Lan8742_RegBCR, u16Bcr);
    }

  return s16Ret;
}

/******************************************************************************/
//function desc: enter PHY power down mode
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_PowerDown(void)
{
  int16_t s16Ret;
  uint16_t u16Bcr = 0;

  s16Ret = s16Eth_PhyRead(Du8Lan8742_RegBCR, &u16Bcr);
  if (ETH_OK == s16Ret)
    {
      u16Bcr &= (uint16_t)~(Du16Lan8742_BcrSoftReset | Du16Lan8742_BcrRestartAN | Du16Lan8742_BcrAutoNegEn);
      u16Bcr |= Du16Lan8742_BcrPowerDown;
      s16Ret = s16Eth_PhyWrite(Du8Lan8742_RegBCR, u16Bcr);
    }

  return s16Ret;
}

/* Private Function Definitions ----------------------------------------------*/
