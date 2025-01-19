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

#include "../Src/tasks/task_iwdt.h"
#include "../Src/scheduler/scheduler.h"

#define LED_1_Pin LL_GPIO_PIN_1
#define LED_1_GPIO_Port GPIOI


void vMain_InitGpios(void);
void vMain_HeartBeatLED();

#endif /* MAIN_H_ */
