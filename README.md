# CYD Hardware Monitor

Dashboard hardware del PC su **ESP32-2432S028** (Cheap Yellow Display, 2.8" 320×240).

```
PC (monitor.py)  ──USB seriale, JSON 1/s──►  ESP32 (LVGL 9)
```

## 1. Firmware ESP32 (PlatformIO)

1. Installa [VS Code](https://code.visualstudio.com/) + estensione **PlatformIO IDE**.
2. Apri la cartella `firmware/`, collega il CYD e premi **Upload** (→).
   Le librerie (LVGL 9, TFT_eSPI, ArduinoJson, XPT2046_Touchscreen) vengono
   scaricate da sole e i pin di display e touch sono già configurati.

Il display ha due pagine, si passa dall'una all'altra **toccando lo schermo**
(o premendo il tasto **BOOT**):

- **valori**: temperature, carico, frequenza, potenza, RAM/VRAM, rete
- **grafici** (`./sysmon -g`): storico degli ultimi 60 s di carico (verde)
  e temperatura (ambra) di CPU e GPU

Senza PC collegato il display mostra *NO CARRIER*.

> Se i colori risultano invertiti o lo schermo resta bianco (esistono varianti
> del CYD), in `platformio.ini` sostituisci `ILI9341_2_DRIVER` con `ST7789_DRIVER`.

## 2. Script PC (Windows / Linux)

Su Linux segui prima la [configurazione aggiuntiva](#3-linux-configurazione-aggiuntiva).

```bash
cd pc
pip install -r requirements.txt
python monitor.py              # porta rilevata in automatico (CH340/CP210x)
python monitor.py --port COM5  # oppure /dev/ttyUSB0
python monitor.py --dry-run    # prova senza ESP32
```

Opzioni: `--interval 0.5`, `--baud`, `--no-ui` (output JSON), `--once`.

**Chiudi il Serial Monitor di PlatformIO/Arduino** prima di avviare lo script,
altrimenti la porta risulta occupata.

### Sensori per sistema operativo

| Dato | Linux | Windows |
|---|---|---|
| CPU %, RAM, rete | psutil | psutil |
| Frequenza CPU | psutil (reale) | LibreHardwareMonitor (psutil dà un valore fisso) |
| Temperatura CPU | `coretemp`/`k10temp` | **LibreHardwareMonitor** |
| GPU NVIDIA | NVML | NVML |
| GPU AMD | sysfs `amdgpu` | LibreHardwareMonitor |
| GPU Intel | non supportata | LibreHardwareMonitor |
| Nome GPU | solo NVIDIA | LibreHardwareMonitor / NVML |

**Windows:** Windows non espone la temperatura della CPU. Scarica
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases),
avvialo **come amministratore** e lascialo aperto (anche ridotto a icona).
Lo script ne legge i sensori via WMI oppure, nelle versioni recenti, dal
server web integrato: in LHM attiva **Options → Remote Web Server → Run**
(porta 8085). Se LHM viene avviato dopo lo script, viene trovato entro 10 s.
Con più GPU viene scelta quella dedicata (NVIDIA > AMD > Intel, poi più VRAM).

## 3. Linux: configurazione aggiuntiva

Comandi per Debian/Ubuntu/Mint; per Fedora usa `dnf`, per Arch `pacman`.

### Python in un ambiente virtuale

Le distribuzioni recenti bloccano `pip install` sul Python di sistema
(errore `externally-managed-environment`). Usa un ambiente virtuale:

```bash
sudo apt install python3 python3-venv python3-pip
cd cyd-monitor/pc
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python monitor.py
```

(`wmi` e `pywin32` sono solo per Windows e vengono saltati in automatico.)

### Permessi della porta seriale

Il CYD compare come `/dev/ttyUSB0`. Per usarlo senza `sudo` aggiungi il tuo
utente al gruppo della porta, poi **esci e rientra** (o riavvia):

```bash
sudo usermod -aG dialout $USER     # Arch: gruppo "uucp" invece di "dialout"
```

### Il CYD non compare / sparisce dopo pochi secondi (Ubuntu)

Il pacchetto `brltty` (display braille) su Ubuntu "ruba" i convertitori
CH340 e la porta `/dev/ttyUSB0` scompare. Se non usi un display braille:

```bash
sudo apt remove brltty
```

Verifica con `ls /dev/ttyUSB*` dopo aver ricollegato la scheda.

### Temperature CPU

Lo script legge i sensori del kernel (`coretemp` per Intel, `k10temp` per AMD),
di solito già attivi. Se la temperatura CPU resta `--`:

```bash
sudo apt install lm-sensors
sudo sensors-detect --auto
sensors            # deve mostrare "k10temp" o "coretemp"
```

### GPU

- **AMD**: funziona subito con il driver open `amdgpu` del kernel, niente da
  installare. Con più GPU (es. integrata del Ryzen + dedicata) viene scelta
  quella con più VRAM. Il nome della scheda non viene mostrato (solo `GPU`).
- **NVIDIA**: serve il **driver proprietario** NVIDIA (non `nouveau`), che
  include la libreria NVML: `sudo ubuntu-drivers install`, poi verifica con
  `nvidia-smi`.
- **Intel**: non supportata su Linux.

### Upload del firmware con PlatformIO

Su Linux PlatformIO richiede le sue regole udev per accedere alla scheda:

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Poi ricollega la scheda. Serve anche il gruppo `dialout` (vedi sopra).

### Avvio automatico (opzionale)

Per far partire lo script da solo all'avvio, crea
`~/.config/systemd/user/cyd-monitor.service` (adatta il percorso):

```ini
[Unit]
Description=CYD Hardware Monitor

[Service]
ExecStart=%h/cyd-monitor/pc/.venv/bin/python %h/cyd-monitor/pc/monitor.py --no-ui
StandardOutput=null
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now cyd-monitor
loginctl enable-linger $USER    # parte anche senza fare login
```

Stato e log: `systemctl --user status cyd-monitor`.

## Protocollo

Una riga JSON per aggiornamento; i campi mancanti vengono mostrati come `--`:

```json
{"host":"PC","time":"16:09","cpu_t":54.0,"cpu_u":12.5,"cpu_f":4700,
 "gpu_name":"RX 9070 XT","gpu_t":48,"gpu_u":7,"gpu_p":35.2,"vram_used":1.2,"vram_tot":12,
 "ram_u":39,"ram_used":24.2,"ram_tot":61.6,"net_up":2560,"net_dn":680,
 "up":273600,"procs":312}
```
