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

**Windows:** Windows non espone la temperatura della CPU. Scarica
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases),
avvialo **come amministratore** e lascialo aperto (anche ridotto a icona).
Lo script ne legge i sensori via WMI oppure, nelle versioni recenti, dal
server web integrato: in LHM attiva **Options → Remote Web Server → Run**
(porta 8085). Se LHM viene avviato dopo lo script, viene trovato entro 10 s.
Con più GPU viene scelta quella dedicata (NVIDIA > AMD > Intel, poi più VRAM).

**Linux:** per la porta seriale aggiungi l'utente al gruppo `dialout`
(`sudo usermod -aG dialout $USER`, poi rifai il login). Per le temperature
può servire `lm-sensors`.

## Protocollo

Una riga JSON per aggiornamento; i campi mancanti vengono mostrati come `--`:

```json
{"host":"PC","time":"16:09","cpu_t":54.0,"cpu_u":12.5,"cpu_f":4700,
 "gpu_name":"RX 9070 XT","gpu_t":48,"gpu_u":7,"gpu_p":35.2,"vram_used":1.2,"vram_tot":12,
 "ram_u":39,"ram_used":24.2,"ram_tot":61.6,"net_up":2560,"net_dn":680,
 "up":273600,"procs":312}
```
