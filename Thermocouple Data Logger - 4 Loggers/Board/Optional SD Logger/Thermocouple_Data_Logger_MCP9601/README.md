# Thermocouple Data Logger four-channel thermal logger

Adapted from the original Invictus sketch for the Adafruit Feather RP2040
Adalogger (8 MB flash) and four MCP9601 amplifiers at 0x64–0x67.
The original source in Downloads was not changed.

## Operation

- Starts automatically when powered; no laptop required after initial setup.
- Appends a CSV sample to an onboard LittleFS flash journal every 20 ms.
- Commits the flash journal approximately every 250 ms.
- Every three seconds, closes the journal, copies it to the SD log, syncs
  the SD file, and reopens it to verify the copied bytes with an FNV-1a checksum.
- Removes the journal only after successful verification, then starts the next batch.
- Creates FLIGHT0000.CSV, FLIGHT0001.CSV, etc., without replacing existing files.
- Recovers complete committed rows from a pending journal at boot. Replays at
  the original file offset, so an interrupted SD copy does not append duplicates.
  Recovery requires the original SD card and log file. Do not swap cards with
  a pending journal.
- Stops on a write/verification failure and retains the journal. A missing or
  unsupported SD card is retried during startup; the SD card is never formatted
  automatically.

## Temperatures and timing

K-type probes, 18-bit ADC, no digital filtering. The 20 ms setting is the
poll/log interval, not the sensor conversion rate: at 18-bit resolution the
MCP9601 ADC conversion takes about 320 ms, plus calculation time. Repeated
values between conversions are expected. Storage writes can also delay polling;
timestamps are actual milliseconds since startup, and skipped 20 ms slots are counted.
This is not a guaranteed uninterrupted 50 Hz acquisition system.

CSV columns: Time_ms, TC_0x64_C, TC_0x65_C, TC_0x66_C, TC_0x67_C,
Status_0x64, Status_0x65, Status_0x66, Status_0x67.
Status is OK, OPEN, SHORT, or I2C_ERROR. A fault has a blank temperature field.

USB readings once per second remain compatible with temperature_monitor.py.
The status LED is green/orange briefly at startup, then off; storage errors blink red.

## Stop and inspect

Use a USB serial terminal at 115200 baud. Close the Python GUI before using
another serial application.

- `s`: flush remaining samples to SD and pause. Wait for "STOPPED" before powering off.
- `r`: resume logging to the same file.
- `i`: print counters, SD file size, and the first few saved CSV lines.

A reset starts a new flight file after recovering any old journal.
An unexpected loss of power may lose the most recent uncommitted flash writes;
the staging design does not guarantee zero loss or SD integrity on power failure.
Flash has finite write endurance; this design intentionally writes flash as requested.

## Build

Arduino-Pico core 6.2.0, board "Adafruit Feather RP2040 Adalogger".
Flash layout: 8 MB total, 2 MB filesystem. Library: Adafruit NeoPixel 1.15.2.
Wire, SPI, SdFat and LittleFS are supplied by the board core.

FQBN: rp2040:rp2040:adafruit_feather_adalogger:flash=8388608_2097152

On a blank onboard flash partition, the logger waits for `FORMAT` plus newline
over serial before initializing LittleFS. This command initializes onboard flash,
not the SD card. The filesystem is never reformatted automatically on mount failure.

Hardware: SDA GPIO2, SCL GPIO3; SD SPI1 on GPIO18/19/20, CS GPIO23;
NeoPixel GPIO17. The firmware replaces CircuitPython. The Python GUI still works
through USB serial, but there is no CIRCUITPY drive in Arduino mode.

References:
- https://arduino-pico.readthedocs.io/en/latest/fs.html
- https://learn.adafruit.com/adafruit-feather-rp2040-adalogger/pinouts
- https://www.microchip.com/en-us/product/mcp9601
