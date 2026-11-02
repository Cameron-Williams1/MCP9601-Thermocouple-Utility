"""USB thermocouple recorder. Requires pyserial and PySide6."""
import csv
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
import serial
from serial.tools import list_ports
from PySide6.QtWidgets import QFileDialog, QMessageBox, QTabWidget
from PySide6.QtCore import QTimer, QEvent
from PySide6.QtWidgets import QApplication, QWidget, QVBoxLayout, QLabel, QPushButton, QTableWidget, QTableWidgetItem

base = Path(sys.executable).resolve().parents[3] if getattr(sys, "frozen", False) else Path(__file__).resolve().parent
settings = base/"addresses.txt"
if not settings.exists() and not getattr(sys,"frozen",False): settings = base.parent/"addresses.txt"
ADDR = tuple(settings.read_text().split()) if settings.exists() else ("0x64","0x65","0x66","0x67")
DEST = Path.home() / "Downloads"
def feather():
    ports = [p for p in list_ports.comports() if p.vid == 0x239a and "Feather" in (p.description or "")]
    if len(ports) != 1:
        raise OSError("Feather not connected or multiple Feathers detected.")
    return serial.Serial(ports[0].device, 115200, timeout=0)

class QuietApplication(QApplication):
    def notify(self, receiver, event):
        if event.type() == QEvent.Type.ToolTip:
            return True
        return super().notify(receiver,event)

class CsvGraph(QWidget):
    def __init__(self):
        super().__init__()
        self.layout_box = QVBoxLayout(self)
        self.open_btn = QPushButton("Open CSV…")
        self.open_btn.clicked.connect(self.choose)
        self.layout_box.addWidget(self.open_btn)
        self.message = QLabel("No recording selected")
        self.layout_box.addWidget(self.message)
        self.canvas = None
        self.layout_box.addStretch()

    def choose(self):
        name, _ = QFileDialog.getOpenFileName(self, "Open recording", str(DEST), "CSV recordings (*.csv *.CSV)")
        if name:
            self.load_csv(Path(name))

    def load_csv(self, path):
        import math
        from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg
        from matplotlib.figure import Figure
        series = {a: ([], []) for a in ADDR}
        first = None
        try:
            with path.open(encoding="utf-8-sig", newline="") as stream:
                rows = csv.DictReader(stream)
                fields = set(rows.fieldnames or [])
                usb = {"board_time_us","address","status","temperature_C"} <= fields
                sd = "Time_ms" in fields and any("TC_"+a+"_C" in fields for a in ADDR)
                if not (usb or sd):
                    raise ValueError("Expected a StaticFire USB or FLIGHT SD recording.")
                for row in rows:
                    try:
                        stamp = int(row["board_time_us" if usb else "Time_ms"])
                    except (TypeError, ValueError):
                        continue
                    if first is None: first = stamp
                    t = (stamp-first)/(1e6 if usb else 1000)
                    for a in ([row["address"]] if usb else ADDR):
                        if a not in series: continue
                        val = row.get("temperature_C" if usb else "TC_"+a+"_C")
                        ok = row.get("status" if usb else "Status_"+a, "OK") == "OK"
                        try: y = float(val) if ok else math.nan
                        except (ValueError, TypeError): y = math.nan
                        series[a][0].append(t);series[a][1].append(y if math.isfinite(y) else math.nan)
            if not any(any(math.isfinite(y) for y in ys) for xs,ys in series.values()):
                raise ValueError("No valid thermocouple temperatures in this recording.")
            fig = Figure(figsize=(9,4.5), layout="constrained")
            ax = fig.subplots()
            for i,(a,(xs,ys)) in enumerate(series.items()):
                if any(math.isfinite(y) for y in ys):
                    ax.plot(xs,ys,color=("tab:blue","tab:orange","tab:green","tab:red","tab:purple","tab:brown")[i],label="Thermocouple "+a,linewidth=1.5)
            title = path.stem
            try: title = "StaticFire "+datetime.strptime(title,"StaticFire_%Y-%m-%d_%H-%M-%S_%f").strftime("%Y-%m-%d %H:%M:%S")
            except ValueError: pass
            ax.set(title=title+" – Temperature by Thermocouple",xlabel="Time since start (s)",ylabel="Temperature (°C)")
            ax.grid(alpha=.30);ax.legend(loc="upper right")
            canvas = FigureCanvasQTAgg(fig)
            if self.canvas:
                self.layout_box.removeWidget(self.canvas);self.canvas.deleteLater()
            else: self.layout_box.takeAt(self.layout_box.count()-1)
            self.canvas=canvas;self.layout_box.addWidget(canvas,1)
            self.message.setText(path.name)
        except (ValueError, OSError, csv.Error) as err:
            QMessageBox.warning(self,"Open CSV",str(err))

class Recorder(QWidget):
    def event(self,event):
        if event.type() == QEvent.Type.ToolTip:
            return True
        return super().event(event)

    def __init__(self):
        super().__init__()
        self.setWindowTitle("Thermocouple Data Logger")
        self.resize(970,560)
        outer=QVBoxLayout(self);self.tabs=QTabWidget();outer.addWidget(self.tabs)
        live=QWidget();self.tabs.addTab(live,"Recorder");self.graph=CsvGraph();self.tabs.addTab(self.graph,"CSV Graph")
        lay=QVBoxLayout(live)
        self.status = QLabel(); self.status.setWordWrap(True); lay.addWidget(self.status)
        self.table = QTableWidget(len(ADDR),4)
        self.table.setHorizontalHeaderLabels(["Address","Temperature °C","Status","Events / second"])
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        for i,a in enumerate(ADDR):
            for j,v in enumerate([a,"—","Waiting","—"]): self.table.setItem(i,j,QTableWidgetItem(v))
        self.table.horizontalHeader().setStretchLastSection(True); lay.addWidget(self.table)
        self.btn = QPushButton("Start recording"); self.btn.clicked.connect(self.toggle); lay.addWidget(self.btn)
        self.reconnect = QPushButton("Reconnect"); self.reconnect.clicked.connect(self.connect); lay.addWidget(self.reconnect)
        self.note = QLabel(""); self.note.setWordWrap(True);lay.addWidget(self.note)
        self.port=self.file=None
        self.connect()
        self.timer=QTimer(self);self.timer.timeout.connect(self.poll);self.timer.start(10)
    def connect(self):
        self.stop()
        if self.port: self.port.close()
        self.port=None;self.buf=b"";self.latest={};self.count=[0]*len(ADDR);self.last=time.monotonic();self.prev=None;self.ready=False
        for i in range(len(ADDR)):
            for j,v in enumerate(["—","Waiting","—"],1):self.table.item(i,j).setText(v)
        self.btn.setEnabled(False)
        try:
            self.port=feather();self.status.setText("Connected • Waiting for thermocouple stream")
        except OSError as e:self.status.setText(str(e))
    def stop(self):
        if self.file:
            try:
                self.file.flush();os.fsync(self.file.fileno())
            finally:
                self.file.close();self.file=None
            self.note.setText(f"Saved {self.rows:,} rows to {self.path}")
        self.btn.setText("Start recording")
    def toggle(self):
        try:
            if self.file:self.stop();return
            if not self.ready:return
            self.path=DEST / datetime.now().strftime("StaticFire_%Y-%m-%d_%H-%M-%S_%f.csv")
            self.file=self.path.open("x",newline="",encoding="utf-8")
            self.writer=csv.writer(self.file)
            self.writer.writerow(["host_utc","board_time_us","sequence","address","status","temperature_C","missing_usb_events_before"])
            self.rows=0;self.btn.setText("Stop and save");self.note.setText(f"Recording to {self.path}")
        except OSError as e:self.status.setText(f"File error: {e}")
    def poll(self):
        if not self.port:return
        try:
            self.buf+=self.port.read(65536)
            lines=self.buf.split(b"\n");self.buf=lines.pop()[-2048:]
            for line in lines:
                p=line.decode("ascii",errors="ignore").strip().split(",")
                if len(p)!=6 or p[0]!="T" or p[3] not in ADDR:continue
                _,seq,us,a,state,c=p
                try:n=int(seq);int(us);deg=float(c) if state=="OK" else None
                except ValueError:continue
                gap=0 if self.prev is None else (n-self.prev-1)%2**32
                self.prev=n;self.ready=True;self.btn.setEnabled(True)
                i=ADDR.index(a);self.latest[i]=(deg,state,time.monotonic());self.count[i]+=1
                if self.file:
                    self.writer.writerow([datetime.now(timezone.utc).isoformat(timespec="microseconds"),us,seq,a,state,c,gap]);self.rows+=1
            now=time.monotonic()
            if now-self.last>=1:
                for i in range(len(ADDR)):
                    deg,state,seen=self.latest.get(i,(None,"Waiting",0))
                    stale=now-seen>2
                    self.table.item(i,1).setText("—" if stale or deg is None else f"{deg:.2f}")
                    self.table.item(i,2).setText("No recent data" if stale else {"OPEN":"No thermocouple"}.get(state,state))
                    self.table.item(i,3).setText(f"{self.count[i]/(now-self.last):.1f}")
                self.count=[0]*len(ADDR);self.last=now
                if self.file:self.file.flush()
                self.status.setText(f"Recording • {self.rows:,} rows" if self.file else "Live" if self.ready else "Waiting for data")
        except OSError as e:
            try:self.stop()
            except OSError:pass
            self.port.close();self.port=None;self.btn.setEnabled(False);self.status.setText(f"Recording stopped: {e}")
    def closeEvent(self,event):
        self.stop()
        if self.port:self.port.close()
        event.accept()
if __name__=="__main__":
    app=QuietApplication(sys.argv);win=Recorder();win.show();sys.exit(app.exec())
