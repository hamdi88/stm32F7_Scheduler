/*
 * ethernet_internal.h
 *
 *  Created on: Jun 8, 2026
 *      Author: hosni
 *
 *  Helpers shared between the ethernet driver files, not part of the public API
 */

#ifndef ETHERNET_ETHERNET_INTERNAL_H_
#define ETHERNET_ETHERNET_INTERNAL_H_


/* Includes ------------------------------------------------------------------*/

#include <stdint.h>

/* Public Function Prototypes ------------------------------------------------*/

/******************************************************************************/
//function desc: busy wait based on the DWT cycle counter
//inputs: Fu32Us -> delay in microseconds
//outputs:
/******************************************************************************/
void vEth_DelayUs(uint32_t Fu32Us);

#endif /* ETHERNET_ETHERNET_INTERNAL_H_ */
