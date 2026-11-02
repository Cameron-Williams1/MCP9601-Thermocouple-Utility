"""Four-channel Feather temperature monitor. Requires PySide6 and pyserial."""
import re
import sys
import time

import serial
from serial.tools import list_ports
from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (
    QApplication, QComboBox, QGridLayout, QHBoxLayout, QLabel,
    QPushButton, QVBoxLayout, QWidget,
)

ADDRESSES = ("0x64", "0x65", "0x66", "0x67")


def parse_readings(line):
    result = {}
    for address, value in re.findall(r"(0x6[4-7]): ([^|\r\n]+)", line):
        value = value.strip()
        match = re.fullmatch(r"(-?\d+(?:\.\d+)?) C / (-?\d+(?:\.\d+)?) F", value)
        if match:
            result[address] = (f"{float(match[1]):.2f} °C", f"{float(match[2]):.2f} °F", "Reading")
        elif value.startswith("OPEN PROBE"):
            result[address] = ("—", "—", "No thermocouple")
        elif value.startswith("SHORT CIRCUIT"):
            result[address] = ("—", "—", "Check wiring: short circuit")
        elif value.startswith(("ERROR", "INPUT FAULT")):
            result[address] = ("—", "—", value)
    return result


class Monitor(QWidget):
    def __init__(self):
        super().__init__()
        self.connection = None
        self.buffer = b""
        self.last_reading = 0
        self.setWindowTitle("Thermocouple temperatures")
        self.resize(700, 360)
        layout = QVBoxLayout(self)
        title = QLabel("Thermocouple temperatures")
        title.setStyleSheet("font-size: 24px; font-weight: 600; margin-bottom: 10px;")
        layout.addWidget(title)
        controls = QHBoxLayout()
        self.ports = QComboBox()
        controls.addWidget(self.ports, 1)
        refresh = QPushButton("Refresh")
        refresh.clicked.connect(self.refresh_ports)
        controls.addWidget(refresh)
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.toggle_connection)
        controls.addWidget(self.connect_button)
        layout.addLayout(controls)
        grid = QGridLayout()
        grid.setVerticalSpacing(20)
        for col, text in enumerate(("I²C address", "Celsius", "Fahrenheit", "Status")):
            label = QLabel(text)
            label.setStyleSheet("font-weight: 600;")
            grid.addWidget(label, 0, col)
        self.rows = {}
        for row, address in enumerate(ADDRESSES, 1):
            grid.addWidget(QLabel(address), row, 0)
            labels = [QLabel("—"), QLabel("—"), QLabel("Disconnected")]
            for col, label in enumerate(labels, 1):
                grid.addWidget(label, row, col)
            self.rows[address] = labels
        layout.addLayout(grid)
        self.status = QLabel("Choose your Feather and connect.")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        layout.addWidget(QLabel("K-type probes • Updates once per second"))
        self.setStyleSheet("QWidget {font-size: 14px;} QPushButton, QComboBox {padding: 6px;}")
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(100)
        self.refresh_ports()
        if self.ports.count() == 1:
            self.toggle_connection()

    def refresh_ports(self):
        selected = self.ports.currentData()
        self.ports.clear()
        for port in list_ports.comports():
            if port.vid == 0x239A or "usbmodem" in port.device or "Feather" in (port.description or ""):
                self.ports.addItem(f"{port.description} — {port.device}", port.device)
        index = self.ports.findData(selected)
        if index >= 0:
            self.ports.setCurrentIndex(index)

    def clear_readings(self, status):
        for labels in self.rows.values():
            for label, value in zip(labels, ("—", "—", status)):
                label.setText(value)

    def disconnect(self, message="Disconnected"):
        if self.connection is not None:
            self.connection.close()
        self.connection = None
        self.connect_button.setText("Connect")
        self.ports.setEnabled(True)
        self.clear_readings("Disconnected")
        self.status.setText(message)

    def toggle_connection(self):
        if self.connection is not None:
            self.disconnect()
            return
        port = self.ports.currentData()
        if not port:
            self.status.setText("Plug in the Feather, then click Refresh.")
            return
        try:
            self.connection = serial.Serial(port, 115200, timeout=0)
        except (serial.SerialException, OSError) as error:
            self.status.setText(f"Could not connect. Close Arduino Serial Monitor and retry. {error}")
            return
        self.buffer = b""
        self.last_reading = time.monotonic()
        self.connect_button.setText("Disconnect")
        self.ports.setEnabled(False)
        self.clear_readings("Waiting…")
        self.status.setText("Connected — waiting for temperatures…")

    def poll(self):
        if self.connection is None:
            return
        try:
            self.buffer += self.connection.read(8192)
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                readings = parse_readings(line.decode(errors="replace"))
                if readings:
                    for address in ADDRESSES:
                        values = readings.get(address, ("—", "—", "No reading"))
                        for label, value in zip(self.rows[address], values):
                            label.setText(value)
                    self.last_reading = time.monotonic()
                    self.status.setText("Connected • Updated " + time.strftime("%H:%M:%S"))
            self.buffer = self.buffer[-16384:]
            if time.monotonic() - self.last_reading > 5:
                self.clear_readings("No recent data")
                self.status.setText("No recent data — check the USB connection and board program.")
        except (serial.SerialException, OSError) as error:
            self.disconnect(f"USB connection lost. Reconnect the board and click Refresh, then Connect. {error}")

    def closeEvent(self, event):
        self.disconnect()
        event.accept()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = Monitor()
    window.show()
    sys.exit(app.exec())
