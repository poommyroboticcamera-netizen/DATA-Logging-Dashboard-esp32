# Passive automotive CAN candidate analyzer

This implementation is integrated into the existing PlatformIO dashboard project. It contains no manufacturer database, vehicle-specific identifiers, DBC, signal names, assumed ECU count, scaling, or physical units. The separate bench sketch uses explicitly synthetic test identifiers.

The analyzer installs TWAI in **LISTEN_ONLY**, disables the transmit queue, accepts all identifiers, and has no transmit API call, transmit command, diagnostic request, or active-mode switch. START resumes software acquisition; it does not change the controller mode. STOP pauses acquisition while the controller remains listen-only and the RX task drains the driver.

Espressif documents listen-only mode as disabling message, ACK, and error-frame transmission. Classical ESP32 TWAI does not support CAN FD. See [Espressif TWAI documentation](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/peripherals/twai.html#operating-modes) and its overview. A bitrate mismatch cannot be diagnosed as an empty bus from silence alone.

## Project integration and build

Open this existing PlatformIO project and run `pio run`. The verified environment is espressif32 6.10.0, Arduino ESP32 2.0.17, ESP32 Dev Module; existing dependencies and Wi-Fi settings remain in place. Firmware is `.pio/build/esp32dev/firmware.bin`. The web page is gzip-compressed into PROGMEM by the existing build script. No filesystem upload or new runtime library is needed.

| File | Purpose |
|---|---|
| `src/main.cpp` | Existing sensors, WebServer, shared SD mutex, console integration |
| `src/can/Config.h` | CAN wiring, bitrate, memory limits |
| `src/can/CanService.h/.cpp` | RX, analysis, SD, reporting tasks; Serial and HTTP bridge |
| `src/can/Analysis.h/.cpp` | Portable statistics, fields, counter and checksum candidates |
| `dashboard/can.js` | CAN lifecycle and lightweight Encoder/IMU tabs |
| `dashboard/index.html`, `style.css`, `app.js` | Dashboard layout, rendering and polling |
| `tests/can/test_analysis.cpp` | Actual C++ analysis tests, assertions enabled even in optimized builds |
| `dashboard/can.test.cjs` | CAN input and bounds tests |
| `scripts/analyze_can_csv.py` | Offline checksum hypotheses and validation split |
| `examples/can_bench_generator` | Separate guarded test transmitter; never compiled into dashboard firmware |

**CAN defaults are TX GPIO25, RX GPIO26, 500 kbit/s. Verify these against the external transceiver before connecting.** GPIO21/22 already serve I2C and must not be reused for CAN. Existing Encoder GPIO32/33, MPU6050/MPU6500 handling, encoder geometry settings, SD SPI18/19/23/5, and saved sensor settings are reused. Supported compile-time bitrates are 50/100/125/250/500/1000 kbit/s. Discovery requires the correct bitrate; automatic bitrate scanning is not implemented.

## Browser modes and automatic CSV

- **Dashboard** preserves existing instruments and settings.
- **CAN + CSV** automatically queues START and LOG START. Wait for `RECORDING` and a filename before assuming capture to SD has started. SD errors remain visible. A header-only CSV is created even when no frames arrive.
- Leaving CAN queues LOG STOP. The file is drained and closed asynchronously; wait for the close message before removing the card. Closing a browser or losing Wi-Fi does not reliably send a mode-exit request: the ESP32 continues logging until explicitly stopped.
- CAN mode temporarily gives raw CAN logging exclusive SD ownership; regular sensor SD capture is paused. Existing sensor readings continue. The old browser recording flag is suppressed during CAN mode. On leaving CAN, ordinary sensor recording can resume if the physical START token is still set.
- **Encoder** shows speed, shaft/wheel RPM and counts using the existing calibration settings. **IMU** shows acceleration and roll/pitch/relative yaw using the existing verified IMU driver. The compact tabs do not run a 3D scene or draw a graph. Acceleration includes gravity; IMU is not used to invent a drift-free vehicle speed.
- CAN snapshots poll once per second. Compact sensor views poll the existing state endpoint once per second; Dashboard retains its normal faster view. No high-rate raw CAN frame list accumulates in browser memory. On-demand reports retain only their last 4 KiB.
- This is one device-wide logging mode. Multiple browser clients can change the same mode; use one operator during capture. CSV files are stored on SD, not downloaded by the browser. Existing browser CSV files remain in their original sensor format.
- The local/public demo uses no actual CAN traffic or SD files and says so explicitly. Synthetic sensor values in the pre-existing demo are presentation data, not hardware validation.

Serial CAN lines can use `:STATUS`, `:ID 321`, etc. The colon prefix avoids conflicts with existing single-letter sensor controls. Uppercase CAN commands are also accepted as full lines. CAN commands work even with the original IP-only console option; ordinary legacy controls keep their existing behavior. Web reports and Serial reports share a bounded worker queue.

The transceiver must match the actual physical layer and ESP32 logic levels. The firmware does not guess a transceiver's standby/silent pin polarity. For a hardware-enforced passive probe, use a documented transceiver silent mode or isolate its TXD drive and hold TXD recessive according to its datasheet. Firmware listen-only begins after TWAI initialization; reset, boot, power, wiring faults, and transceiver behavior need hardware consideration.

## Architecture

```text
Transceiver RX -> TWAI hardware / driver RX queue (128)
                 -> RX task, priority 20, core 0
                    |-> analysis queue (128), send with zero wait
                    |   -> statistics task, priority 4
                    |      -> bounded Record database
                    |
                    |-> logging queue (256), send with zero wait
                        -> SD task, priority 2 -> 4 KiB CSV buffer -> SD

Serial -> existing console/UI owner -> control queue -> statistics task
                            -> logging control queue -> SD task
                            -> report queue -> candidate/report task, priority 1
                                                | short per-ID database snapshot
                                                | analysis outside database lock
                                                -> bounded text queue -> UI -> Serial
```

On dual-core ESP32, workers run on core 1. A single-core build maps all workers to core 0. Only the RX task calls TWAI receive/status. CAN logging owns the shared storage mutex for its file lifetime; the legacy SD writer takes the same mutex per record. These are the only SD writers and cannot perform disk operations concurrently. After startup, the existing console task owns Serial; CAN workers send it bounded output chunks. The RX task never takes the database lock, waits for SD, or waits for a full queue. Its logger gate uses a zero-wait try-lock; contention is counted as a logger drop. No runtime signal-discovery allocations occur.

The statistics task owns database mutations and capture control. Reports copy one record under a mutex, release it, then compute and format results. A long report can wait on the bounded output queue without stopping CAN reception or streaming analysis. The console drains a bounded number of output chunks per loop and continues reading commands. Capture/reset requests during SUMMARY are rejected with a retry message, preserving the comparison. IDS and individual ID reports are live per-record snapshots, not a simultaneous whole-bus snapshot.

## Data and capacity

Records are keyed by **(identifier, standard/extended, data/RTR)**. This distinguishes standard and extended frames having the same numeric ID and avoids mixing remote frames with payload statistics. A key identifies a traffic stream, not an ECU. Multiple ECUs can contribute traffic, and one ECU can use many IDs.

The default database holds **16 stream records**. Records are allocated individually at startup, avoiding a requirement for one large contiguous heap block. There is no allocation in the receive or per-frame analysis paths. Startup halts on allocation/task/driver failure; it never falls back to an active CAN mode.

When full, existing records continue updating. Untracked traffic still contributes to aggregate statistics and independent SD logging. `database_full_frames` counts frames that could not receive a record; it does not claim to count distinct rejected IDs. There is no silent eviction. Increase `MAX_IDS` only after checking measured free heap, or log the entire bus and perform offline discovery. RESET clears the database and captures, preserving lifetime loss/error/log counters.

Each record contains:

- Identifier, extended/RTR flags, observed DLC bit mask, frame count, first/last microsecond timestamp.
- Period count, minimum/maximum period, running mean period; estimated frequency is `1e6 / mean_period_us`.
- Eight ByteStats: sample count, extrema, Welford mean/M2, adjacent comparison count, change count, accumulated XOR mask, and eight bit-toggle counts.
- The most recent 32 data-frame samples for discovery and counter checking.
- Baseline and action windows: full-stream byte moments, observed bit sets, frame count, and up to 32 time-binned payload samples per window.

RTR frames contribute timing/count/DLC statistics, never byte or signal samples. DLC 0 is valid. Payload bytes outside DLC are absent, not zero observations. Non-Classical DLC values are rejected and counted.

## Algorithms and interpretation

### Discovery and bus traffic

All accepted standard and extended frames are examined. Timing uses `esp_timer_get_time()` when the RX task dequeues a frame. These are monotonic software timestamps, **not hardware start-of-frame timestamps**. Driver backlog and scheduling jitter can distort short periods; min/max period is especially sensitive. Do not infer ECU timing precision from them.

Known acquisition breaks/analysis drops increment a gap epoch. Period and byte transition calculations do not cross a known gap. Timing can cross DLC changes for the same traffic stream; payload transition calculations do not. Driver loss counters are polled every 100 ms, so exact loss boundaries are unavailable and some contaminated transitions can precede detection. Always inspect loss counters before trusting fine-grained results.

STATUS reports observed frame rate, payload bytes, bus-error/driver-loss counters, queue occupancy/high-water marks, and an **unstuffed observed bus-load estimate**. The estimate counts 47 bits per standard or 67 bits per extended frame including intermission, plus 8 bits per data byte. RTR has no data bits. It excludes bit stuffing, errors, overload frames, retransmission details, and missed traffic; it is not a precise utilization measurement. Its elapsed denominator excludes software STOP time. Recently queued frames can cause small temporary reporting discrepancies.

### Bytes and bits

Welford's update is `n += 1; delta = x - mean; mean += delta/n; M2 += delta*(x - mean)`. Reported sample variance is `M2/(n-1)`. Change probability is `changes / valid_adjacent_comparisons`. XOR masks OR together `current XOR previous`; bit-toggle probability uses the same valid comparison denominator.

Toggle frequency is observed toggles divided by the record's first-to-last wall time, so pauses and missing observations bias it downward. Per-bit toggle counters saturate at UINT32_MAX; most diagnostic telemetry counters wrap modulo 2^32. Frame counts and statistical sample counts are 64-bit.

At least 16 byte observations are required for classification. Equal extrema mean **observed constant**, not universally constant. Change probability below 0.05 is labelled slowly changing, above 0.5 rapidly changing; those thresholds are descriptive heuristics. Every observed toggling bit is a binary/state boundary candidate and might instead belong to a packed number, counter, or checksum. Smooth raw values are possible analog/continuously varying candidates; physical units, signedness, and meaning remain unknown.

### Signal boundaries and endianness

`ID <hex>` searches every valid start offset 0..63 for widths **1, 8, 16, 24, 32**. Counter searches additionally include **2 and 4**. Both Intel and Motorola interpretations are tested, including non-byte-aligned fields; duplicate one-bit endian searches are omitted. There are 672 valid field hypotheses in the combined search, rather than a permanently allocated statistics matrix for each one.

Bit numbering is LSB0 within each byte. Intel's start is the least-significant bit and traversal increments the bit index. Motorola's start is the most-significant bit; traversal decrements within a byte, jumping from bit 0 of one byte to bit 7 of the next (`p = p%8 ? p-1 : p+15`). Thus bytes 1..2 correspond to Intel start 8/width 16 or Motorola start 15/width 16. A Motorola start/width should not be printed as a misleading contiguous numeric bit range.

Unsigned raw extraction never reads outside DLC. Candidate evidence includes sample range, number of changes, activity fraction, positive/negative deltas, smoothness, and Pearson correlation with sample time. Smoothness is `max(0, 1 - mean_absolute_step / observed_range)`. Continuously varying candidate confidence is smoothness times `min(1, changes/8)`; only scores >=0.5 with >=16 observations are reported, up to eight candidates. This is a heuristic score, not a calibrated probability.

Overlapping fields, subfields, and both endian interpretations can score well. The output deliberately preserves alternatives. It does not resolve multiplexing, signed values, scaling, physical meanings, or independent adjacent signals. Fast random data and short ramps can produce false candidates. Wider fields containing a counter can look smooth. This is evidence for a human, not automatic DBC truth.

### Rolling counters

For widths 2, 4, 8, and 16, each valid consecutive transition is checked against `next == (previous + 1) mod 2^width`. Matching fraction is reported when >=0.8, with at least 15 comparisons. The report includes the match numerator/denominator and observed wrap count. With no wrap observed, the modulus is explicitly **not demonstrated**. The recent 32-frame ring is consecutive data for the key, except known gaps or DLC transitions, which are excluded.

Time-binned experiment samples are never used for modulo-counter detection because they deliberately skip frames. Multiplexed counters, increment-by-two counters, non-power-of-two moduli, packet loss, and repeated values can reduce a true counter's score. A smooth ramp can match a counter hypothesis temporarily. Repeated captures and offline analysis are necessary for confirmation.

### Checksum, CRC, parity candidates

For each payload byte, a bounded pass checks additive sum of the other bytes modulo 256, its two's complement, and XOR of the other bytes. It also measures how many bits of the proposed checksum byte change when other payload bytes change. A changing field with a moderate changed-bit fraction is a possible checksum/CRC location, **not evidence of a particular polynomial**. >=16 samples and >=4 candidate changes are required before reporting.

Invariant even/odd payload parity is reported as a payload-level parity candidate. It cannot localize the parity bit and can result from an XOR checksum. The firmware does no CRC polynomial brute force and does not claim knowledge of checksum coverage, ID inclusion, seeds, salts, final XOR, reflection, or hidden state.

Offline example, using the bench fixture ID only:

```text
python scripts/analyze_can_csv.py can_000001.csv --id 321 --extended 0 --dlc 8
python scripts/analyze_can_csv.py can_000001.csv --id 321 --crc8-poly 0x1D --init 0xFF --xorout 0xFF
```

The second command tests an explicitly supplied CRC hypothesis; it is not asserted to describe any vehicle. The script separates chronological discovery and validation halves, requires at least 32 matching DATA frames, bounds matching rows, and preserves standard/extended/DLC distinctions. Reflected polynomials require `--reflect` and the reflected representation. Coverage is the remaining payload in wire order; more complex coverage needs extension.

### Guided and differential learning

```text
BASELINE 10
EXPERIMENT THROTTLE 10
SUMMARY
```

Wait for the baseline completion message, then start the experiment and wait for its recording message before operating the control. `THROTTLE` is a user-supplied label, never a decoded meaning. SUMMARY runs automatically after action completion and can be repeated. A new baseline invalidates the old action; a new action can reuse a completed baseline. Overlapping captures are rejected. STOP cancels an in-progress capture.

Equivalent differential commands:

```text
CAPTURE BASELINE
CAPTURE ACTION
```

These each record 10 seconds by default, or accept a duration argument. Durations are integers from 1 to 3600 seconds. They are timed captures, not indefinite toggles. Capture membership uses reception timestamps and excludes the exact end timestamp. Queued pre-deadline frames are drained before completion on an idle bus. Control commands are asynchronous; wait for their response. Frames still queued when STOP executes are deliberately discarded from analysis.

Full-stream byte moments and rates are compared. Signal samples retain the latest frame per time bin across the whole window, not just the last 32 frames. Reports show newly observed IDs, IDs absent from action, per-ID rate changes, changed byte moments, changes in observed bit sets, sampled bit-one probabilities, per-ID candidate ranking, and a global top-12 signal list.

Each signal score combines positive variance gain (30%), balanced phase association (25%), sampled activity gain (15%), positive range gain (20%), and monotonicity weighted by evidence of change (10%). Evidence is reduced when either window has fewer than 16 signal samples and requires at least four samples in each window. Held-state mean shifts are retained even if both windows have zero variance. ID ranking uses the best signal score; newly observed/absent IDs are listed separately because they lack two-sided evidence.

Phase association is `abs(mean_action-mean_baseline) / sqrt(delta_mean^2 + 2*(var_action+var_baseline) + 1)`, giving both phases equal weight. It is a regularized phase-association metric, not correlation with a measured pedal/steering/reference trace. `action_time_r` is Pearson correlation with elapsed time. Monotonicity is the dominant nonzero delta direction's fraction. Background ramps unchanged between phases cannot rank on monotonicity alone.

Short events may be missed by time-bin sampling, and sampling can alias activity. Byte moments use all analyzed frames, whereas signal moments/ranking use only retained samples. No fixed sample budget can preserve arbitrary-duration high-rate signal histories exactly. Negative variance/activity/range changes are shown in byte/rate diagnostics but are not positively rewarded by the action-increase score. Repeat baseline/action cycles and use CSV plus an external reference to establish stronger evidence.

## Serial commands

| Command | Effect |
|---|---|
| `HELP` | Command syntax |
| `STATUS` | Acquisition, capture, memory, traffic, logging and loss status |
| `IDS` | All tracked traffic keys and timing summaries |
| `ID 321` | Details for numeric hexadecimal ID, all observed formats |
| `ID 321 STD` / `ID 321 EXT` | Restrict format; RTR/DATA remain distinguished |
| `START` / `STOP` | Resume/pause software acquisition, always listen-only |
| `BASELINE 10` | Record a new baseline |
| `EXPERIMENT LABEL 10` | Record action against baseline; label <=23 characters without spaces |
| `CAPTURE BASELINE [seconds]` | Timed baseline, default 10 s |
| `CAPTURE ACTION [seconds]` | Timed action and automatic comparison |
| `SUMMARY` | Recompute candidate comparison after both captures |
| `RESET` | Clear records/captures and aggregate analysis statistics |
| `LOG START` / `LOG STOP` | Open a new SD session / drain and close files |

Commands are case-insensitive and bounded to 79 input characters. Overlong lines are entirely discarded. Queue-full conditions return a retry response. SD sessions are independent of RESET. STOP acquisition does not close an open log; use LOG STOP to drain/close before removal.

## Asynchronous logging and loss

CSV header is exactly:

```csv
timestamp_us,id,extended,dlc,d0,d1,d2,d3,d4,d5,d6,d7
```

IDs are uppercase hexadecimal without `0x`; timestamps and payload bytes are decimal. Missing data bytes are empty cells. Remote-frame rows also have empty payload cells, with explicit RTR records in the companion `.meta` file to preserve the requested CSV schema. DLC-0 DATA/RTR distinctions require that metadata. Files use unique sequential names `/can_000001.csv`, `/can_000001.meta`, etc.; existing files are not overwritten.

The RX task independently queues logging frames, so database-full frames can still be logged and analysis overload need not imply logging loss. SD writes use a 4096-byte buffer, with flush attempts at one-second intervals and on LOG STOP. STOP first disables producer admission under the logger gate, then drains the queue; late producers cannot race the close. CSV and metadata write return values are checked. Errors disable logging and are reported; queued leftovers are counted as discarded. `buffered_or_written_rows` is not a durable-row count. Short writes, metadata errors, or power failure can leave incomplete data; discarded/unconfirmed-row counts are conservative.

STATUS distinguishes driver missed frames, hardware overruns, analysis queue drops, logger queue/gate drops, database-full frames, invalid frames, bus errors, and SD write errors. Driver counters can overlap and their sum is not necessarily a unique lost-frame count. Experiment loss totals are indicators of contamination, not exact window-local lost-frame counts. The driver provides no exact per-frame timestamps of loss. SD flush has no reliable end-to-end durability result in this Arduino interface. Use clean LOG STOP, preserve metadata, and inspect the last CSV row after interruption.

At 10,000 frames/s, the 128-frame analysis queue covers about 12.8 ms and the 256-frame logging queue covers about 25.6 ms. SD cards can stall much longer. Asynchronous means reception is decoupled from writes; it cannot guarantee lossless storage at every bus load. No error counter can count traffic never successfully received at a wrong bitrate or with an incompatible physical layer.

## RAM and CPU budget

The actual portable layout test gives Frame=32 bytes, Sample=24 bytes, Record=3696 bytes. The default 16-record database consumes **59,136 bytes** plus allocation overhead. Records are allocated once, individually, to avoid a large contiguous heap requirement.

| Additional CAN allocation | Default |
|---|---:|
| Record payloads | 59,136 B |
| RX analysis queue (128 x 32 B) | 4,096 B |
| SD logging queue (256 x 32 B) | 8,192 B |
| CAN task stacks: RX / statistics / SD / reports | 3,072 / 4,096 / 4,096 / 8,192 B |
| Driver queue, command/notice/output queues | Several KiB, bounded |
| Report cache and SD write buffer | 4,096 B each, static |
| HTTP response scratch | 6,144 B, reused for JSON/text, static |
| Whole-board static RAM / flash | See `docs/can-validation.md` for final build numbers |

The build's static-RAM percentage does **not** include runtime CAN records, queues, task stacks, existing sensor allocations, Wi-Fi, or SD runtime allocations. On this already sensor-heavy board, measure free/minimum heap and stack headroom on hardware before increasing capacity. No promise of zero frame loss or zero jitter is made without sustained-load testing. Reduce MAX_IDS if headroom is insufficient. Every removed record saves about 3.6 KiB; SD remains able to capture IDs that do not fit the database.

Per-frame work is bounded by a linear lookup over at most 16 keys plus eight byte updates. Double-precision Welford operations have software cost on ESP32. Report searches are up to 672 fields x 32 samples per ID; experiment comparison searches 424 fields x two windows. They run only on request/completion, outside the reception path, and yield between width/endian groups. The statistics task yields after each batch of 32 frames so the lower-priority workers can run; throughput depends on tick frequency and actual workload.

STATUS exposes cumulative task busy-microsecond counters modulo 2^32 and queue high-water marks. Differences across a measured interval approximate RX and streaming-analysis task wall-time consumption, including preemption/lock waiting; they are not exact CPU-cycle utilization and do not measure all worker CPU use. No hardware throughput figure is claimed. Benchmark full bus load with simultaneous reporting and slow-card stalls, then tune queue lengths, record capacity, and priorities from measured losses. Consider a bounded hash table if lookup CPU becomes material.

## Two-ESP32 bench procedure

1. Use an isolated Classical CAN bench with two compatible external transceivers. Set both boards to the same bitrate. Wire CANH-to-CANH, CANL-to-CANL, and the appropriate common reference. Terminate the two bus ends with 120 ohms each; unpowered resistance should be about 60 ohms across H/L. Keep the stub wiring short.
2. Flash the dashboard firmware only to board A. Inspect STATUS, confirm LISTEN_ONLY, and verify all configured pins. If possible, observe A's TXD with a logic analyzer: it must stay recessive during traffic and malformed-frame tests.
3. Flash the separate `examples/can_bench_generator` sketch to board B. **Never attach board B with this sketch to a vehicle.** Ground B's GPIO27 and send `ENABLE BENCH` to its Serial port. The generator uses NO_ACK and single-shot frames because A must not ACK. The analyzer itself never uses NO_ACK.
4. Run `:IDS` on A, or open the CAN tab. Expect synthetic standard and extended 0x321 as separate keys; 0x322 with a little-endian 16-bit counter; 0x345 with changing DLC; and 0x456 as RTR. There is no vehicle meaning attached to these IDs.
5. Run `ID 321 STD`. Check low two-bit and high-nibble counters in byte 0, byte-6 rolling count, constant/ramp bytes, and byte-7 additive-checksum candidate. Short windows can admit wider overlapping counter candidates; wrap counts distinguish demonstrated moduli.
6. Leave B at `ACTION OFF`; record `BASELINE 10` on A. After baseline completion, send `ACTION ON` to B and `EXPERIMENT BENCH_ACTION 10` to A. B varies bytes 1..2 as a little-endian value and bytes 3..5 as a big-endian value. A should report varying candidates around those fields and new synthetic ID 0x700. Repeat with B set back to ACTION OFF. Compare rankings across repeated trials rather than expecting a uniquely correct boundary from one run.
7. Test `LOG START`, capture, and `LOG STOP`; inspect CSV header/row lengths, decimal bytes, hexadecimal IDs, metadata RTR rows, unique filenames, and clean close. Run the offline sum check against 0x321.
8. Increase the generator rate and burstiness on the isolated bench. Run ID/SUMMARY reports concurrently and exercise a slow SD card. Record queue peaks and all loss counters. The RX task should continue even when SD fails; loss must be visible. Unplugging the card may corrupt it, so use a disposable test card if testing removal faults.
9. Exercise no traffic, DLC 0..8, coincident STD/EXT IDs, counter wraps, pauses, RESET, invalid/overlong commands, overlapping captures, and allocation/database-full conditions. To force full capacity, temporarily use MAX_IDS=2 or expand the bench's synthetic ID stream. Verify existing records continue and all untracked frames remain eligible for logging.
10. On actual hardware, measure minimum free heap, stack headroom, maximum sustained frame rate, error counts, and stability over hours. Build success and host tests do not establish these hardware properties.

## Vehicle passive-capture procedure

1. Confirm the network's physical layer and that it is Classical CAN, identify its bitrate, and verify board/transceiver voltage levels, power protection, RX/TX connections, and ground/reference strategy. Use an isolated interface where the installation requires it. Do not power an ESP32 directly from vehicle battery voltage.
2. Disconnect the bench generator. Use only the analyzer firmware. Prefer hardware silent enforcement for the probe, and verify recessive TXD during boot/reset with the actual transceiver. Software cannot protect against incorrect wiring or a failed transceiver.
3. With the vehicle parked safely, connect through a suitable breakout. Do not add another 120-ohm terminator to an already terminated vehicle bus. Keep the probe stub short. Use existing awake traffic; the analyzer cannot wake or query sleeping ECUs.
4. Confirm STATUS says LISTEN_ONLY and that useful traffic is present at the selected bitrate. Watch errors and loss counters. No traffic does not prove the bus is absent: it may be asleep, a different bitrate, an inaccessible gateway segment, or CAN FD.
5. Start SD logging and record a stationary baseline. Label one controlled action at a time; use engine-off/ignition-on experiments where feasible. Avoid driving or operating safety-critical controls to satisfy an experiment. If a moving test is necessary, arrange a separate safe test process and operator rather than interacting with Serial while driving.
6. Repeat baseline/action sequences, record independent observations/timing, and preserve raw CSV plus metadata. Treat rankings as candidates. IDs do not enumerate ECUs, and a changing field need not encode the labelled control.
7. LOG STOP, wait for the drain/close response, inspect errors, then power down and remove the probe. Decode offline, validate across multiple sessions, and assign physical meanings only with external evidence.

## Future improvements

- Time-aligned external reference traces for actual value correlation, lag estimation, repeated experiments, and permutation/null controls.
- Multiplexer discovery, signed/offset encodings, scaling hypotheses, boundary overlap reduction, and stronger held-out validation.
- Offline counter models with missing-frame inference, non-power-of-two moduli, counter/checksum coverage, ID/seed/salt inclusion, and bounded CRC parameter searches.
- Hardware timestamps or a dedicated capture interface for timing precision; CAN FD hardware for FD networks.
- Larger PSRAM-backed histories, preallocated/binary SD logs, loss-event records, configurable capture selection, and a bounded hash table.
- DBC candidate export with explicit confidence/provenance, never unverified physical labels.
- Device-specific silent-pin support after the actual transceiver and wiring are known, plus hardware-in-loop regression and sustained-load profiling.
