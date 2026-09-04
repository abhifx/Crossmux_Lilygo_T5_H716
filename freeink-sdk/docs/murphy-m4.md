# Murphy M4 board support

Build with `FREEINK_DEVICE_MURPHY_M4=1` on an ESP32-S3 N16R8 target using DIO
flash and OPI PSRAM. The profile provides:

- SSD1677 800×480 on GPIO4/3/5/6/7/8 at 20 MHz;
- GPIO0 power plus GPIO1/2 navigation; GPIO0 is not a confirm key, and its short
  action remains a consumer setting;
- FT6336U at `0x2E` on native I²C1 SDA13/SCL12 at 100 kHz, unusable active-low
  GPIO46 IRQ, active-low GPIO45 power, and GPIO7 reset shared with the display;
- 4-bit SDMMC on CLK16/CMD15/D0=17/D1=18/D2=11/D3=14 with active-low GPIO10
  power;
- RX8010SJ at `0x32` on the same native I²C1 bus with a 400 kHz device handle,
  ADC9 battery sensing with a 2.0 divider, and active-low GPIO43 charging status;
- cool GPIO47 and warm GPIO48 frontlight PWM at 25 kHz / 10-bit using the
  official gamma-1.6554 percentage curve.

The native I²C1 bus and its two device handles are allocated once and retained
until reset. Display initialization toggles the shared GPIO7 reset, so firmware
must call `InputManager::reinitializeTouchAfterSharedReset()` after
`FreeInkDisplay::begin()` to restore the FT6336U's volatile mode, threshold, and
report-rate registers.

After that reinitialization, `InputManager` starts a core-0 task that reads one
validated frame every 10 ms without consulting the unusable IRQ level. A fixed
snapshot retains the first unconsumed press-to-release gesture while the
consumer is blocked by an e-paper refresh; gesture classification remains in
the consumer's normal `InputManager::update()` path. The task uses a static
3072-byte stack, static TCB, and fixed state guarded by a short critical section,
so it creates no queue or heap allocation. The native IDF I²C1 driver serializes
touch and RX8010 transactions. Deep sleep pauses the sampler and waits for any
in-flight read before disabling GPIO45.

The SDK owns no board-revision probe or thresholds. Before
`InputManager::begin()`, the consumer selects a final batch and calls
`InputManager::setMurphyM4Batch()` and
`FreeInkDisplay::setMurphyM4Batch()` before display initialization. The display
facade retains the selection and constructs the SSD1677 singleton from one of
two immutable configurations; it does not mutate shared driver config. Batch 1
(no R13) uses HALF/window pseudo-temperature `0x3C` and touch short-axis range
`[-52,553]`; batch 2 (R13 fitted) uses `0x50` and `[-47,514]`. Define
`FREEINK_MURPHY_M4_BATCH1=1` only for recovery or diagnostics.

GPIO1 reference data overlaps across batches: first-batch hardware measured a
6008 µs median across 101 samples (6004–6059 µs, 0.379% coefficient of
variation), and known second-batch hardware measured 6218 µs (6170–6233 µs,
101/101 valid). This disproves GPIO1-only detection. The consumer therefore may
compare GPIO2/R13 against GPIO1 to positively confirm batch 1, but must use
batch 2 as the default for every other result. A first-batch restart has also
confirmed that the consumer's existing v2 First cache reaches these SDK display
and touch selections unchanged.

On first-batch hardware, the touch task retained at least 1120 bytes of its
3072-byte static stack during a 70-second diagnostic run. Free/minimum/largest
PSRAM remained fixed at 8091424/8091424/7995380 bytes; no touch/RTC I²C read
failure was observed. Physical gesture and Power sleep/wake checks remain a
release requirement.
AHT20 and SC7A20 are not part of the initial reader profile.
