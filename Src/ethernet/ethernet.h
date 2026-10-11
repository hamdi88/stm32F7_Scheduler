/*
 * ethernet.h
 *
 *  Created on: Jun 8, 2026
 *      Author: hosni
 *
 *  Bare-metal Ethernet MAC/DMA driver for the STM32F746G-DISCO board
 *  (on-board LAN8742A PHY, RMII interface, LL drivers only).
 *
 *  Board wiring (RMII, AF11):
 *    PA1  REF_CLK   PA2  MDIO    PA7  CRS_DV
 *    PC1  MDC       PC4  RXD0    PC5  RXD1
 *    PG11 TX_EN     PG13 TXD0    PG14 TXD1
 *
 *  Typical usage with the cooperative scheduler:
 *    s16Eth_Init(NULL);                        // defaults, MAC from chip UID
 *    vEth_RegisterRxCallback(my_rx_handler);
 *    s16Schd_AddTask(vEth_Process, 0, 1);      // RX drain / TX reclaim
 *    s16Schd_AddTask(vEth_LinkTask, 0, 100);   // PHY link monitoring
 *
 *  Requirement: HCLK must be >= 25 MHz (RM0385), configure the PLL first.
 */

#ifndef ETHERNET_ETHERNET_H_
#define ETHERNET_ETHERNET_H_


/* Includes ------------------------------------------------------------------*/

#include <stdint.h>
#include <stddef.h>
#include "stm32f7xx.h"

/* Public Macros -------------------------------------------------------------*/

#define Du8Eth_MacAddrLen         6U
#define Du16Eth_MaxFrameLen       1518U   //DA+SA+Type+1500 payload+CRC
#define Du16Eth_MaxPayloadFrameLen 1514U  //frame handed to/from the driver (no CRC)
#define Du16Eth_MinFrameLen       60U     //shorter frames are zero padded by the MAC

#define Du16Eth_RxDescNb          8U      //number of RX DMA descriptors/buffers
#define Du16Eth_TxDescNb          4U      //number of TX DMA descriptors/buffers
#define Du16Eth_BufferSize        1536U   //multiple of 32 (cache line)

#define Du32Eth_MinHclkHz         25000000UL

/* Public Types --------------------------------------------------------------*/

enum
{
  ETH_OK          = 0,
  ETH_ERR         = -1,  //generic / invalid parameter
  ETH_ERR_TIMEOUT = -2,  //hardware did not respond in time
  ETH_ERR_BUSY    = -3,  //no free TX descriptor
  ETH_ERR_NO_DATA = -4,  //no frame received
  ETH_ERR_PHY     = -5,  //PHY not found / wrong ID
  ETH_ERR_CLOCK   = -6,  //HCLK too low for the Ethernet peripheral
  ETH_ERR_STATE   = -7,  //driver not in the right state
  ETH_ERR_FRAME   = -8,  //received frame had errors or was too big
};

typedef enum
{
  ETH_LINK_DOWN = 0,
  ETH_LINK_UP   = 1
} TenEth_LinkState;

typedef enum
{
  ETH_SPEED_10M  = 0,
  ETH_SPEED_100M = 1
} TenEth_Speed;

typedef enum
{
  ETH_DUPLEX_HALF = 0,
  ETH_DUPLEX_FULL = 1
} TenEth_Duplex;

typedef struct
{
  uint8_t      au8MacAddr[Du8Eth_MacAddrLen]; //all zero -> derived from chip UID
  uint8_t      u8AutoNeg;        //1 -> auto-negotiation, 0 -> forced speed/duplex
  TenEth_Speed enSpeed;          //used only when u8AutoNeg == 0
  TenEth_Duplex enDuplex;        //used only when u8AutoNeg == 0
  uint8_t      u8ChecksumOffload;//1 -> HW IPv4/TCP/UDP/ICMP checksum insert & check
  uint8_t      u8UseIrq;         //1 -> enable ETH IRQ (wakes the CPU from WFI)
  uint8_t      u8PhyAddr;        //LAN8742A address, 0 on the DISCO board
} TstEth_Config;

typedef struct
{
  uint32_t u32RxFrames;      //good frames delivered
  uint32_t u32RxErrors;      //frames dropped because of errors
  uint32_t u32RxDropped;     //frames dropped (too big / user buffer too small)
  uint32_t u32RxBufUnavail;  //DMA ran out of RX descriptors
  uint32_t u32TxFrames;      //frames transmitted successfully
  uint32_t u32TxErrors;      //frames with TX error status
  uint32_t u32TxBusy;        //transmit refused, ring full
  uint32_t u32FatalBusErr;   //DMA fatal bus errors
  /* hardware MMC counters */
  uint32_t u32MmcTxGood;
  uint32_t u32MmcTxSingleCol;
  uint32_t u32MmcTxMultiCol;
  uint32_t u32MmcRxCrcErr;
  uint32_t u32MmcRxAlignErr;
  uint32_t u32MmcRxGoodUnicast;
} TstEth_Stats;

typedef void (*TpEth_RxCallback)(const uint8_t *Fpu8Frame, uint16_t Fu16Len);
typedef void (*TpEth_TxDoneCallback)(void);
typedef void (*TpEth_LinkCallback)(TenEth_LinkState FenState, TenEth_Speed FenSpeed, TenEth_Duplex FenDuplex);

/* Public Function Prototypes ------------------------------------------------*/

/******************************************************************************/
//function desc: fill a config struct with the board defaults
//               (auto-neg, checksum offload, IRQ on, PHY addr 0, UID MAC)
//inputs: Fpst_Config -> struct to fill
//outputs:
/******************************************************************************/
void vEth_GetDefaultConfig(TstEth_Config *Fpst_Config);

/******************************************************************************/
//function desc: init GPIOs, clocks, MAC, DMA, PHY and start the interface
//               PHY reset is blocking (< 500ms), auto-neg completes in vEth_LinkTask
//inputs: Fpst_Config -> configuration, NULL -> defaults
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Eth_Init(const TstEth_Config *Fpst_Config);

/******************************************************************************/
//function desc: stop the interface, power down the PHY, reset/clock-off the MAC
//inputs:
//outputs:
/******************************************************************************/
void vEth_DeInit(void);

/******************************************************************************/
//function desc: start / stop MAC + DMA transmission and reception
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Eth_Start(void);
int16_t s16Eth_Stop(void);

/******************************************************************************/
//function desc: queue one frame for transmission (frame is copied)
//inputs: Fpu8Frame -> DA+SA+Type+payload (no CRC), Fu16Len -> 14..1514 bytes
//outputs: ETH_OK, ETH_ERR_BUSY (ring full), ETH_ERR
/******************************************************************************/
int16_t s16Eth_Transmit(const uint8_t *Fpu8Frame, uint16_t Fu16Len);

/******************************************************************************/
//function desc: poll for one received frame (copied into user buffer, no CRC)
//               do not mix with vEth_Process when an RX callback is registered
//inputs: Fpu8Buf -> destination, Fu16BufSize -> its size, Fpu16Len -> frame len
//outputs: ETH_OK, ETH_ERR_NO_DATA, ETH_ERR_FRAME, ETH_ERR
/******************************************************************************/
int16_t s16Eth_Receive(uint8_t *Fpu8Buf, uint16_t Fu16BufSize, uint16_t *Fpu16Len);

/******************************************************************************/
//function desc: scheduler task (1ms): delivers RX frames to the RX callback
//               (zero copy, frame valid only during the callback) and reclaims
//               transmitted descriptors (TX done callback)
//inputs:
//outputs:
/******************************************************************************/
void vEth_Process(void);

/******************************************************************************/
//function desc: scheduler task (~100ms): polls the PHY, handles link up/down,
//               applies negotiated speed/duplex to the MAC, calls link callback
//inputs:
//outputs:
/******************************************************************************/
void vEth_LinkTask(void);

/******************************************************************************/
//function desc: callbacks registration (NULL to unregister)
//inputs:
//outputs:
/******************************************************************************/
void vEth_RegisterRxCallback(TpEth_RxCallback Fp_Callback);
void vEth_RegisterTxDoneCallback(TpEth_TxDoneCallback Fp_Callback);
void vEth_RegisterLinkCallback(TpEth_LinkCallback Fp_Callback);

/******************************************************************************/
//function desc: link information (last value seen by vEth_LinkTask)
//inputs:
//outputs:
/******************************************************************************/
TenEth_LinkState enEth_GetLinkState(void);
TenEth_Speed enEth_GetSpeed(void);
TenEth_Duplex enEth_GetDuplex(void);

/******************************************************************************/
//function desc: MAC address get / set (perfect filter, address 0)
//inputs: Fpu8Mac -> 6 bytes
//outputs:
/******************************************************************************/
void vEth_SetMacAddress(const uint8_t *Fpu8Mac);
void vEth_GetMacAddress(uint8_t *Fpu8Mac);

/******************************************************************************/
//function desc: address filtering
//  SetPromiscuous: 1 -> receive all frames
//  SetAllMulticast: 1 -> receive all multicast frames
//  SetMulticastList: hash filter rebuilt from the list (Fu16Count = 0 clears)
//inputs:
//outputs: ETH_OK or error code
/******************************************************************************/
void vEth_SetPromiscuous(uint8_t Fu8Enable);
void vEth_SetAllMulticast(uint8_t Fu8Enable);
int16_t s16Eth_SetMulticastList(const uint8_t (*Fpau8List)[Du8Eth_MacAddrLen], uint16_t Fu16Count);

/******************************************************************************/
//function desc: raw PHY register access through MDIO
//inputs: Fu8Reg -> 0..31
//outputs: ETH_OK or ETH_ERR_TIMEOUT
/******************************************************************************/
int16_t s16Eth_PhyRead(uint8_t Fu8Reg, uint16_t *Fpu16Value);
int16_t s16Eth_PhyWrite(uint8_t Fu8Reg, uint16_t Fu16Value);

/******************************************************************************/
//function desc: PHY loopback (frames sent are received back, link not needed)
//inputs: Fu8Enable -> 1 on, 0 off
//outputs: ETH_OK or error code
/******************************************************************************/
int16_t s16Eth_SetPhyLoopback(uint8_t Fu8Enable);

/******************************************************************************/
//function desc: statistics (software + hardware MMC counters)
//inputs:
//outputs:
/******************************************************************************/
void vEth_GetStats(TstEth_Stats *Fpst_Stats);
void vEth_ResetStats(void);

#endif /* ETHERNET_ETHERNET_H_ */
