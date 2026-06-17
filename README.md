# Bluetooth SensorNode — Wireless AC Energy Monitor

BLE peripheral firmware for the **Renesas DA14706** (DA1470x family). Measures AC mains voltage, current, and power via a 2-channel GPADC front-end, reads ambient temperature/humidity from an AHT20 sensor over I2C, and exposes both sensor data and relay control over a custom BLE GATT service.

Merged from two working projects:
- **BLE-Relay_control** — BLE peripheral with relay control characteristic
- **i2c_and_2ch_gpadc** — GPADC 2-channel interleaved AC measurement + AHT20 I2C

---

## Hardware

### Components

| Component | Description |
|---|---|
| Renesas DA14706 | DA1470x Pro Development Kit (Cortex-M33) |
| Voltage sensing | Step-down transformer + resistive divider → P0.6 (ADC CH1) |
| Current sensing | Hall-effect sensor (80 mV/A sensitivity) → P0.5 (ADC CH0) |
| AHT20 | I2C temperature/humidity sensor |
| Soldered 333024 | 1-channel relay board (250 V AC / 10 A) |
| LM7805 | 5V regulator for relay VCC in production setup |

### GPIO Assignment

| Signal | Port/Pin | Notes |
|---|---|---|
| ADC CH0 (current) | P0.5 | Hall sensor input, `HW_GPIO_FUNC_ADC` |
| ADC CH1 (voltage) | P0.6 | Transformer/divider input, `HW_GPIO_FUNC_ADC` |
| I2C SDA (AHT20) | Configured in `platform_devices.c` | |
| I2C SCL (AHT20) | Configured in `platform_devices.c` | |
| Relay IN | P1.0 (MikroBUS 1 PWM) | `HW_GPIO_POWER_V33`, active-high |

### Relay Power Supply Notes

- **Development**: power relay VCC from a bench DC supply (5V, ≥500 mA). The devkit's MikroBUS 3.3V/5V rails cannot source the ~70–100 mA relay coil draw.
- **Production**: use an LM7805 (5V output) to power relay VCC.

---

## Software Stack

| Tool | Version |
|---|---|
| SmartSnippets Studio | 2.0.18 or higher |
| DA1470x SDK | 10.2.6.49 |
| SEGGER J-Link | Latest |

---

## Project Structure

```
Bluetooth_SensorNode/
├── config/
│   ├── ble_peripheral_config.h   # Feature flags (CFG_MY_CUSTOM_SERVICE = 1)
│   ├── custom_config_ram.h       # Build config for RAM execution (debug)
│   ├── custom_config_oqspi.h     # Build config for OQSPI flash (production)
│   ├── peripheral_setup.h        # Pin assignments
│   └── platform_devices.c/.h    # GPADC, I2C, relay adapter descriptors
├── drivers/aht20/
│   ├── driver_aht20.c/.h         # AHT20 sensor driver
│   └── driver_aht20_interface.c/.h  # DA14706 I2C adapter glue
├── include/
│   ├── gpadc_app.h               # gpadc_app_task() declaration
│   └── aht20_task.h              # aht20_task_start() declaration
├── main.c                        # System init, task creation, queue creation
├── meas_packet.h                 # meas_packet_t struct + shared queue/task handle
├── gpadc_app.c                   # GPADC acquisition task (AC RMS, power)
├── aht20_task.c                  # AHT20 I2C polling task
├── ble_peripheral_task.c         # BLE GATT server, event loop, relay + notify logic
└── my_custom_service.c/.h        # Custom GATT service implementation
```

---

## Firmware Architecture

Three FreeRTOS tasks run concurrently at `OS_TASK_PRIORITY_NORMAL`:

```
┌──────────────────────────────────────────────────────────────┐
│  gpadc_app_task                                              │
│  • Interleaved CH0/CH1 acquisition — BATCH_SIZE=64 samples  │
│  • 1-second RMS/power window, prints diagnostics to UART    │
│  • Writes meas_packet_t to g_meas_queue (xQueueOverwrite)   │
│  • Sends MEAS_DATA_NOTIF to ble_peripheral_task handle      │
└───────────────────────────┬──────────────────────────────────┘
                            │ OS_TASK_NOTIFY (MEAS_DATA_NOTIF)
                            ▼
┌──────────────────────────────────────────────────────────────┐
│  ble_peripheral_task                                         │
│  • BLE GATT server (advertising, connection management)      │
│  • Handles relay write commands on characteristic 1          │
│  • On MEAS_DATA_NOTIF: dequeues packet, injects relay_state  │
│    → mcs_notify_meas_all() → GATT notification to clients    │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│  aht20_task                                                  │
│  • Polls AHT20 over I2C every ~2 s                           │
│  • Updates g_last_temp_c, g_last_hum_percent (volatile)      │
│  • gpadc_app_task snapshots these at each 1-second window    │
└──────────────────────────────────────────────────────────────┘
```

### Sleep Mode

`pm_mode_idle` is required because `gpadc_app_task` runs a continuous ADC sampling loop. Deep sleep would interrupt the acquisition. The BLE stack operates normally in idle mode.

### Hardware Watchdog

The ADC conversion loop (~478 µs per CH0+CH1 pair) completes faster than one FreeRTOS tick (1 ms), which permanently starves the idle task and causes a hardware watchdog timeout after ~10 s. A `vTaskDelay(pdMS_TO_TICKS(1))` at the end of each 1-second measurement window forces a scheduler yield so the idle task can run and feed the watchdog.

---

## BLE GATT Service

**Service UUID**: `00000000-1111-2222-2222-333333333333`  
**Device advertisement name**: `BLE_RELAY_CTRL`

### Characteristic 1 — Relay Control

| Attribute | Value |
|---|---|
| UUID | `11111111-0000-0000-0000-111111111111` |
| Properties | Read / Write / Notify |
| Length | 1 byte |
| User Description | `Relay: 0x01=ON 0x00=OFF 0xFF=TOGGLE` |

| Write value | Action |
|---|---|
| `0x01` | Relay ON (coil energized, COM → NO) |
| `0x00` | Relay OFF (coil de-energized, COM → NC) |
| `0xFF` | Toggle current state |

Any other value returns `ATT_ERROR_APPLICATION_ERROR`. A GATT notification is sent to all subscribed clients whenever the relay state changes.

### Characteristic 2 — Measurements

| Attribute | Value |
|---|---|
| UUID | `22222222-0000-0000-0000-222222222222` |
| Properties | Notify only |
| Length | 15 bytes |
| Rate | 1 notification per second |

Write `0x0001` to the CCC descriptor (UUID `0x2902`) to enable notifications. The measurement packet is only sent to clients that have subscribed.

---

## Measurement Packet Format

15-byte packed struct, little-endian (Cortex-M33 native):

```
Offset  Size  Type     Field         Unit         Example
──────  ────  ───────  ────────────  ───────────  ────────────────
  0     2     int16    v_rms         centivolts   23045 = 230.45 V
  2     2     int16    i_rms         milliamps     1500 =   1.500 A
  4     4     int32    p_w           centiwatts  104230 = 1042.30 W
  8     2     int16    freq          centi-Hz      5000 =  50.00 Hz (placeholder)
 10     2     int16    temp          centi-°C      2584 =  25.84 °C
 12     2     uint16   humid         centi-%RH     3800 =  38.00 %RH
 14     1     uint8    relay_state   0/1          0 = OFF, 1 = ON
```

S (apparent power), Q (reactive power), and PF (power factor) are not transmitted — the central node derives them from `v_rms`, `i_rms`, and `p_w`.

`freq` is a placeholder (`5000` = 50.00 Hz). Zero-crossing detection is not yet implemented.

Python struct format string: `"<hhihhHB"` (15 bytes).

---

## Calibration (`gpadc_app.c`)

| Constant | Default | Description |
|---|---|---|
| `K_V` | `283.620f` | Mains V per ADC mV — transformer ratio × attenuator (calibrated 10/06) |
| `K_I` | `1.297f` | Signal conditioning gain on current channel (calibrated 10/06) |
| `HALL_SENSITIVITY_MV_PER_A` | `80.0f` | Hall sensor sensitivity [mV/A] |
| `P_SIGN` | `-1.0f` | Set to -1 when signal conditioning inverts one channel |
| `OFFSET_MV_CH0/1` | `0.0f` | Per-channel ADC DC offset correction [mV] |
| `GAIN_CH0/1` | `1.0f` | Per-channel ADC gain correction |

**Voltage calibration**: measure the true mains voltage with a reference instrument and adjust `K_V` proportionally:

```
K_V_new = K_V_current × (V_true / V_displayed)
```

---

## Build Configurations

| Configuration | Config header | Use case |
|---|---|---|
| `DA14706-00-Debug_RAM` | `custom_config_ram.h` | Development — executes from RAM, exits when GDB session closes |
| `DA14706-00-Release_OQSPI` | `custom_config_oqspi.h` | Production — flashed to OQSPI, persists across resets |

**For persistent standalone operation, always flash the OQSPI configuration.**

### Build Steps

1. **File → Import → Existing Projects into Workspace** → select `Bluetooth_SensorNode`
2. Select the desired build configuration from the dropdown toolbar
3. **Project → Build Project** (`Ctrl+B`)
4. Flash via the **Run → Debug** launcher (RAM) or the `program_oqspi_jtag` launcher (OQSPI)

### Eclipse CDT Indexer Notes

Red underlines on `g_meas_queue`, `MEAS_DATA_NOTIF`, or `meas_packet_t` are **indexer errors only** — the build succeeds. To clear them: **Project → Index → Rebuild**.

---

## Serial Diagnostic Output

`CONFIG_RETARGET` is defined in both config headers. Connect a serial terminal at **115200 8N1**.

Expected output at each 1-second window:

```
*skew=15312 cycles (~478 us)        ← inter-channel ADC delay (printed once at startup)
*fs_acq=2096  *us_pair=478          ← acquisition throughput
*Temp=25.84 C  *Hum=38%
```

> Vrms, Irms, P, S, Q, and PF serial prints are disabled in this version. All power metrics are computed on the Central Node from the BLE measurement packet.

---

## Testing with nRF Connect (Mobile)

1. Open **nRF Connect** → Scan → connect to **`BLE_RELAY_CTRL`**
2. Navigate to service `00000000-1111-2222-2222-333333333333`
3. **Relay control** (char `11111111-...`):
   - Tap the subscribe button to receive relay state notifications
   - Tap the write button → send `01` (ON), `00` (OFF), or `FF` (TOGGLE)
4. **Measurements** (char `22222222-...`):
   - Tap the subscribe button on the characteristic row to enable notifications
   - Measurement packets appear as hex value updates once per second

---

## Testing with the Python Central Node

A Python 3 BLE central script is at `../Bluetooth_CentralNode/central_node.py`.

```bash
pip install bleak
python ../Bluetooth_CentralNode/central_node.py
```

Expected output:

```
Connected: True
Discovering services...
Subscribed. MEAS packet is 15 bytes. Listening... Ctrl+C to stop.
Vrms=230.45 V  Irms=4.412 A  P=1042.30 W  f=50.00 Hz  T=25.84 °C  H=38.00%  Relay=OFF
```

Update `ADDRESS` in `central_node.py` if the device MAC address differs from `48:23:35:F4:00:07`.

---

## Known Limitations

- **Frequency measurement**: `freq` is a fixed placeholder (50.00 Hz). Zero-crossing detection is not yet implemented.
- **Relay state persistence**: relay defaults to OFF on each power cycle.
- **Single relay**: only one relay output is supported.
- **Floating ADC inputs**: when no AC signal is connected, the ADC channels pick up noise and report non-zero (but meaningless) RMS values.

---

## License

Copyright (C) 2015–2022 Dialog Semiconductor. All Rights Reserved.

Based on the `ble_custom_service` and `i2c_and_2ch_gpadc` samples from the DA1470x SDK 10.2.6.49.
