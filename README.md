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

Il display ha tre pagine, si scorrono in sequenza **toccando lo schermo**
(o premendo il tasto **BOOT**):

- **valori**: temperature, carico, frequenza, potenza, RAM/VRAM, rete
- **grafici** (`./sysmon -g`): storico degli ultimi 60 s di carico (verde)
  e temperatura (ambra) di CPU e GPU
- **top** (`./sysmon -t`): i 13 processi che consumano più CPU, stile `htop`
  (PID, CPU%, MEM%, nome). La CPU% è riferita all'intero sistema, come in
  Task Manager, quindi la somma corrisponde al carico della pagina valori.

**Tenendo premuto** lo schermo (o BOOT) per **2 secondi** la schermata ruota
di 180°, utile se il CYD è montato capovolto. La scelta resta salvata anche
dopo lo spegnimento; ripeti per tornare all'orientamento normale.

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
| Nome CPU | `/proc/cpuinfo` | LibreHardwareMonitor, altrimenti registro di Windows |
| Nome GPU | NVML / database `pci.ids` (AMD) | LibreHardwareMonitor / NVML |

Il nome della GPU compare solo quando i suoi sensori vengono letti davvero
(altrimenti l'intestazione mostra solo `GPU`). I nomi vengono riletti di continuo:
se cambi scheda o processore il display si aggiorna da solo, senza riavviare nulla.

**Windows:** Windows non espone la temperatura della CPU. Scarica
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases),
avvialo **come amministratore** e lascialo aperto (anche ridotto a icona).
Lo script ne legge i sensori via WMI oppure, nelle versioni recenti, dal
server web integrato: in LHM attiva **Options → Remote Web Server → Run**
(porta 8085). Se LHM viene avviato dopo lo script, viene trovato entro 10 s.
Con più GPU viene scelta quella dedicata (NVIDIA > AMD > Intel, poi più VRAM).

### Windows: avvio automatico

**1. LibreHardwareMonitor all'avvio** - nel menu **Options** di LHM spunta:
`Start Minimized`, `Minimize To Tray`, `Run On Windows Startup`
(LHM crea da solo un'attività che parte come amministratore) e verifica che
**Remote Web Server → Run** resti attivo.

**2. Lo script all'avvio** - da un terminale nella cartella `pc`:

```powershell
powershell -ExecutionPolicy Bypass -File autostart_windows.ps1
```

Crea l'attività pianificata **"CYD Monitor"**: parte 20 s dopo il login,
gira in background senza finestra (`pythonw.exe`), si riavvia se va in errore
e ritrova da sola il CYD anche se lo colleghi dopo. Non servono permessi di
amministratore.

| Azione | Comando |
|---|---|
| Avviare subito | `Start-ScheduledTask -TaskName "CYD Monitor"` |
| Fermare (es. per caricare il firmware) | `Stop-ScheduledTask -TaskName "CYD Monitor"` |
| Rimuovere l'avvio automatico | `powershell -ExecutionPolicy Bypass -File autostart_windows.ps1 -Remove` |

L'attività si vede anche in **Utilità di pianificazione**. Finché è attiva la
porta COM è occupata: fermala prima di usare `monitor.py` a mano o l'Upload.

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
  quella con più VRAM. Il nome viene dal database `pci.ids` (pacchetto
  `hwdata` o `pciutils`, quasi sempre già installato) e indica la famiglia del
  chip: una RX 9070 XT appare come `RX 9070/9070 XT`.
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

Dopo aver creato l'ambiente virtuale (vedi sopra), dalla cartella `pc`:

```bash
chmod +x autostart_linux.sh     # solo se il file non è eseguibile
./autostart_linux.sh
```

Crea e avvia il servizio systemd utente **`cyd-monitor`** con i percorsi
corretti, ovunque si trovi il progetto. Il servizio:

- parte all'accensione del PC, anche senza login (chiede la password di
  `sudo` una sola volta per `loginctl enable-linger`)
- usa il Python di `.venv` e gira in background
- si riavvia se va in errore e ritrova da solo il CYD quando lo colleghi

Prima di installare controlla che le librerie Python ci siano e avvisa se
l'utente non è nel gruppo `dialout`.

> **Attenzione:** `autostart_linux.sh` è un **installatore da lanciare una sola
> volta a mano**, dal tuo utente e **senza `sudo`**. Non è il programma da
> tenere in esecuzione: non va messo in `ExecStart=` di un servizio creato a
> mano in `/etc/systemd/system/`, né in cron o in `rc.local`. Il servizio lo
> crea lui, in `~/.config/systemd/user/`, e lancia direttamente `monitor.py`.

Tutti i comandi qui sotto vogliono **`--user`**: senza, `systemctl` e
`journalctl` cercano un servizio di sistema che non esiste (o ne mostrano uno
sbagliato con lo stesso nome).

| Azione | Comando |
|---|---|
| Stato | `systemctl --user status cyd-monitor` |
| Log in tempo reale | `journalctl --user -u cyd-monitor -f` |
| Fermare (es. per caricare il firmware) | `systemctl --user stop cyd-monitor` |
| Riavviare (es. dopo aver aggiornato lo script) | `systemctl --user restart cyd-monitor` |
| Rimuovere l'avvio automatico | `./autostart_linux.sh --remove` |

Se sposti la cartella del progetto, rilancia `./autostart_linux.sh` per
aggiornare i percorsi.

Il log è quasi vuoto di proposito: l'output normale di `monitor.py` viene
scartato (`StandardOutput=null`), nel journal finiscono solo gli errori.

#### Errore `Failed to connect to user scope bus` / `status=1/FAILURE`

Se `journalctl -u cyd-monitor` (senza `--user`) mostra:

```
systemctl[…]: Failed to connect to user scope bus via local transport:
$DBUS_SESSION_BUS_ADDRESS and $XDG_RUNTIME_DIR not defined
cyd-monitor.service: Main process exited, code=exited, status=1/FAILURE
cyd-monitor.service: Start request repeated too quickly.
```

esiste un servizio **di sistema** che esegue `autostart_linux.sh` come
`ExecStart`. Lì dentro `systemctl --user` non ha una sessione utente a cui
collegarsi, quindi lo script esce con errore e systemd lo rilancia a vuoto.
Rimuovi quel servizio e installa quello giusto:

```bash
sudo systemctl disable --now cyd-monitor.service
sudo rm /etc/systemd/system/cyd-monitor.service
sudo systemctl daemon-reload

./autostart_linux.sh            # dal tuo utente, senza sudo
systemctl --user status cyd-monitor
```

Le versioni recenti dello script se ne accorgono da sole: lanciato con `sudo`,
da un servizio di sistema o da una sessione senza login dell'utente (es. `su`
da root) si ferma subito con un messaggio che spiega cosa fare.

## Protocollo

Una riga JSON per aggiornamento; i campi mancanti vengono mostrati come `--`:

```json
{"host":"PC","time":"16:09","cpu_name":"Ryzen 7 9800X3D","cpu_t":54.0,"cpu_u":12.5,"cpu_f":4700,
 "gpu_name":"RX 9070 XT","gpu_t":48,"gpu_u":7,"gpu_p":35.2,"vram_used":1.2,"vram_tot":12,
 "ram_u":39,"ram_used":24.2,"ram_tot":61.6,"net_up":2560,"net_dn":680,
 "up":273600,"procs":312}
```
