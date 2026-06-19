#!/bin/bash

set -e

echo "=== Installation du système d'alarme POSIX ==="

# Compilation
echo "[1/4] Compilation..."
cd daemon && make && cd ..
cd monitor && make && cd ..
cd cli && make && cd ..

# Installation des binaires
echo "[2/4] Installation des binaires..."
sudo cp daemon/alarm_daemon /usr/local/bin/
sudo cp monitor/alarm_logger /usr/local/bin/
sudo cp cli/alarm_ctl /usr/local/bin/

# Installation des services systemd
echo "[3/4] Installation des services systemd..."
sudo cp scripts/alarm-daemon.service /etc/systemd/system/
sudo cp scripts/alarm-logger.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable alarm-daemon.service
sudo systemctl enable alarm-logger.service

# Démarrage
echo "[4/4] Démarrage des services..."
sudo systemctl start alarm-daemon.service
sudo systemctl start alarm-logger.service

echo ""
echo "=== Installation terminée ==="
echo "Vérifier : sudo systemctl status alarm-daemon.service"
echo "Logs     : journalctl -u alarm-daemon -f"
echo "Contrôle : alarm_ctl {arm|disarm|status|reset}"