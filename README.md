# DA14706 BLE Energy Monitor — Sensor Node

BLE peripheral firmware for the **Renesas DA14706** (Cortex-M33, DA1470x family). Measures AC mains voltage, current, active power, and line frequency from a home appliance, reads ambient temperature and humidity from an AHT20 over I2C, and streams all measurements to a BLE central every second. A relay output allows remote load switching.

---

## Hardware

| Component | Part | Interface | Pin(s) |
|---|---|---|---|
| MCU | Renesas DA14706 (Cortex-M33, 32 MHz) | — | — |
| Voltage sensor | BEL DPC12 step-down transformer | GPADC CH1 | P0_6 |
| Current sensor | LEM HLSR 10P Hall sensor (80 mV/A) | GPADC CH0 | P0_5 |
| Temp / Humidity | AHT20 | I2C — MikroBUS 1 | SDA = P1_11, SCL = P1_12 |
| Relay | 1-channel relay board (250 V AC / 10 A) | GPIO — MikroBUS 2 | P1_00 (active-high) |

Both analog sensors feed signal-conditioning circuits that scale and level-shift the mains signals into the GPADC input range. One conditioning stage inverts the channel polarity, compensated by `P_SIGN = -1` in software.

> **Relay power supply**: the relay coil draws ~70–100 mA. During development power its VCC from a bench supply. For a standalone deployment use an LM7805 (5 V, ≥200 mA) fed from a 12V DC power supply.

---

## Software Stack

| Tool | Version |
|---|---|
| SmartSnippets Studio | 2.0.18+ |
| DA1470x SDK | 10.2.6.49 |
| SEGGER J-Link | Latest |

---

## Project Structure

```
DA14706_ble_sensor_node/
├── config/
│   ├── ble_peripheral_config.h      # Feature flags (CFG_MY_CUSTOM_SERVICE = 1)
│   ├── custom_config_ram.h          # Build config — RAM execution (debug)
│   ├── custom_config_oqspi.h        # Build config — OQSPI flash (production)
│   ├── peripheral_setup.h           # I2C pin assignments
│   └── platform_devices.c/.h        # GPADC, I2C, relay adapter descriptors
├── drivers/aht20/
│   ├── driver_aht20.c/.h            # AHT20 libdriver
│   └── driver_aht20_interface.c/.h  # DA14706 I2C adapter glue
├── include/
│   ├── gpadc_app.h                  # gpadc_app_task() declaration
│   └── aht20_task.h                 # aht20_task_start() declaration
├── main.c                           # System init, task creation, queue creation
├── meas_packet.h                    # Shared 15-byte measurement packet definition
├── gpadc_app.c                      # AC measurement task (RMS, power, frequency)
├── aht20_task.c                     # AHT20 I2C polling task
├── ble_peripheral_task.c            # BLE GATT server, relay control, notifications
└── my_custom_service.c/.h           # Custom GATT service implementation
```

---

## Firmware Architecture

Three FreeRTOS tasks run concurrently under `pm_mode_idle`:

```
┌─────────────────────────────────────────────────────────────────┐
│  gpadc_app_task                                                 │
│  • Interleaved 2-channel ADC acquisition (BATCH_SIZE = 64)      │
│  • Per-batch: AC RMS, active power P, ZCD frequency update      │
│  • Every 1 s: scales to physical units, writes meas_packet_t    │
│    to g_meas_queue, notifies ble_peripheral_task                │
└───────────────────────────┬─────────────────────────────────────┘
                            │ MEAS_DATA_NOTIF (OS task notify)
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│  ble_peripheral_task                                            │
│  • BLE GATT server — advertising, connection, pairing           │
│  • Relay characteristic: write 0x01/0x00/0xFF → ON/OFF/TOGGLE  │
│  • On MEAS_DATA_NOTIF: dequeues packet, injects relay state,    │
│    sends GATT notification to all subscribed centrals           │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  aht20_task                                                     │
│  • Reads AHT20 via I2C every 2 s                                │
│  • Writes g_last_temp_c, g_last_hum_percent (volatile globals)  │
│  • gpadc_app_task snapshots these at each 1-second window       │
└─────────────────────────────────────────────────────────────────┘
```

`pm_mode_idle` is required because `gpadc_app_task` runs a continuous ADC loop — deep sleep would interrupt acquisition. The BLE stack operates normally in idle mode.

---

## Signal Processing (gpadc_app.c)

### Acquisition

Each loop iteration opens CH0 (current) then CH1 (voltage) alternately for `BATCH_SIZE = 64` sample pairs. A DWT cycle-counter timestamps the batch for throughput measurement. At ~478 µs per conversion the per-channel sample rate is approximately **1046 Sa/s** (~40 samples per 50 Hz cycle).

### Per-batch: AC RMS and Active Power (`compute_batch_metrics`)

Two-pass computation:
- **Pass 1** — convert raw counts to mV, compute per-channel DC offsets (`mean_v`, `mean_i`).
- **Pass 2** — accumulate squared residuals (`rv²`, `ri²`) for RMS, and the cross-product (`rv × ri`) for active power. Mean subtraction removes the DC×DC bias term from P.

RMS² and cross-product accumulators are summed across all batches in the 1-second window, then square-rooted and scaled to physical units.

### Per-batch: Frequency via Interpolated ZCD (`batch_zcd_update`)

Single-pass zero-crossing detector with sub-sample interpolation:

1. **EMA DC tracking** (`ZCD_DC_ALPHA = 0.01`, ~100-sample time constant) — updates every sample, tracking the mid-rail DC offset without batch-boundary drift.
2. **Hysteresis arming** (`ZCD_HYST_MV = 50 mV`) — the detector arms only after the AC residual dips below −50 mV, preventing false triggers from noise near zero.
3. **Interpolated crossing** — when the armed detector sees a residual sign change (negative → positive), the exact zero-crossing is placed at a fractional sample index using linear interpolation between the two bracketing samples:

```
alpha   = |rv[k-1]| / (rv[k] - rv[k-1])
t_cross = total_samples + (k - 1) + alpha
```

### Every Second: Frequency Derivation

```
freq = (N_crossings - 1) / ((t_last_cross - t_first_cross) / total_samples)
```

`total_samples` (the actual count of voltage-channel samples taken in the window) is used as the per-channel rate. This avoids the ×2 bias in the dual-channel `fs_acq` figure and the acquisition-only bias that excludes inter-batch processing time.

---

## BLE GATT Service

**Advertising name**: `BLE_Relay_Ctrl`

### Relay Characteristic (read / write / notify)

| Write value | Action |
|---|---|
| `0x01` | Relay ON |
| `0x00` | Relay OFF |
| `0xFF` | Toggle current state |

Any other value returns `ATT_ERROR_APPLICATION_ERROR`. All subscribed clients receive a notification whenever the relay state changes.

### Measurement Characteristic (notify — 1 Hz)

Subscribe by writing `0x0001` to the CCC descriptor (UUID `0x2902`). Packets arrive once per second.

---

## Measurement Packet Format

`meas_packet_t` — **15 bytes**, packed, little-endian:

| Offset | Size | Type | Field | Unit | Example raw | Decoded |
|---|---|---|---|---|---|---|
| 0 | 2 | int16 | `v_rms` | centi-V | 23041 | 230.41 V |
| 2 | 2 | int16 | `i_rms` | milli-A | 1500 | 1.500 A |
| 4 | 4 | int32 | `p_w` | centi-W | 34567 | 345.67 W |
| 8 | 2 | int16 | `freq` | centi-Hz | 5002 | 50.02 Hz |
| 10 | 2 | int16 | `temp` | centi-°C | 2584 | 25.84 °C |
| 12 | 2 | uint16 | `humid` | centi-%RH | 4000 | 40.00 % |
| 14 | 1 | uint8 | `relay_state` | 0 / 1 | 1 | ON |

`freq = 0` means no voltage signal was detected (appliance under measurement is off or disconnected).

S (apparent power), Q (reactive power), and PF (power factor) are not transmitted — the central node derives them from `v_rms`, `i_rms`, and `p_w`.

Python struct format string: `"<hhihhHB"` (15 bytes).

---

## Calibration

Constants at the top of [gpadc_app.c](gpadc_app.c):

```c
#define K_V                       (283.620f)   // mains V per ADC mV  — calibrated 10/06
#define K_I                       (1.297f)     // conditioning gain on current channel — calibrated 10/06
#define HALL_SENSITIVITY_MV_PER_A (80.0f)      // LEM HLSR 10P: 80 mV/A
#define P_SIGN                    (-1.0f)      // -1 when conditioning inverts one channel
```

**To recalibrate `K_V`**: apply known mains voltage, read `*Vrms` from the serial terminal, then:
```
K_V_new = K_V_current × (V_reference / V_displayed)
```

**To recalibrate `K_I`**: apply a known AC current (or use a clamp meter reference) and adjust `K_I` by the same ratio.

---

## ZCD Tuning

```c
#define ZCD_HYST_MV   50.0f   // hysteresis: arm below -50 mV ADC, fire at zero
#define ZCD_DC_ALPHA   0.01f  // EMA coefficient (~100-sample / ~95 ms time constant)
```

- Increase `ZCD_HYST_MV` if you see spurious extra crossings on a noisy or distorted waveform.
- The EMA settles in ~100 ms after boot. The first 1-second frequency report may be slightly off; from the second window onward accuracy is typically **±0.1 Hz**.

---

## Serial Diagnostic Output

Connect a serial terminal at **115200 8N1** (`CONFIG_RETARGET` is enabled in both build configs).

```
*skew=15312 cycles (~478 us)       ← CH0→CH1 inter-sample delay, printed once at startup
*fs_acq=2092  *us_pair=956         ← ADC throughput: dual-channel Sa/s, µs per CH0+CH1 pair
*freq=50.02 Hz  xings=50           ← mains frequency and crossing count this window
*Temp=25.84 C  *Hum=40%            ← AHT20 snapshot
```

Vrms, Irms, P, S, Q, and PF serial prints are disabled — all power metrics are forwarded via the BLE packet and decoded on the central node.

---

## Build Configurations

| Configuration | Config header | Use |
|---|---|---|
| `DA14706-00-Debug_RAM` | `custom_config_ram.h` | Development — runs from RAM, exits when GDB closes |
| `DA14706-00-Release_OQSPI` | `custom_config_oqspi.h` | Production — flashed to OQSPI, persists across resets |

### Build Steps

1. **File → Import → Existing Projects into Workspace** → select this folder.
2. Select build configuration from the toolbar dropdown.
3. **Project → Build Project** (`Ctrl+B`).
4. Flash via the **Run → Debug** launcher (RAM) or the `program_oqspi_jtag` launcher (OQSPI).

> Red underlines on `g_meas_queue`, `MEAS_DATA_NOTIF`, or `meas_packet_t` are CDT indexer artefacts — the build succeeds. Clear them with **Project → Index → Rebuild**.

---

## Known Limitations / Planned Improvements

- **Q, S, PF not transmitted.** The commented-out code in `gpadc_app_task` computes them correctly for a balanced sinusoidal load but does not correct for the ~478 µs sequential-sampling skew between CH0 and CH1. At 50 Hz this introduces ~8–9° of phase error, causing a systematic PF underestimate on resistive loads. Planned fix: interpolate the voltage sample back to the instant the current sample was taken using the measured startup skew.
- **Per-batch DC mean for power.** `compute_batch_metrics` subtracts a per-batch mean from V and I before computing P. If a batch does not span an integer number of mains cycles the mean is slightly biased. A global mean across the full 1-second window would be more accurate.
- **Calibration constants are hardcoded.** No runtime calibration procedure or NVM storage is implemented yet.
- **Relay state is not persistent.** The relay always starts OFF after a power cycle.
