# Safety platform (STM32F7, bare metal)

Generic safety layer for projects using the time-triggered scheduler.
It checks that the MCU and the software are healthy and forces the
application into its safe state when they are not. The application
only decides what "safe" means (`vSafetyApp_EnterSafeState()`).

## Files

| File | Role |
|---|---|
| `safety_cfg.h` | project configuration, the only file a project edits |
| `safety.h / safety.c` | public API, startup sequence, runtime task, fault log, safe state |
| `safety_selftest.c` | RAM March C- (transparent), flash CRC (hardware CRC unit) |
| `safety_cpu_test.s` | CPU register R0-R12 and APSR flag test |
| `safety_clock.c` | CSS (HSE loss -> NMI), SYSCLK vs LSI cross-check (TIM5 capture) |
| `safety_flow.c` | task alive supervision, IWDG with window |
| `safety_fault.c` | HardFault / MemManage / BusFault / UsageFault / NMI handlers |
| `safety_mpu.c` | MPU: RO flash, XN RAM, non-cacheable DMA region, stack and NULL guards |
| `../../tools/flash_crc.py` | post-build: writes the expected flash CRC into the ELF |

## Integration

```c
vMain_SystemClockConfig();
vMain_InitGpios();                 // outputs at their safe level first
vSafety_Init();                    // startup tests, fault handlers, MPU
s16Schd_Init(Du16Schd_TickMs);
... driver init, s16Schd_AddTask(...) ...
s16Schd_AddTask(vSafety_Task, 0, Du32SafetyCfg_TaskPeriodMs);
vSafety_Start();                   // watchdog + supervision
vSchd_Enable(SCHD_ENABLE);
```

and implement the hook:

```c
void vSafetyApp_EnterSafeState(void)
{
  //de-energize outputs: short, non blocking, callable from any context
}
```

DMA buffers must be declared with `__attribute__((section(".dma_buffer")))`.

## What is checked

| Check | When | Fault code |
|---|---|---|
| CPU registers / flags | startup + every task period | `SAFETY_FAULT_CPU_TEST` |
| Flash CRC | startup (full) + runtime (1KB per period) | `SAFETY_FAULT_FLASH_CRC` |
| RAM March C- | startup (full) + runtime (256B per period) | `SAFETY_FAULT_RAM_TEST` |
| HSE loss (CSS) | continuous (NMI) | `SAFETY_FAULT_CLOCK_HSE` |
| SYSCLK vs LSI | startup + every 1s | `SAFETY_FAULT_CLOCK_FREQ` |
| Task run count per window | every 1s | `SAFETY_FAULT_TASK_ALIVE` |
| Task execution time > budget | every task run | `SAFETY_FAULT_TASK_OVERRUN` |
| Scheduler late (ticks pile up) | every tick | `SAFETY_FAULT_TICK_OVERRUN` |
| CPU faults, stack overflow, NULL access | exception | `SAFETY_FAULT_HARDFAULT` ... |
| Watchdog (too late or too early) | IWDG | reset, logged at next boot |

The fault log (last 8 faults with PC/LR/CFSR/address) lives in `.noinit`
RAM and survives resets: read it with `s16Safety_GetFault()`.

## Flash CRC post-build step

In CubeIDE: Project > Properties > C/C++ Build > Settings > Build Steps >
Post-build steps:

```
python ../tools/flash_crc.py ${ProjName}.elf
```

Without it the CRC word stays `0xFFFFFFFF`; this is accepted only while
`Du8SafetyCfg_FlashAllowUnpatched` is 1 (development). Set it to 0 for a
release. Note: a `.bin`/`.hex` generated before the post-build step does
not contain the patched CRC; flash the ELF or regenerate them afterwards.

## Known limitations

- The stack area and the `.dma_buffer` region are not covered by the RAM test.
- The LSI cross-check only detects gross clock errors (LSI is 17-47kHz).
  A product needing tighter monitoring should use the LSE crystal as reference.
- A stack overflow during exception entry can lock up the core; the IWDG
  then resets the MCU (`SAFETY_FAULT_WATCHDOG_RESET` logged at next boot).
- One MCU, one channel: the platform provides diagnostics, not redundancy.
  SIL 2 / PL d products still need a hardware architecture (dual channel,
  output feedback) and their own safety analysis.
