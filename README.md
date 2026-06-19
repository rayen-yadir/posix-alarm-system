# POSIX Alarm System — Multi-process GPIO & IPC on Linux

> Système d'alarme embarqué multi-processus utilisant IPC POSIX,
> pthreads, GPIO sysfs et systemd sur Raspberry Pi / Linux.

---

## Table des matières

- [Architecture](#architecture)
- [Technologies](#technologies)
- [Prérequis](#prérequis)
- [Installation et exécution](#installation-et-exécution)
- [Résultat attendu](#résultat-attendu)
- [Auteur](#auteur)

---

## Architecture
posix-alarm-system/

├── common/

│   └── alarm_common.h             # Définitions partagées (IPC, structures)

├── daemon/

│   ├── alarm_daemon.c             # Processus principal multi-thread

│   └── Makefile

├── monitor/

│   ├── alarm_logger.c             # Processus de journalisation (SHM)

│   └── Makefile

├── cli/

│   ├── alarm_ctl.c                # Outil CLI de contrôle

│   └── Makefile

└── scripts/

├── alarm-daemon.service       # Service systemd daemon

├── alarm-logger.service       # Service systemd logger

└── install.sh                 # Script d'installation

### Schéma de communication
alarm_ctl ──[FIFO]──▶ alarm_daemon ──[SHM + sémaphore]──▶ alarm_logger

│

┌──────┴──────┐

│             │

Thread GPIO   Thread Actionneur

(poll() IRQ)  (LED + Buzzer)

---

## Technologies

| Domaine | Outils / Concepts |
|---|---|
| **IPC POSIX** | Mémoire partagée (`shm_open`/`mmap`), FIFO nommés (`mkfifo`) |
| **Synchronisation** | Sémaphores POSIX nommés (`sem_open`), mutex, condition variables |
| **Threads** | pthreads, pattern producteur/consommateur, `pthread_cond_timedwait` |
| **GPIO** | Détection par `poll()` sur sysfs (pas de polling actif) |
| **Signaux Unix** | `SIGTERM`/`SIGINT`, arrêt coordonné multi-thread |
| **systemd** | Services avec dépendances (`Requires=`), restart automatique |
| **Langage** | C (POSIX) |

---

## Prérequis

### Hardware
- Raspberry Pi 3 ou 4
- Bouton sur **GPIO17**, LED sur **GPIO27**, Buzzer sur **GPIO22**

### Software
```bash
sudo apt update
sudo apt install build-essential
```

---

## Installation et exécution

### Option 1 — Script automatique
```bash
chmod +x scripts/install.sh
./scripts/install.sh
```

### Option 2 — Manuel

#### 1. Compiler les 3 composants
```bash
cd daemon/ && make && cd ..
cd monitor/ && make && cd ..
cd cli/ && make && cd ..
```

#### 2. Tester manuellement
```bash
# Terminal 1 : lancer le daemon
sudo ./daemon/alarm_daemon

# Terminal 2 : lancer le logger
sudo ./monitor/alarm_logger

# Terminal 3 : contrôler via CLI
./cli/alarm_ctl status
./cli/alarm_ctl arm
./cli/alarm_ctl status
./cli/alarm_ctl disarm
```

#### 3. Installer avec systemd
```bash
sudo cp daemon/alarm_daemon /usr/local/bin/
sudo cp monitor/alarm_logger /usr/local/bin/
sudo cp cli/alarm_ctl /usr/local/bin/
sudo cp scripts/alarm-daemon.service /etc/systemd/system/
sudo cp scripts/alarm-logger.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable alarm-daemon.service alarm-logger.service
sudo systemctl start alarm-daemon.service alarm-logger.service
```

---

## Résultat attendu

```bash
# Vérifier l'état
$ alarm_ctl status
État          : DISARMED
Déclenchements: 0
LED           : OFF
Buzzer        : OFF

# Armer le système
$ alarm_ctl arm
Commande 'arm' envoyée

# Appuyer sur le bouton GPIO17 -> LED + Buzzer clignotent 5s

# Vérifier après déclenchement
$ alarm_ctl status
État          : ARMED
Déclenchements: 1
Dernier décl. : Sat Jan 12 14:23:01 2025

# Logs en temps réel
$ journalctl -u alarm-daemon -f
Jan 12 14:23:01 raspberrypi alarm_daemon[1234]: Bouton appuyé détecté !
Jan 12 14:23:01 raspberrypi alarm_daemon[1234]: Activation de l'alarme (LED + buzzer)
```

---

## Auteur

**Rayen** — Ingénieur Linux Embarqué / BSP

[![GitHub](https://img.shields.io/badge/GitHub-profil-black?logo=github)](https://github.com/rayen-yadir)