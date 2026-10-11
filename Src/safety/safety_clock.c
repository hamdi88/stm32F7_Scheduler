/*
 * safety_clock.c
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  Clock monitoring:
 *   - CSS (clock security system): HSE failure -> NMI (see safety_fault.c)
 *   - cross-check of SYSCLK against the independent LSI oscillator:
 *     TIM5 (clocked from APB1, derived from SYSCLK) captures 8 LSI periods
 *     on channel 4 (internally remapped to LSI). If SYSCLK is not what the
 *     software believes, the measured LSI frequency falls out of its
 *     datasheet range.
 */


/* Includes ------------------------------------------------------------------*/

#include "safety_internal.h"
#include "stm32f7xx_ll_bus.h"
#include "stm32f7xx_ll_rcc.h"

/* Private Macros ------------------------------------------------------------*/

#define Du32SafetyClock_LsiPeriods     8UL      //LSI periods per capture (IC prescaler /8)
#define Du32SafetyClock_LsiTimeoutUs   5000UL   //LSI start / capture timeout (blocking)
#define Du32SafetyClock_StepTimeout    10UL     //vSafety_Task calls before a measure times out
#define Du32SafetyClock_IrqPriority    7UL

//measurement phases (interrupt driven runtime check)
#define Du8SafetyClock_Idle            0U
#define Du8SafetyClock_WaitFirst       1U
#define Du8SafetyClock_WaitSecond      2U
#define Du8SafetyClock_Done            3U

/* Private Types -------------------------------------------------------------*/


/* Private Variables ---------------------------------------------------------*/

//current measurement phase, shared with TIM5_IRQHandler
static volatile uint8_t u8SafetyClock_Phase = Du8SafetyClock_Idle;

//TIM5 value captured at the first LSI edge
static volatile uint32_t u32SafetyClock_FirstCapture = 0;

//TIM5 ticks measured over Du32SafetyClock_LsiPeriods LSI periods
static volatile uint32_t u32SafetyClock_Ticks = 0;

//vSafety_Task calls spent waiting for the current measurement
static uint32_t u32SafetyClock_Wait = 0;

/* Private Function Prototypes -----------------------------------------------*/

static uint32_t u32SafetyClock_TimerHz(void);
static uint8_t u8SafetyClock_Evaluate(uint32_t Fu32Ticks, uint32_t *Fpu32Measured);
static uint8_t u8SafetyClock_WaitCapture(uint32_t *Fpu32Capture);

/* Public Function Definitions -----------------------------------------------*/

/******************************************************************************/
//function desc: check the clock tree, enable CSS, LSI and the capture timer
//inputs:
//outputs: Du8Safety_TestPass / Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetyClock_Init(void)
{
  uint8_t u8Ret = Du8Safety_TestPass;  //result
  uint32_t u32Start;                   //DWT counter at the start of the LSI wait
  uint8_t u8HseUsed;                   //1 if SYSCLK = PLL fed by HSE

  u8HseUsed = ((LL_RCC_SYS_CLKSOURCE_STATUS_PLL == LL_RCC_GetSysClkSource()) &&
               (LL_RCC_PLLSOURCE_HSE == LL_RCC_PLL_GetMainSource()) &&
               (1U == LL_RCC_HSE_IsReady())) ? 1U : 0U;

#if (Du8SafetyCfg_RequireHse == 1U)
  if (!u8HseUsed)
    {
      u8Ret = Du8Safety_TestFail;
    }
#endif

  //HSE failure -> NMI, hardware switches SYSCLK to HSI
  if (u8HseUsed)
    {
      LL_RCC_HSE_EnableCSS();
    }

  //LSI: independent reference (also clocks the IWDG)
  //a dead LSI is reported by the frequency check (capture timeout)
  LL_RCC_LSI_Enable();
  u32Start = DWT->CYCCNT;
  while ((1U != LL_RCC_LSI_IsReady()) &&
         ((DWT->CYCCNT - u32Start) < (Du32SafetyClock_LsiTimeoutUs * (SystemCoreClock / 1000000UL))))
    {
    }

  //TIM5: free running 32-bit counter, CH4 input capture of LSI, /8
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM5);
  DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM5_STOP;
  TIM5->CR1 = 0;
  TIM5->OR = TIM_OR_TI4_RMP_0;                     //TI4 <- LSI
  TIM5->PSC = 0;
  TIM5->ARR = 0xFFFFFFFFUL;
  TIM5->CCMR2 = TIM_CCMR2_CC4S_0 | TIM_CCMR2_IC4PSC; //IC4 on TI4, capture every 8 edges
  TIM5->CCER = TIM_CCER_CC4E;                       //rising edge
  TIM5->EGR = TIM_EGR_UG;                           //load PSC
  TIM5->SR = 0;
  TIM5->CR1 = TIM_CR1_CEN;

  NVIC_SetPriority(TIM5_IRQn, Du32SafetyClock_IrqPriority);
  NVIC_EnableIRQ(TIM5_IRQn);

  return u8Ret;
}

/******************************************************************************/
//function desc: one complete measurement by polling (startup)
//inputs: Fpu32Measured -> measured LSI frequency (Hz)
//outputs: Du8Safety_TestPass / Du8Safety_TestFail
/******************************************************************************/
uint8_t u8SafetyClock_CheckBlocking(uint32_t *Fpu32Measured)
{
  uint8_t u8Ret = Du8Safety_TestFail;  //result
  uint32_t u32First = 0;               //capture at the first edge
  uint32_t u32Second = 0;              //capture at the second edge

  *Fpu32Measured = 0;
  TIM5->DIER &= ~TIM_DIER_CC4IE;       //polling, no interrupt
  (void)TIM5->CCR4;                    //drop an old capture
  TIM5->SR = ~TIM_SR_CC4IF;

  if ((Du8Safety_TestPass == u8SafetyClock_WaitCapture(&u32First)) &&
      (Du8Safety_TestPass == u8SafetyClock_WaitCapture(&u32Second)))
    {
      u8Ret = u8SafetyClock_Evaluate(u32Second - u32First, Fpu32Measured);
    }

  return u8Ret;
}

/******************************************************************************/
//function desc: non-blocking measurement, call periodically (runtime)
//inputs: Fpu32Measured -> measured LSI frequency (Hz) when finished
//outputs: Du8Safety_TestBusy while measuring, then Pass / Fail
/******************************************************************************/
uint8_t u8SafetyClock_Step(uint32_t *Fpu32Measured)
{
  uint8_t u8Ret = Du8Safety_TestBusy;  //result

  switch (u8SafetyClock_Phase)
    {
    case Du8SafetyClock_Idle:
      //arm: the interrupt captures two edges then stops itself
      u32SafetyClock_Wait = 0;
      TIM5->SR = ~TIM_SR_CC4IF;
      u8SafetyClock_Phase = Du8SafetyClock_WaitFirst;
      TIM5->DIER |= TIM_DIER_CC4IE;
      break;

    case Du8SafetyClock_Done:
      u8Ret = u8SafetyClock_Evaluate(u32SafetyClock_Ticks, Fpu32Measured);
      u8SafetyClock_Phase = Du8SafetyClock_Idle;
      break;

    default:
      //still waiting: LSI or timer dead if it takes too long
      u32SafetyClock_Wait++;
      if (u32SafetyClock_Wait > Du32SafetyClock_StepTimeout)
        {
          TIM5->DIER &= ~TIM_DIER_CC4IE;
          u8SafetyClock_Phase = Du8SafetyClock_Idle;
          *Fpu32Measured = 0;
          u8Ret = Du8Safety_TestFail;
        }
      break;
    }

  return u8Ret;
}

/******************************************************************************/
//function desc: TIM5 interrupt, captures two LSI edges for u8SafetyClock_Step
//inputs:
//outputs:
/******************************************************************************/
void TIM5_IRQHandler(void)
{
  uint32_t u32Capture;  //captured counter value

  if (TIM5->SR & TIM_SR_CC4IF)
    {
      u32Capture = TIM5->CCR4;  //reading CCR4 clears CC4IF

      if (Du8SafetyClock_WaitFirst == u8SafetyClock_Phase)
        {
          u32SafetyClock_FirstCapture = u32Capture;
          u8SafetyClock_Phase = Du8SafetyClock_WaitSecond;
        }
      else if (Du8SafetyClock_WaitSecond == u8SafetyClock_Phase)
        {
          u32SafetyClock_Ticks = u32Capture - u32SafetyClock_FirstCapture;
          u8SafetyClock_Phase = Du8SafetyClock_Done;
          TIM5->DIER &= ~TIM_DIER_CC4IE;
        }
      else
        {
          TIM5->DIER &= ~TIM_DIER_CC4IE;  //not expected, stop
        }
    }
  TIM5->SR = ~(TIM_SR_CC4OF | TIM_SR_UIF);
}

/* Private Function Definitions ----------------------------------------------*/

//TIM5 clock: PCLK1, x2 when the APB1 prescaler is not 1
static uint32_t u32SafetyClock_TimerHz(void)
{
  uint32_t u32Prescaler = LL_RCC_GetAPB1Prescaler();  //APB1 prescaler setting
  uint32_t u32Div;                                    //APB1 division factor

  switch (u32Prescaler)
    {
    case LL_RCC_APB1_DIV_2:  u32Div = 2U;  break;
    case LL_RCC_APB1_DIV_4:  u32Div = 4U;  break;
    case LL_RCC_APB1_DIV_8:  u32Div = 8U;  break;
    case LL_RCC_APB1_DIV_16: u32Div = 16U; break;
    default:                 u32Div = 1U;  break;
    }

  return (1U == u32Div) ? SystemCoreClock : ((SystemCoreClock / u32Div) * 2U);
}

//LSI frequency = timer clock * periods / ticks, must be in datasheet range
static uint8_t u8SafetyClock_Evaluate(uint32_t Fu32Ticks, uint32_t *Fpu32Measured)
{
  uint8_t u8Ret = Du8Safety_TestFail;  //result
  uint64_t u64Hz;                      //measured LSI frequency

  *Fpu32Measured = 0;
  if (Fu32Ticks != 0U)
    {
      u64Hz = ((uint64_t)u32SafetyClock_TimerHz() * Du32SafetyClock_LsiPeriods) / Fu32Ticks;
      *Fpu32Measured = (uint32_t)u64Hz;
      if ((u64Hz >= Du32SafetyCfg_LsiMinHz) && (u64Hz <= Du32SafetyCfg_LsiMaxHz))
        {
          u8Ret = Du8Safety_TestPass;
        }
    }

  return u8Ret;
}

//poll one capture event with timeout
static uint8_t u8SafetyClock_WaitCapture(uint32_t *Fpu32Capture)
{
  uint8_t u8Ret = Du8Safety_TestPass;                                //result
  uint32_t u32Start = DWT->CYCCNT;                                   //DWT at entry
  uint32_t u32Max = Du32SafetyClock_LsiTimeoutUs * (SystemCoreClock / 1000000UL); //timeout in cycles

  while ((0U == (TIM5->SR & TIM_SR_CC4IF)) && (Du8Safety_TestPass == u8Ret))
    {
      if ((DWT->CYCCNT - u32Start) > u32Max)
        {
          u8Ret = Du8Safety_TestFail;
        }
    }
  *Fpu32Capture = TIM5->CCR4;  //clears CC4IF

  return u8Ret;
}
