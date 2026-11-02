"""Four MCP9601 thermocouples; USB serial readings once per second."""
import time
import board
import busio
import digitalio
import adafruit_mcp9600
from adafruit_register.i2c_bit import ROBit


class MCP9601(adafruit_mcp9600.MCP9600):
    open_circuit = ROBit(0x04, 4)
    short_circuit = ROBit(0x04, 5)

ADDRESSES = (0x64, 0x65, 0x66, 0x67)
THERMOCOUPLE_TYPE = "K"  # Change to match your probes: K, J, T, N, S, E, B, R.
i2c = busio.I2C(board.SCL, board.SDA, frequency=100000)
led = digitalio.DigitalInOut(board.LED)
led.direction = digitalio.Direction.OUTPUT
sensors = {}

print("Four thermocouples | type " + THERMOCOUPLE_TYPE + " | degrees C / F")
while True:
    readings = []
    for address in ADDRESSES:
        try:
            if address not in sensors:
                sensor = MCP9601(
                    i2c, address=address, tctype=THERMOCOUPLE_TYPE
                )
                sensor.shutdown_mode = sensor.NORMAL
                sensors[address] = sensor
                print("Connected: 0x%02X" % address)
                time.sleep(0.3)
            sensor = sensors[address]
            if sensor.open_circuit:
                readings.append("0x%02X: OPEN PROBE (check thermocouple connection)" % address)
            elif sensor.short_circuit:
                readings.append("0x%02X: SHORT CIRCUIT (check thermocouple wiring)" % address)
            else:
                celsius = sensor.temperature
                readings.append("0x%02X: %.2f C / %.2f F" %
                                (address, celsius, celsius * 1.8 + 32))
        except (OSError, RuntimeError) as error:
            sensors.pop(address, None)
            readings.append("0x%02X: ERROR %s" % (address, error))
    print(" | ".join(readings))
    led.value = not led.value
    time.sleep(1)
