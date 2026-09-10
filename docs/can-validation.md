# CAN analyzer validation

Validation completed on 2026-09-10 for the integrated ESP32 dashboard project. These checks confirm compilation, bounded analysis behavior, the listen-only software contract, browser input handling, and the offline CSV workflow. They do not replace testing with the final transceiver, wiring, vehicle bus, and SD card.

## Build results

| Target | Toolchain | Result | Static usage |
|:--|:--|:--|:--|
| Integrated dashboard firmware | PlatformIO, Espressif32 6.10.0, Arduino ESP32 2.0.17, `esp32dev` | PASS | 65,608 B RAM (20.0%); 972,913 B flash (74.2%) |
| Isolated bench generator | Arduino CLI, Arduino ESP32 2.0.10, `esp32:esp32:esp32` | PASS | 21,456 B RAM (6%); 267,497 B flash (20%) |

The dashboard web bundle is 105,322 bytes before gzip and 28,536 bytes after gzip. The resulting firmware binary is available at `.pio/build/esp32dev/firmware.bin` after a successful build.

## Automated checks

| Check | Result | Coverage |
|:--|:--|:--|
| Portable C++ analysis tests | PASS | Intel and Motorola extraction, valid boundaries, Welford statistics, DLC/RTR handling, loss gaps, bounded rings, 2/4/8/16-bit modulo counters, baseline/action ranking, held values, ramps, and additive checksum evidence |
| Passive firmware contract | PASS | Production firmware contains no `twai_transmit` call, installs `TWAI_MODE_LISTEN_ONLY`, sets `tx_queue_len=0`, boots with CAN/acquisition disabled, pauses Dashboard devices while retaining SD, exposes only two main modes, and keeps Encoder/IMU in Dashboard |
| CAN dashboard JavaScript | PASS | Listen-only packet validation, bounded ID/candidate arrays, CAN driver states, hexadecimal IDs, experiment label rules, and 1–3600 second capture bounds |
| Existing dashboard JavaScript | PASS | Recording cadence, device controls, encoder parameters, INA shunts, GA controls, supply states, recovery, CSV behavior, and IMU model behavior |
| Firmware CSV compatibility | PASS | Seven native firmware scenarios parsed by `DashboardCore`, with the expected 77-column sensor CSV schema |
| Offline CAN CSV analyzer | PASS | A 64-frame synthetic capture identified an additive checksum candidate in both chronological discovery and validation halves |
| Python syntax checks | PASS | CAN offline analyzer, demo builder, dashboard embedder, and firmware CSV harness |
| Interactive browser review | PASS | Dashboard and CAN Analyzer are the only main modes; the demo models exclusive sensor pause, CAN enable/disable, and runtime bitrate control |
| Git whitespace check | PASS | No whitespace errors in the pending change set |

The host-side C++ layout is `Frame=32`, `Sample=24`, and `Record=3696` bytes. Sixteen records use 59,136 bytes before allocator overhead. This runtime allocation is separate from the static RAM figure reported by the firmware linker.

## Commands used

```text
pio run
node --test dashboard/core.test.cjs dashboard/imu-model.test.cjs dashboard/can.test.cjs
python -m unittest tests.can.test_offline_analyzer tests.can.test_passive_contract
python scripts/test_firmware_csv.py --cxx <C++ compiler> --node <Node.js>
<C++ compiler> -std=c++11 -O2 -Wall -Wextra -Werror src/can/Analysis.cpp tests/can/test_analysis.cpp
arduino-cli compile --fqbn esp32:esp32:esp32 examples/can_bench_generator
git diff --check
```

## Hardware validation still required

- Verify CAN TX GPIO25, CAN RX GPIO26, the selected bitrate, transceiver voltage, standby/silent pins, ground/reference strategy, and bus termination against the assembled board.
- Confirm with a logic analyzer that the transceiver TXD and CAN bus remain recessive during ESP32 boot, reset, normal capture, malformed traffic, and software failure.
- Run sustained full-load traffic while SD logging and web reports are active. Record driver misses, overruns, queue peaks, dropped frames, minimum heap, stack headroom, and reset reasons.
- Test SD open, slow-write, full-card, clean-close, removal, and power-loss behavior using a disposable bench card.
- Repeat controlled baseline/action experiments and compare raw CSV against an independent physical reference before assigning any signal meaning.

The separate bench generator intentionally transmits synthetic frames and is excluded from the dashboard firmware. Use it only on an isolated two-node test bus; never connect that generator build to a vehicle.
