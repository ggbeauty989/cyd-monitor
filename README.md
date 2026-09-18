# CYD Hardware Monitor

PC hardware dashboard on an **ESP32-2432S028** (Cheap Yellow Display, 2.8" 320×240).

```
PC (monitor.py)  ──USB serial, JSON 1/s──►  ESP32 (LVGL 9)
```

## 1. ESP32 firmware (PlatformIO)

1. Install [VS Code](https://code.visualstudio.com/) + the **PlatformIO IDE** extension.
2. Open the `firmware/` folder, plug in the CYD and press **Upload** (→).
   The libraries (LVGL 9, TFT_eSPI, ArduinoJson, XPT2046_Touchscreen) are
   downloaded automatically and the display and touch pins are already configured.

The display has three pages, cycled in order by **touching the screen**
(or pressing the **BOOT** button):

- **values**: temperatures, load, frequency, power, RAM/VRAM, network
- **charts** (`./sysmon -g`): 60 s history of load (green)
  and temperature (amber) for CPU and GPU
- **top** (`./sysmon -t`): the 13 processes using the most CPU, `htop` style
  (PID, CPU%, MEM%, name). CPU% is relative to the whole system, as in
  Task Manager, so the sum matches the load shown on the values page.

**Pressing and holding** the screen (or BOOT) for **2 seconds** rotates the
display by 180°, useful if the CYD is mounted upside down. The choice is kept
after power off; repeat to go back to the normal orientation.

Without a PC connected, the display shows *NO CARRIER*.

> If the colors look inverted or the screen stays blank (there are CYD
> variants), replace `ILI9341_2_DRIVER` with `ST7789_DRIVER` in `platformio.ini`.

## 2. PC script (Windows / Linux)

On Linux, follow the [additional setup](#3-linux-additional-setup) first.

```bash
cd pc
pip install -r requirements.txt
python monitor.py              # port detected automatically (CH340/CP210x)
python monitor.py --port COM5  # or /dev/ttyUSB0
python monitor.py --dry-run    # test without the ESP32
```

Options: `--interval 0.5`, `--baud`, `--no-ui` (JSON output), `--once`.

**Close the PlatformIO/Arduino Serial Monitor** before starting the script,
otherwise the port will be busy.

### Sensors by operating system

| Data | Linux | Windows |
|---|---|---|
| CPU %, RAM, network | psutil | psutil |
| CPU frequency | psutil (real) | LibreHardwareMonitor (psutil gives a fixed value) |
| CPU temperature | `coretemp`/`k10temp` | **LibreHardwareMonitor** |
| NVIDIA GPU | NVML | NVML |
| AMD GPU | sysfs `amdgpu` | LibreHardwareMonitor |
| Intel GPU | not supported | LibreHardwareMonitor |
| CPU name | `/proc/cpuinfo` | LibreHardwareMonitor, otherwise the Windows registry |
| GPU name | NVML / `pci.ids` database (AMD) | LibreHardwareMonitor / NVML |

The GPU name is shown only when its sensors are actually being read
(otherwise the header shows just `GPU`). The names are re-read continuously:
if you change your GPU or CPU the display updates on its own, without
restarting anything.

**Windows:** Windows does not expose the CPU temperature. Download
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases),
run it **as administrator** and leave it open (even minimized to the tray).
The script reads its sensors via WMI or, on recent versions, from the
built-in web server: in LHM enable **Options → Remote Web Server → Run**
(port 8085). If LHM is started after the script, it is found within 10 s.
With multiple GPUs the dedicated one is picked (NVIDIA > AMD > Intel, then
most VRAM).

### Windows: autostart

**1. LibreHardwareMonitor at startup** - in the LHM **Options** menu check:
`Start Minimized`, `Minimize To Tray`, `Run On Windows Startup`
(LHM creates the task itself, which runs as administrator) and make sure
**Remote Web Server → Run** stays active.

**2. The script at startup** - from a terminal in the `pc` folder:

```powershell
powershell -ExecutionPolicy Bypass -File autostart_windows.ps1
```

This creates the scheduled task **"CYD Monitor"**: it starts 20 s after
login, runs in the background without a window (`pythonw.exe`), restarts if
it errors and re-finds the CYD on its own even if you plug it in later.
No administrator privileges are needed.

| Action | Command |
|---|---|
| Start now | `Start-ScheduledTask -TaskName "CYD Monitor"` |
| Stop (e.g. to upload the firmware) | `Stop-ScheduledTask -TaskName "CYD Monitor"` |
| Remove autostart | `powershell -ExecutionPolicy Bypass -File autostart_windows.ps1 -Remove` |

The task is also visible in **Task Scheduler**. While it is active the COM
port is held: stop it before using `monitor.py` manually or uploading.

## 3. Linux: additional setup

Commands for Debian/Ubuntu/Mint; use `dnf` on Fedora, `pacman` on Arch.

### Python in a virtual environment

Recent distributions block `pip install` on the system Python
(error `externally-managed-environment`). Use a virtual environment:

```bash
sudo apt install python3 python3-venv python3-pip
cd cyd-monitor/pc
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python monitor.py
```

(`wmi` and `pywin32` are Windows-only and are skipped automatically.)

### Serial port permissions

The CYD shows up as `/dev/ttyUSB0`. To use it without `sudo`, add your
user to the port's group, then **log out and back in** (or reboot):

```bash
sudo usermod -aG dialout $USER     # Arch: "uucp" group instead of "dialout"
```

### The CYD does not appear / disappears after a few seconds (Ubuntu)

The `brltty` package (braille display) on Ubuntu "steals" the CH340
converters and the `/dev/ttyUSB0` port disappears. If you do not use a
braille display:

```bash
sudo apt remove brltty
```

Verify with `ls /dev/ttyUSB*` after re-plugging the board.

### CPU temperatures

The script reads the kernel sensors (`coretemp` for Intel, `k10temp` for AMD),
usually already enabled. If the CPU temperature stays `--`:

```bash
sudo apt install lm-sensors
sudo sensors-detect --auto
sensors            # must show "k10temp" or "coretemp"
```

### GPU

- **AMD**: works out of the box with the kernel's open `amdgpu` driver,
  nothing to install. With multiple GPUs (e.g. Ryzen iGPU + dedicated) the
  one with the most VRAM is picked. The name comes from the `pci.ids`
  database (`hwdata` or `pciutils` package, almost always already installed)
  and indicates the chip family: an RX 9070 XT shows up as `RX 9070/9070 XT`.
- **NVIDIA**: requires the **proprietary** NVIDIA driver (not `nouveau`),
  which includes the NVML library: `sudo ubuntu-drivers install`, then check
  with `nvidia-smi`.
- **Intel**: not supported on Linux.

### Uploading the firmware with PlatformIO

On Linux PlatformIO needs its udev rules to access the board:

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then re-plug the board. The `dialout` group is also required (see above).

### Autostart (optional)

After creating the virtual environment (see above), from the `pc` folder:

```bash
chmod +x autostart_linux.sh     # only if the file is not executable
./autostart_linux.sh
```

This creates and starts the systemd user service **`cyd-monitor`** with the
correct paths, wherever the project is located. The service:

- starts at PC boot, even without a login (asks for the `sudo` password
  once, for `loginctl enable-linger`)
- uses the `.venv` Python and runs in the background
- restarts if it errors and re-finds the CYD on its own when you plug it in

Before installing it checks that the Python libraries are present and warns
if the user is not in the `dialout` group.

> **Note:** `autostart_linux.sh` is a **one-shot installer to be run manually
> once**, from your user, **without `sudo`**. It is not the program to keep
> running: do not put it in the `ExecStart=` of a hand-made service in
> `/etc/systemd/system/`, nor in cron or `rc.local`. It creates the service
> itself, in `~/.config/systemd/user/`, and it launches `monitor.py` directly.

All the commands below require **`--user`**: without it, `systemctl` and
`journalctl` look for a system service that does not exist (or show a wrong
one with the same name).

| Action | Command |
|---|---|
| Status | `systemctl --user status cyd-monitor` |
| Live logs | `journalctl --user -u cyd-monitor -f` |
| Stop (e.g. to upload the firmware) | `systemctl --user stop cyd-monitor` |
| Restart (e.g. after updating the script) | `systemctl --user restart cyd-monitor` |
| Remove autostart | `./autostart_linux.sh --remove` |

If you move the project folder, run `./autostart_linux.sh` again to update
the paths.

The log is almost empty on purpose: the normal output of `monitor.py` is
discarded (`StandardOutput=null`), only errors end up in the journal.

#### `Failed to connect to user scope bus` / `status=1/FAILURE` error

If `journalctl -u cyd-monitor` (without `--user`) shows:

```
systemctl[…]: Failed to connect to user scope bus via local transport:
$DBUS_SESSION_BUS_ADDRESS and $XDG_RUNTIME_DIR not defined
cyd-monitor.service: Main process exited, code=exited, status=1/FAILURE
cyd-monitor.service: Start request repeated too quickly.
```

there is a **system** service running `autostart_linux.sh` as `ExecStart`.
In that context `systemctl --user` has no user session to connect to, so the
script exits with an error and systemd keeps restarting it for nothing.
Remove that service and install the right one:

```bash
sudo systemctl disable --now cyd-monitor.service
sudo rm /etc/systemd/system/cyd-monitor.service
sudo systemctl daemon-reload

./autostart_linux.sh            # from your user, without sudo
systemctl --user status cyd-monitor
```

Recent versions of the script detect this on their own: when run with
`sudo`, from a system service or from a session without a user login (e.g.
`su` from root) it stops immediately with a message explaining what to do.

## Protocol

One JSON line per update; missing fields are shown as `--`:

```json
{"host":"PC","time":"16:09","cpu_name":"Ryzen 7 9800X3D","cpu_t":54.0,"cpu_u":12.5,"cpu_f":4700,
 "gpu_name":"RX 9070 XT","gpu_t":48,"gpu_u":7,"gpu_p":35.2,"vram_used":1.2,"vram_tot":12,
 "ram_u":39,"ram_used":24.2,"ram_tot":61.6,"net_up":2560,"net_dn":680,
 "up":273600,"procs":312}
```
