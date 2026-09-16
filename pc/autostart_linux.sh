#!/usr/bin/env bash
# Avvio automatico di monitor.py su Linux (servizio systemd utente).
#
#   ./autostart_linux.sh            # installa e avvia
#   ./autostart_linux.sh --remove   # rimuove
#
# Il servizio "cyd-monitor" parte all'accensione del PC (anche senza login),
# si riavvia se va in errore e ritrova da solo il CYD quando viene collegato.
# Non servono permessi di root (tranne "loginctl enable-linger", vedi sotto).
set -euo pipefail

SERVICE=cyd-monitor
UNIT_DIR="$HOME/.config/systemd/user"
UNIT="$UNIT_DIR/$SERVICE.service"
PC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${1:-}" == "--remove" ]]; then
    systemctl --user disable --now "$SERVICE" 2>/dev/null || true
    rm -f "$UNIT"
    systemctl --user daemon-reload
    echo "Avvio automatico rimosso."
    exit 0
fi

# Usa il Python dell'ambiente virtuale se esiste (consigliato), altrimenti quello di sistema
if [[ -x "$PC_DIR/.venv/bin/python" ]]; then
    PYTHON="$PC_DIR/.venv/bin/python"
else
    PYTHON="$(command -v python3)"
    echo "Attenzione: .venv non trovato, uso $PYTHON (vedi README: 'Python in un ambiente virtuale')"
fi

# Verifica che le librerie ci siano prima di installare il servizio
if ! "$PYTHON" -c "import psutil, serial, rich" 2>/dev/null; then
    echo "Errore: mancano le librerie Python. Esegui prima:"
    echo "  python3 -m venv $PC_DIR/.venv && $PC_DIR/.venv/bin/pip install -r $PC_DIR/requirements.txt"
    exit 1
fi

if ! id -nG | grep -qwE 'dialout|uucp'; then
    echo "Attenzione: l'utente non è nel gruppo 'dialout' (Arch: 'uucp'): la porta seriale"
    echo "potrebbe non essere accessibile. Vedi README: 'Permessi della porta seriale'."
fi

mkdir -p "$UNIT_DIR"
cat > "$UNIT" <<EOF
[Unit]
Description=CYD Hardware Monitor (display ESP32)

[Service]
ExecStart="$PYTHON" "$PC_DIR/monitor.py" --no-ui
WorkingDirectory=$PC_DIR
StandardOutput=null
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now "$SERVICE"

# Senza "linger" i servizi utente partono solo dopo il login
if [[ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" != "yes" ]]; then
    echo "Abilito l'avvio anche senza login (richiede la password di sudo)..."
    sudo loginctl enable-linger "$USER" || echo "Non riuscito: il servizio partirà solo dopo il login."
fi

echo
echo "Avvio automatico installato: $UNIT"
echo "  Stato:     systemctl --user status $SERVICE"
echo "  Log:       journalctl --user -u $SERVICE -f"
echo "  Fermare:   systemctl --user stop $SERVICE     (es. prima dell'upload del firmware)"
echo "  Rimuovere: $0 --remove"
