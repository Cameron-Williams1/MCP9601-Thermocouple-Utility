# Thermocouple Date Logger

Records up to six K-type thermocouple channels, read by Adafruit MCP9601 amplifiers, from an Adafruit Feather RP2040 Adalogger to a laptop over USB, for static fire testing. Each recording is saved as a CSV file with one row per measurement event, and a saved file can be graphed without a board connected.

## Hardware

- Adafruit Feather RP2040 Adalogger
- Up to six Adafruit MCP9601 (MCP96L01) I2C Thermocouple Amplifiers (STEMMA QT / Qwiic) on the I2C chain, each set to its own address
- K-type thermocouple probes
- USB data cable
- A computer with Python

The I2C chain uses SDA on GPIO2 and SCL on GPIO3. No SD card is needed for USB recording.

## Quick start

1. Connect the Feather by USB. Close Arduino Serial Monitor or any other program using its serial port.
2. Set up and start the recorder:

   ```
   python -m venv .venv
   source .venv/bin/activate        # Windows: .venv\Scripts\activate
   pip install -r Computer/requirements.txt
   python Computer/static_fire_recorder.py
   ```

3. Check the row for each amplifier address. A connected probe should show a plausible temperature and `OK`. An unused input can show `No thermocouple`.
4. Click **Start recording** and confirm the row count increases.
5. Click **Stop and save** when the run is finished.

Each recording creates a new `StaticFire_…csv` file in the current user's Downloads folder, so make sure that folder exists. A preview alone does not create a file. Keep the computer awake and the USB cable connected for the whole run.

## Graphing a saved run

In the application window, choose **CSV Graph → Open CSV…** and select a StaticFire USB or FLIGHT SD recording. The graph shows elapsed seconds against temperature in °C. Unplugged or faulted probes leave gaps. No Feather needs to be connected to review a file.

## CSV format

Each row is one event from one amplifier address. Channels update independently, so adjacent rows are not simultaneous. Use the board timestamps to align channels. Repeated temperatures can be separate conversions. A fault event has no temperature.

| Field | Meaning |
| --- | --- |
| `host_utc` | UTC time when the laptop processed the event. |
| `board_time_us` | Microseconds since board startup when the firmware reported the result. Use differences for elapsed time. |
| `sequence` | Board event counter across all channels. |
| `address` | Amplifier address, one of up to six. |
| `status` | `OK`, `OPEN` (no probe or open circuit), `SHORT` or `I2C_ERROR`. |
| `temperature_C` | Probe temperature in Celsius for valid readings, otherwise blank. |
| `missing_usb_events_before` | Gap in the board event sequence since the last received row. A board reset restarts the counter, so start a new run after a reset. |

## Performance

The firmware uses the fastest 12-bit conversion setting, which trades temperature resolution for speed. Bench measurements gave approximately 36 to 40 valid readings per second per connected probe. The window updates once per second, while the CSV keeps every individual event, including faults. The displayed event rate includes fault reports.

## Firmware

The Feather ships configured. To restore the USB recorder on the same board model:

1. Hold **BOOTSEL**, tap **RESET**, then release BOOTSEL.
2. Copy `Board/USB/StaticFireUSB.uf2` to the `RPI-RP2` drive.
3. The board restarts automatically.

To rebuild the firmware from source, open `Board/USB/StaticFireUSB/StaticFireUSB.ino` in the Arduino IDE with Arduino-Pico 6.2.0 and select **Adafruit Feather RP2040 Adalogger**.

### Other board modes

- `Board/Optional SD Logger` holds the earlier standalone SD firmware.
- `Board/Optional CircuitPython Monitor` holds the earlier CircuitPython program and its libraries. To use it, install its CircuitPython UF2 in BOOTSEL mode, then copy only `code.py` and `lib` to the `CIRCUITPY` drive. Run `desktop_monitor.py` on the computer with the packages from `Computer/requirements.txt`.

These modes replace the USB firmware and use a different serial format. Restore `StaticFireUSB.uf2` to use the fast recorder.

## Repository layout

```
Board/
  USB/
    StaticFireUSB.uf2              prebuilt USB firmware
    StaticFireUSB/StaticFireUSB.ino
  Optional SD Logger/              earlier standalone SD firmware
  Optional CircuitPython Monitor/  earlier slower monitor
Computer/
  static_fire_recorder.py          desktop recorder
  requirements.txt
```

## Troubleshooting

| What you see | What to check |
| --- | --- |
| Feather not connected | Use a USB data cable, confirm the correct firmware is installed, close other serial tools, then click Reconnect. |
| No thermocouple / `SHORT` | Check the two probe leads at that address. An open input is expected on unused channels. |
| No recent data | Check USB power and the STEMMA chain. If USB disconnected during a recording, reconnect and start a new file. |
| File error | Check free disk space and that your system allows the program to write to the Downloads folder. |

Copyright © 2026 Cameron Williams
