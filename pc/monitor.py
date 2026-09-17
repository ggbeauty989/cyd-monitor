#!/usr/bin/env python3
"""
CYD Hardware Monitor - lato PC (Windows / Linux)

Raccoglie i dati di sistema (CPU, GPU, RAM, rete) e li invia via seriale
a un ESP32-2432S028 ("Cheap Yellow Display") come righe JSON.

Sorgenti dati:
  - psutil                  -> CPU %, frequenza, RAM, rete (tutti gli OS)
  - Linux sysfs / psutil    -> temperatura CPU, GPU AMD
  - NVIDIA NVML (pynvml)    -> GPU NVIDIA (Windows e Linux)
  - LibreHardwareMonitor    -> temperature CPU/GPU su Windows (WMI o server web :8085)

Uso:
  python monitor.py                 # rileva la porta in automatico
  python monitor.py --port COM5     # porta esplicita
  python monitor.py --dry-run       # nessuna seriale, mostra solo i dati
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import platform
import re
import socket
import sys
import time
import urllib.request
from datetime import datetime
from typing import Optional

import psutil
import serial
import serial.tools.list_ports
from rich.console import Console
from rich.live import Live
from rich.panel import Panel
from rich.table import Table

# Avviato con pythonw.exe (senza finestra) non esiste un terminale: scarta l'output
if sys.stdout is None:
    sys.stdout = open(os.devnull, "w")
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w")

IS_WINDOWS = platform.system() == "Windows"
IS_LINUX = platform.system() == "Linux"

# VID dei convertitori USB-seriale tipici delle schede ESP32
ESP_USB_VIDS = {0x1A86: "CH340", 0x10C4: "CP210x", 0x303A: "Espressif", 0x0403: "FTDI"}

console = Console()


# --------------------------------------------------------------------------- #
#  Sorgenti sensori
# --------------------------------------------------------------------------- #
def short_gpu_name(name: str) -> str:
    """'NVIDIA GeForce RTX 4080' -> 'RTX 4080', 'AMD Radeon RX 9070 XT' -> 'RX 9070 XT'."""
    for junk in ("NVIDIA", "GeForce", "AMD", "Radeon(TM)", "Radeon", "Intel(R)", "(TM)", "(R)", "Graphics"):
        name = name.replace(junk, "")
    name = name.encode("ascii", "ignore").decode()  # il font del display è solo ASCII
    return " ".join(name.split())[:16] or "iGPU"


def short_cpu_name(name: str) -> str:
    """'AMD Ryzen 7 9800X3D 8-Core Processor' -> 'Ryzen 7 9800X3D',
    'Intel(R) Core(TM) i7-14700K' -> 'i7-14700K'."""
    name = name.split("@")[0]  # Intel vecchi: "... CPU @ 3.60GHz"
    name = re.sub(r"\b\d+-Core\b|with Radeon.*|Processor|CPU", "", name)
    for junk in ("AMD", "Intel(R)", "Core(TM)", "(TM)", "(R)"):
        name = name.replace(junk, "")
    name = name.encode("ascii", "ignore").decode()
    return " ".join(name.split())[:16]


def system_cpu_name() -> str:
    """Nome del processore dal sistema operativo (non serve LibreHardwareMonitor)."""
    try:
        if IS_WINDOWS:
            import winreg

            key = r"HARDWARE\DESCRIPTION\System\CentralProcessor\0"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                return winreg.QueryValueEx(k, "ProcessorNameString")[0]
        if IS_LINUX:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith(("model name", "Model")):
                        return line.split(":", 1)[1]
    except Exception:
        pass
    return platform.processor()


class NvidiaSource:
    """GPU NVIDIA tramite NVML (pacchetto nvidia-ml-py)."""

    def __init__(self) -> None:
        self.handle = None
        try:
            import pynvml

            pynvml.nvmlInit()
            self.nv = pynvml
            self.handle = pynvml.nvmlDeviceGetHandleByIndex(0)
        except Exception:
            self.handle = None

    @property
    def ok(self) -> bool:
        return self.handle is not None

    def read(self) -> dict:
        nv, h = self.nv, self.handle
        out: dict = {}
        try:
            out["gpu_t"] = float(nv.nvmlDeviceGetTemperature(h, nv.NVML_TEMPERATURE_GPU))
            out["gpu_u"] = float(nv.nvmlDeviceGetUtilizationRates(h).gpu)
            mem = nv.nvmlDeviceGetMemoryInfo(h)
            out["vram_used"] = mem.used / 1024**3
            out["vram_tot"] = mem.total / 1024**3
            name = nv.nvmlDeviceGetName(h)  # il nome solo se i sensori rispondono
            out["gpu_name"] = short_gpu_name(name.decode() if isinstance(name, bytes) else name)
        except Exception:
            pass
        try:
            out["gpu_p"] = nv.nvmlDeviceGetPowerUsage(h) / 1000.0
        except Exception:
            pass
        return out


class LinuxAmdGpuSource:
    """GPU AMD (e alcune Intel) su Linux tramite sysfs amdgpu."""

    PCI_IDS = ("/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids")

    def __init__(self) -> None:
        # Con più GPU (es. integrata del Ryzen + scheda dedicata) scegli quella con più VRAM
        cards = [c for c in glob.glob("/sys/class/drm/card[0-9]/device")
                 if os.path.exists(os.path.join(c, "gpu_busy_percent"))]
        self.dev = max(cards, key=lambda c: self._read_num(f"{c}/mem_info_vram_total") or 0, default=None)
        self.name = self._pci_name(self.dev) if self.dev else ""

    @classmethod
    def _pci_name(cls, dev: str) -> str:
        """
        amdgpu non espone il nome commerciale: lo cerca nel database pci.ids
        tramite gli ID della scheda. Es. 1002:7550 -> "Navi 48 [Radeon RX 9070/9070 XT/9070 GRE]"
        -> "RX 9070/9070 XT" (lo stesso chip può essere venduto con più nomi).
        """
        try:
            with open(f"{dev}/vendor") as f:
                vendor = f.read().strip().lower().removeprefix("0x")
            with open(f"{dev}/device") as f:
                device = f.read().strip().lower().removeprefix("0x")
        except OSError:
            return ""
        for path in cls.PCI_IDS:
            try:
                f = open(path, encoding="utf-8", errors="replace")
            except OSError:
                continue
            with f:
                in_vendor = False
                for line in f:
                    if not line.strip() or line.startswith("#"):
                        continue
                    if not line.startswith("\t"):  # riga del produttore
                        if in_vendor:
                            break
                        in_vendor = line[:4].lower() == vendor
                    elif in_vendor and not line.startswith("\t\t") and line[1:5].lower() == device:
                        name = line[5:].strip()
                        m = re.search(r"\[(.+)\]", name)  # "Navi 48 [Radeon RX ...]" -> parte tra []
                        return short_gpu_name(m.group(1) if m else name).rstrip("/ ")
        return ""

    @property
    def ok(self) -> bool:
        return self.dev is not None

    @staticmethod
    def _read_num(path: str) -> Optional[float]:
        try:
            with open(path) as f:
                return float(f.read().strip())
        except Exception:
            return None

    def read(self) -> dict:
        d = self.dev
        out: dict = {}
        u = self._read_num(f"{d}/gpu_busy_percent")
        if u is not None:
            out["gpu_u"] = u
        used = self._read_num(f"{d}/mem_info_vram_used")
        tot = self._read_num(f"{d}/mem_info_vram_total")
        if used is not None and tot:
            out["vram_used"], out["vram_tot"] = used / 1024**3, tot / 1024**3
        for hw in glob.glob(f"{d}/hwmon/hwmon*"):
            t = self._read_num(f"{hw}/temp1_input")
            if t is not None:
                out["gpu_t"] = t / 1000.0
            p = self._read_num(f"{hw}/power1_average") or self._read_num(f"{hw}/power1_input")
            if p is not None:
                out["gpu_p"] = p / 1_000_000.0
        if self.name and out:  # nome solo se la scheda ha risposto
            out["gpu_name"] = self.name
        return out


class LibreHardwareMonitorSource:
    """
    Windows: legge i sensori da LibreHardwareMonitor (LHM), che deve essere in
    esecuzione come amministratore. Due canali, provati in quest'ordine:
      1. WMI (root\\LibreHardwareMonitor) - solo versioni di LHM che lo espongono
      2. Server web di LHM (Options -> Remote Web Server), JSON su porta 8085
    """

    CPU_TEMP_NAMES = ("Core (Tctl/Tdie)", "CPU Package", "Core Average", "Core (Tctl)", "Tdie", "Tctl")
    GPU_VENDORS = ("gpu-nvidia", "nvidiagpu", "gpu-amd", "atigpu", "gpu-intel")

    def __init__(self, url: str = "http://127.0.0.1:8085/data.json") -> None:
        self.url = url
        self.mode: Optional[str] = None
        self.conn = None
        self.hw_names: dict[str, str] = {}  # "gpu-amd/5" -> "AMD Radeon RX 9070 XT"
        if IS_WINDOWS:
            try:
                import wmi

                for ns in (r"root\LibreHardwareMonitor", r"root\OpenHardwareMonitor"):
                    try:
                        c = wmi.WMI(namespace=ns)
                        if c.Sensor():
                            self.conn, self.mode = c, "WMI"
                            return
                    except Exception:
                        continue
            except ImportError:
                pass
        if IS_WINDOWS and self._http_sensors():  # LHM esiste solo per Windows
            self.mode = "web"

    @property
    def ok(self) -> bool:
        return self.mode is not None

    # --- lettura grezza: lista di (id, nome, tipo, valore) ------------------
    @staticmethod
    def _num(text) -> Optional[float]:
        """'57,3 °C' / '2045.0 MB' / 57.3 -> float (gestisce la virgola decimale)."""
        if isinstance(text, (int, float)):
            return float(text)
        m = re.search(r"-?\d+(?:[.,]\d+)?", str(text))
        return float(m.group().replace(",", ".")) if m else None

    def _http_sensors(self) -> list[tuple]:
        try:
            with urllib.request.urlopen(self.url, timeout=0.8) as r:
                root = json.load(r)
        except Exception:
            return []
        out: list[tuple] = []
        names: dict[str, str] = {}
        # Albero: hardware -> gruppo ("Temperatures") -> sensore. Il nome
        # dell'hardware (es. "AMD Radeon RX 9070 XT") è quindi il "nonno".
        stack = [(root, "", "")]
        while stack:
            node, parent, grandparent = stack.pop()
            if node.get("SensorId"):
                sid = node["SensorId"]
                val = self._num(node.get("RawValue") or node.get("Value"))
                out.append((sid, node.get("Text", ""), node.get("Type", ""), val))
                names["/".join(sid.split("/")[1:3])] = grandparent
            for child in node.get("Children", []):
                stack.append((child, node.get("Text", ""), parent))
        self.hw_names = names
        return out

    def _sensors(self) -> list[tuple]:
        if self.mode == "WMI":
            try:
                self.hw_names = {"/".join(hw.Identifier.split("/")[1:3]): hw.Name
                                 for hw in self.conn.Hardware()}
                return [(s.Identifier, s.Name, s.SensorType, s.Value) for s in self.conn.Sensor()]
            except Exception:
                return []
        return self._http_sensors()

    # --- interpretazione ---------------------------------------------------
    def read(self) -> dict:
        out: dict = {}
        sensors = [s for s in self._sensors() if s[3] is not None]
        if not sensors:
            return out

        cpu = [s for s in sensors if "cpu" in s[0].split("/")[1]]
        if cpu and self.hw_names.get("/".join(cpu[0][0].split("/")[1:3])):
            out["cpu_name"] = short_cpu_name(self.hw_names["/".join(cpu[0][0].split("/")[1:3])])

        # --- CPU ---
        temps = {name: v for _, name, typ, v in cpu if typ == "Temperature"}
        for name in self.CPU_TEMP_NAMES:
            if name in temps:
                out["cpu_t"] = temps[name]
                break
        else:
            if temps:
                out["cpu_t"] = max(temps.values())

        clocks = {name: v for _, name, typ, v in cpu if typ == "Clock"}
        if "Cores (Average)" in clocks:
            out["cpu_f"] = clocks["Cores (Average)"]
        else:
            cores = [v for n, v in clocks.items() if "Core #" in n and "Effective" not in n and v]
            if cores:
                out["cpu_f"] = sum(cores) / len(cores)

        # --- GPU: raggruppa per scheda (/gpu-amd/5) e scegli la dedicata ---
        groups: dict[str, list] = {}
        for s in sensors:
            parts = s[0].split("/")
            if len(parts) > 2 and parts[1] in self.GPU_VENDORS:
                groups.setdefault("/".join(parts[1:3]), []).append(s)

        def vram_total(sens: list) -> float:
            return max((v for _, n, t, v in sens if t == "SmallData" and n == "GPU Memory Total"), default=0)

        def rank(key: str):  # vendor più "dedicato" prima, poi più VRAM
            return (self.GPU_VENDORS.index(key.split("/")[0]) // 2, -vram_total(groups[key]))

        gpu_key = min(groups, key=rank) if groups else None
        gpu = groups[gpu_key] if gpu_key else []

        def pick(stype: str, *names: str) -> Optional[float]:
            vals = {n: v for _, n, t, v in gpu if t == stype}
            for n in names:
                if n in vals:
                    return vals[n]
            return None

        if gpu:
            t = pick("Temperature", "GPU Core", "GPU Temperature")
            if t is not None:
                out["gpu_t"] = t
            u = pick("Load", "GPU Core", "D3D 3D")
            if u is not None:
                out["gpu_u"] = u
            used = pick("SmallData", "GPU Memory Used", "D3D Dedicated Memory Used")
            tot = pick("SmallData", "GPU Memory Total")
            if used is not None and tot:
                out["vram_used"], out["vram_tot"] = used / 1024, tot / 1024
            p = pick("Power", "GPU Package", "GPU Power", "GPU Core")
            if p is not None:
                out["gpu_p"] = p
            # il nome accompagna i dati: se non c'è nessuna lettura, niente nome
            if any(k in out for k in ("gpu_t", "gpu_u", "vram_tot")) and self.hw_names.get(gpu_key):
                out["gpu_name"] = short_gpu_name(self.hw_names[gpu_key])
        return out


# --------------------------------------------------------------------------- #
#  Raccolta dati
# --------------------------------------------------------------------------- #
class Collector:
    def __init__(self) -> None:
        self.host = socket.gethostname()[:20]
        self._cpu_name, self._cpu_name_time = "", -1e9
        self.lhm = LibreHardwareMonitorSource()
        self._lhm_retry = time.monotonic()
        self.nvidia = NvidiaSource()
        self.amd = LinuxAmdGpuSource() if IS_LINUX else None
        self._amd_time = time.monotonic()
        self._net_last = psutil.net_io_counters()
        self._net_time = time.monotonic()
        psutil.cpu_percent(None)  # primo campione a vuoto

    def sources(self) -> list[str]:
        s = ["psutil"]
        if self.lhm.ok:
            s.append(f"LibreHardwareMonitor ({self.lhm.mode})")
        if self.nvidia.ok:
            s.append("NVIDIA NVML")
        if self.amd and self.amd.ok:
            s.append("AMD sysfs")
        return s

    @staticmethod
    def _linux_cpu_temp() -> Optional[float]:
        try:
            temps = psutil.sensors_temperatures()
        except Exception:
            return None
        for key in ("coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu-thermal", "acpitz"):
            entries = temps.get(key)
            if not entries:
                continue
            for e in entries:  # preferisci Package / Tctl / Tdie
                if e.label and any(x in e.label for x in ("Package", "Tctl", "Tdie")):
                    return e.current
            return max(e.current for e in entries)
        return None

    def collect(self) -> dict:
        data: dict = {
            "host": self.host,
            "time": datetime.now().strftime("%H:%M"),
            "cpu_u": psutil.cpu_percent(None),
            "up": int(time.time() - psutil.boot_time()),
            "procs": len(psutil.pids()),
        }

        freq = psutil.cpu_freq()
        if freq and freq.current:
            data["cpu_f"] = freq.current

        vm = psutil.virtual_memory()
        data["ram_u"] = vm.percent
        data["ram_used"] = (vm.total - vm.available) / 1024**3
        data["ram_tot"] = vm.total / 1024**3

        now = time.monotonic()
        net = psutil.net_io_counters()
        dt = max(now - self._net_time, 1e-3)
        data["net_up"] = (net.bytes_sent - self._net_last.bytes_sent) / dt
        data["net_dn"] = (net.bytes_recv - self._net_last.bytes_recv) / dt
        self._net_last, self._net_time = net, now

        if IS_LINUX:
            t = self._linux_cpu_temp()
            if t is not None:
                data["cpu_t"] = t

        # LHM avviato dopo lo script? Riprova a trovarlo ogni 10 s
        if IS_WINDOWS and not self.lhm.ok and now - self._lhm_retry > 10:
            self.lhm = LibreHardwareMonitorSource()
            self._lhm_retry = now

        # Ordine: le sorgenti successive sovrascrivono le precedenti
        if self.lhm.ok:
            data.update(self.lhm.read())
        if IS_LINUX and now - self._amd_time > 30:  # schede aggiunte/cambiate
            self.amd, self._amd_time = LinuxAmdGpuSource(), now
        if self.amd and self.amd.ok:
            data.update(self.amd.read())
        if self.nvidia.ok:
            data.update(self.nvidia.read())

        # CPU: carico e frequenza sono sempre letti, quindi il nome c'è sempre.
        # Se LHM non l'ha fornito, leggilo dal sistema (ricontrollato ogni 30 s).
        if "cpu_name" not in data:
            if now - self._cpu_name_time > 30:
                self._cpu_name, self._cpu_name_time = short_cpu_name(system_cpu_name()), now
            if self._cpu_name:
                data["cpu_name"] = self._cpu_name

        return {k: round(v, 2) if isinstance(v, float) else v for k, v in data.items()}


# --------------------------------------------------------------------------- #
#  Seriale
# --------------------------------------------------------------------------- #
def find_port() -> Optional[str]:
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid in ESP_USB_VIDS:
            return p.device
    return None


def open_serial(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0
    ser.write_timeout = 1
    # Evita il reset dell'ESP32 all'apertura della porta
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


# --------------------------------------------------------------------------- #
#  UI terminale
# --------------------------------------------------------------------------- #
def fmt(v, unit: str = "", digits: int = 0) -> str:
    return "[dim]--[/]" if v is None else f"{v:.{digits}f}{unit}"


def temp_color(t) -> str:
    if t is None:
        return "dim"
    return "green" if t < 60 else "yellow" if t < 80 else "red"


def fmt_rate(bps: float) -> str:
    for unit in ("B/s", "KB/s", "MB/s", "GB/s"):
        if bps < 1024:
            return f"{bps:.1f} {unit}"
        bps /= 1024
    return f"{bps:.1f} TB/s"


def render(data: dict, status: str, sources: list[str]) -> Panel:
    t = Table.grid(padding=(0, 2))
    t.add_column(style="bold cyan", justify="right")
    t.add_column()
    ct, gt = data.get("cpu_t"), data.get("gpu_t")
    t.add_row("CPU", f"[{temp_color(ct)}]{fmt(ct, '°C', 1)}[/]   {fmt(data.get('cpu_u'), '%', 0)}   {fmt(data.get('cpu_f'), ' MHz')}   [dim]{data.get('cpu_name', '')}[/]")
    t.add_row("GPU", f"[{temp_color(gt)}]{fmt(gt, '°C', 1)}[/]   {fmt(data.get('gpu_u'), '%', 0)}   {fmt(data.get('gpu_p'), ' W')}   [dim]{data.get('gpu_name', '')}[/]")
    t.add_row("RAM", f"{fmt(data.get('ram_used'), '', 1)} / {fmt(data.get('ram_tot'), ' GB', 1)}  ({fmt(data.get('ram_u'), '%')})")
    if "vram_tot" in data:
        t.add_row("VRAM", f"{fmt(data.get('vram_used'), '', 1)} / {fmt(data.get('vram_tot'), ' GB', 1)}")
    t.add_row("NET", f"↓ {fmt_rate(data.get('net_dn', 0))}   ↑ {fmt_rate(data.get('net_up', 0))}")
    t.add_row("", "")
    t.add_row("Seriale", status)
    t.add_row("Sorgenti", ", ".join(sources))
    return Panel(t, title=f"[bold magenta]CYD Monitor[/] · {data.get('host', '')}", border_style="magenta", expand=False)


# --------------------------------------------------------------------------- #
#  Main
# --------------------------------------------------------------------------- #
def main() -> None:
    ap = argparse.ArgumentParser(description="Invia i dati hardware del PC a un ESP32-2432S028")
    ap.add_argument("--port", help="porta seriale (es. COM5 o /dev/ttyUSB0). Default: auto")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--interval", type=float, default=1.0, help="secondi tra un invio e l'altro")
    ap.add_argument("--dry-run", action="store_true", help="non usa la seriale, mostra solo i dati")
    ap.add_argument("--no-ui", action="store_true", help="stampa JSON invece della dashboard")
    ap.add_argument("--once", action="store_true", help="esegue una sola lettura ed esce")
    args = ap.parse_args()

    collector = Collector()
    if IS_WINDOWS and not collector.lhm.ok:
        console.print("[yellow]Nota:[/] LibreHardwareMonitor non rilevato: su Windows la temperatura CPU "
                      "non sarà disponibile. Avvialo come amministratore (vedi README).")

    ser: Optional[serial.Serial] = None
    status = "[dim]dry-run[/]" if args.dry_run else "[yellow]in ricerca...[/]"
    time.sleep(min(args.interval, 0.5))

    def tick() -> dict:
        nonlocal ser, status
        data = collector.collect()
        if args.dry_run:
            return data
        if ser is None:
            port = args.port or find_port()
            if port:
                try:
                    ser = open_serial(port, args.baud)
                    status = f"[green]connesso[/] {port} @ {args.baud}"
                except serial.SerialException as e:
                    status = f"[red]errore[/] {port}: {e}"
            else:
                status = "[yellow]nessun ESP32 trovato, riprovo...[/]"
        if ser is not None:
            try:
                ser.write((json.dumps(data, separators=(",", ":")) + "\n").encode())
                ser.reset_input_buffer()
            except (serial.SerialException, OSError) as e:
                status = f"[red]disconnesso[/] ({e.__class__.__name__}), riprovo..."
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
        return data

    try:
        if args.no_ui or args.once:
            while True:
                data = tick()
                print(json.dumps(data, ensure_ascii=False), flush=True)
                if args.once:
                    break
                time.sleep(args.interval)
        else:
            with Live(render({}, status, collector.sources()), console=console, refresh_per_second=4) as live:
                while True:
                    data = tick()
                    live.update(render(data, status, collector.sources()))
                    time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        if ser is not None:
            ser.close()


if __name__ == "__main__":
    main()
