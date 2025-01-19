/*
 * task_iwdt.c
 *
 *  Created on: Jan 19, 2025
 *      Author: hosni
 */

#include "task_iwdt.h"
/******************************************************************************/
//function desc: setup Iwdt
//inputs: Fu32ReloadMs-> iwdt timeout in ms
//outputs:
/******************************************************************************/
void vTaskIwdt_init(uint32_t Fu32ReloadMs)
{
  //start iWDT
  IWDG->KR = Du32iWDT_StartKey;
  //unlock iWDT
  IWDG->KR = Du32iWDT_UnlockKey;
  // set prescaler to 32
  IWDG->PR = Du32iWDT_ClkPrescaler;
  // set reload
  IWDG->RLR = Fu32ReloadMs;

  //wait to validate register write
  while (0u != IWDG->SR )
    {}
  //reload
  IWDG->KR = Du32iWDT_ReloadKey;

}


/******************************************************************************/
//function desc:
//inputs:
//outputs:
/******************************************************************************/
void vTaskIWDT_Reload(void)
{
  IWDG->KR = Du32iWDT_ReloadKey;
}
