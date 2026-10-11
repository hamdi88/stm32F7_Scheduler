/*
 * safety_cpu_test.s
 *
 *  Created on: Oct 11, 2026
 *      Author: hosni
 *
 *  CPU register self-test (stuck-at / coupling between bits).
 *  Each register R0-R12 is loaded with 0xAAAAAAAA then 0x55555555 and
 *  compared with a reference, then the APSR N Z C V Q flags are set and
 *  cleared in two complementary patterns.
 *
 *  uint32_t u32SafetyCpu_TestRegisters(void)
 *    returns 0 when every register works, 1 on failure
 */

  .syntax unified
  .cpu cortex-m7
  .thumb

  .section .text.u32SafetyCpu_TestRegisters,"ax",%progbits
  .global u32SafetyCpu_TestRegisters
  .type u32SafetyCpu_TestRegisters, %function

/* load PATTERN in REG then compare with the reference kept in r0 */
.macro CHECK_REG reg, pattern
  ldr \reg, =\pattern
  cmp \reg, r0
  bne cpu_test_fail
.endm

/* run the register check for one pattern on r1-r12 */
.macro CHECK_ALL pattern
  ldr r0, =\pattern             /* reference value */
  CHECK_REG r1, \pattern
  CHECK_REG r2, \pattern
  CHECK_REG r3, \pattern
  CHECK_REG r4, \pattern
  CHECK_REG r5, \pattern
  CHECK_REG r6, \pattern
  CHECK_REG r7, \pattern
  CHECK_REG r8, \pattern
  CHECK_REG r9, \pattern
  CHECK_REG r10, \pattern
  CHECK_REG r11, \pattern
  CHECK_REG r12, \pattern
.endm

u32SafetyCpu_TestRegisters:
  push {r4-r11, lr}             /* callee saved registers are restored at exit */

  /* r1-r12 against r0, both patterns */
  CHECK_ALL 0xAAAAAAAA
  CHECK_ALL 0x55555555

  /* r0 itself, checked against r1 */
  ldr r1, =0xAAAAAAAA
  ldr r0, =0xAAAAAAAA
  cmp r0, r1
  bne cpu_test_fail
  ldr r1, =0x55555555
  ldr r0, =0x55555555
  cmp r0, r1
  bne cpu_test_fail

  /* APSR flags: N C Q set, Z V clear */
  ldr r0, =0xA8000000
  msr APSR_nzcvq, r0
  mrs r1, APSR
  ldr r2, =0xF8000000           /* N Z C V Q mask */
  and r1, r1, r2
  cmp r1, r0
  bne cpu_test_fail

  /* APSR flags: Z V set, N C Q clear */
  ldr r0, =0x50000000
  msr APSR_nzcvq, r0
  mrs r1, APSR
  and r1, r1, r2
  cmp r1, r0
  bne cpu_test_fail

  /* clear every flag before returning */
  movs r0, #0
  msr APSR_nzcvq, r0

  movs r0, #0                   /* pass */
  pop {r4-r11, pc}

cpu_test_fail:
  movs r0, #1                   /* fail */
  pop {r4-r11, pc}

  .ltorg                        /* literal pool for the ldr = constants */
  .size u32SafetyCpu_TestRegisters, .-u32SafetyCpu_TestRegisters
