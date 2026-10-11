/*
 * lan8742.h
 *
 *  Created on: Jun 8, 2026
 *      Author: hosni
 *
 *  Microchip LAN8742A PHY driver (used internally by ethernet.c)
 */

#ifndef ETHERNET_LAN8742_H_
#define ETHERNET_LAN8742_H_


/* Includes ------------------------------------------------------------------*/

#include "ethernet.h"

/* Public Macros -------------------------------------------------------------*/

//IEEE 802.3 registers
#define Du8Lan8742_RegBCR         0U    //basic control
#define Du8Lan8742_RegBSR         1U    //basic status
#define Du8Lan8742_RegPHYID1      2U
#define Du8Lan8742_RegPHYID2      3U
#define Du8Lan8742_RegANAR        4U    //auto-neg advertisement
#define Du8Lan8742_RegANLPAR      5U    //auto-neg link partner ability
//vendor specific registers
#define Du8Lan8742_RegSMR         18U   //special modes
#define Du8Lan8742_RegISFR        29U   //interrupt source flags
#define Du8Lan8742_RegIMR         30U   //interrupt mask
#define Du8Lan8742_RegPHYSCSR     31U   //PHY special control/status

//BCR bits
#define Du16Lan8742_BcrSoftReset  0x8000U
#define Du16Lan8742_BcrLoopback   0x4000U
#define Du16Lan8742_BcrSpeed100   0x2000U
#define Du16Lan8742_BcrAutoNegEn  0x1000U
#define Du16Lan8742_BcrPowerDown  0x0800U
#define Du16Lan8742_BcrIsolate    0x0400U
#define Du16Lan8742_BcrRestartAN  0x0200U
#define Du16Lan8742_BcrFullDuplex 0x0100U

//BSR bits
#define Du16Lan8742_BsrAutoNegDone 0x0020U
#define Du16Lan8742_BsrLinkUp      0x0004U

//ANAR: 100FD | 100HD | 10FD | 10HD | IEEE 802.3 selector
#define Du16Lan8742_AnarAll       0x01E1U

//PHYSCSR bits
#define Du16Lan8742_ScsrAutoDone  0x1000U
#define Du16Lan8742_ScsrSpeedMask 0x001CU
#define Du16Lan8742_Scsr10Half    0x0004U
#define Du16Lan8742_Scsr10Full    0x0014U
#define Du16Lan8742_Scsr100Half   0x0008U
#define Du16Lan8742_Scsr100Full   0x0018U

//identifier (PHYID2 low nibble is the silicon revision)
#define Du16Lan8742_Id1           0x0007U
#define Du16Lan8742_Id2           0xC130U
#define Du16Lan8742_Id2Mask       0xFFF0U

#define Du32Lan8742_ResetTimeoutMs 500UL

/* Public Function Prototypes ------------------------------------------------*/

/******************************************************************************/
//function desc: check PHY ID, soft reset, then start auto-neg or force mode
//inputs: Fpst_Config -> driver config (u8AutoNeg, enSpeed, enDuplex)
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_Init(const TstEth_Config *Fpst_Config);

/******************************************************************************/
//function desc: read current link state and resolved speed/duplex
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_GetLink(TenEth_LinkState *Fpen_State, TenEth_Speed *Fpen_Speed, TenEth_Duplex *Fpen_Duplex);

/******************************************************************************/
//function desc: enable / disable PHY internal loopback
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_SetLoopback(uint8_t Fu8Enable);

/******************************************************************************/
//function desc: enter PHY power down mode
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Lan8742_PowerDown(void);

#endif /* ETHERNET_LAN8742_H_ */
