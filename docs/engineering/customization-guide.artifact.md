# Lilygo Crossmux Customization Guide

This document tracks all modifications made to the [CrossMux/CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) core to support the **Lilygo T5 H716 (ESP32-S3)** board.

## Core Philosophical Goal
Maintain 100% compatibility with the upstream reading engine while providing an optimized experience for the H716 display and hardware.

## Modified Core Files (Firmware)

| File Path | Nature of Change | Purpose |
|---|---|---|
| `platformio.ini` | Added `lilygo_h716` env | Build environment for S3 + PSRAM + EPD_Painter |
| `src/main.cpp` | GPIO/Power/Display init | Hardware-specific boot sequence |
| `lib/hal/HalGPIO.h/cpp` | Added `DeviceType::H716` | Runtime device detection |
| `lib/hal/HalDisplay.h/cpp` | Geometry & Driver selection | Support for 7-inch 800x480 grayscale EPD |
| `src/CrossPointSettings.h` | Grayscale/Refresh settings | Expose H716-specific display controls |
| `lib/Epub/...` | Grayscale dither / conversion | 16-level grayscale rendering path |
| `src/activities/home/...` | UI Layout adjustments | Optimized dashboard for larger screen |

## Modified Core Files (SDK - freeink-sdk)

| File Path | Nature of Change | Purpose |
|---|---|---|
| `libs/display/FreeInkDisplay/...` | Added `PainterDriver` | Support for the GDEH0716T71 display |
| `libs/hardware/BoardConfig/...` | Added `LilygoH716.h` | Board-level pin mapping and config |
| `libs/hardware/InputManager/...` | Touch support for S3 | Support for the touch controller on H716 |

## Board-Specific Invariants (H716)
- **MCU:** ESP32-S3 (with PSRAM)
- **Display:** 7.16" E-Ink (800x480)
- **Grayscale:** Supported (16 levels) via `EPD_Painter`
- **Memory:** `dio_opi` (Octal PSRAM) is required for boot stability.

## Maintenance Checklist for Upstream Sync
1. Run `git fetch upstream`.
2. Merge `upstream/main` into `main`.
3. Use `scripts/sync-upstream.ps1` to resolve guide conflicts.
4. Verify `platformio.ini` has not reverted `board_build.arduino.memory_type = dio_opi`.
5. Rebuild `lilygo_h716` and check serial for "Hardware detect: H716".
