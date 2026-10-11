/*
 * main.h
 *
 *  Created on: Jan 12, 2025
 *      Author: hosni
 */

#ifndef MAIN_H_
#define MAIN_H_

#include "core_cm7.h"
#include "stm32f7xx.h"
#include "system_stm32f7xx.h"

#include "../drivers/STM32F7xx_HAL_Driver/Inc/stm32f7xx_ll_bus.h"
#include "../drivers/STM32F7xx_HAL_Driver/Inc/stm32f7xx_ll_gpio.h"
#include "../drivers/STM32F7xx_HAL_Driver/Inc/stm32f7xx_ll_rcc.h"
#include "../drivers/STM32F7xx_HAL_Driver/Inc/stm32f7xx_ll_pwr.h"
#include "../drivers/STM32F7xx_HAL_Driver/Inc/stm32f7xx_ll_system.h"

#include "../Src/scheduler/scheduler.h"
#include "../Src/ethernet/ethernet.h"
#include "../Src/safety/safety.h"

#define LED_1_Pin LL_GPIO_PIN_1
#define LED_1_GPIO_Port GPIOI

//system clock: 120MHz from PLL (HSE 25MHz crystal, HSI fallback)
#define Du32Main_HseTimeout       0x0003FFFFUL
#define Du32Main_PllN             240U    //VCO = 1MHz * 240 = 240MHz


void vMain_SystemClockConfig(void);
void vMain_InitGpios(void);
void vMain_HeartBeatLED();

#endif /* MAIN_H_ */
