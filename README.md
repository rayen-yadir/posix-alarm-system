# POSIX Alarm System — Multi-process GPIO & IPC on Linux

> Système d'alarme embarqué multi-processus utilisant IPC POSIX,
> pthreads, GPIO et systemd — testé sur **Raspberry Pi (kernel 6.12, Raspberry Pi OS Trixie)**.

**Auteur :** Rayen Yadir

---

## Démonstration réelle sur Raspberry Pi

### Système complet en fonctionnement — 3 processus, logs en temps réel

![Système complet](docs/screenshots/full_system_logs.png)

*Logs du daemon (gauche) : bouton détecté → alarme déclenchée → état TRIGGERED → retour ARMED*

### Status après armement et déclenchement

![Status après déclenchement](docs/screenshots/status_after_trigger.png)

*`alarm_ctl status` affiche : ARMED, 1 déclenchement, date/heure du dernier déclenchement*

---

## Table des matières

- [Architecture](#architecture)
- [Technologies](#technologies)
- [Hardware](#hardware)
- [Installation et exécution](#installation-et-exécution)
- [Résultat attendu](#résultat-attendu)

---

## Architecture

```
posix-alarm-system/
├── common/
│   └── alarm_common.h              # Définitions partagées (IPC, structures)
├── daemon/
│   ├── alarm_daemon.c              # Processus principal multi-thread
│   └── Makefile
├── monitor/
│   ├── alarm_logger.c              # Processus de journalisation (SHM)
│   └── Makefile
├── cli/
│   ├── alarm_ctl.c                 # Outil CLI de contrôle
│   └── Makefile
└── scripts/
    ├── alarm-daemon.service        # Service systemd daemon
    ├── alarm-logger.service        # Service systemd logger
    └── install.sh                  # Script d'installation
```

### Schéma de communication

```
alarm_ctl ──[FIFO]──▶ alarm_daemon ──[SHM + sémaphore]──▶ alarm_logger
                           │
                    ┌──────┴──────┐
                    │             │
               Thread GPIO   Thread Actionneur
               (libgpiod v2)  (LED + Buzzer)
```

---

## Technologies

| Domaine | Outils / Concepts |
|---|---|
| **IPC POSIX** | Mémoire partagée (`shm_open`/`mmap`), FIFO nommés (`mkfifo`) |
| **Synchronisation** | Sémaphores POSIX nommés (`sem_open`), mutex, condition variables |
| **Threads** | pthreads, pattern producteur/consommateur, `pthread_cond_timedwait` |
| **GPIO** | libgpiod v2 (`gpiod_chip_open`, `gpiod_line_request_*`) |
| **Signaux Unix** | `SIGTERM`/`SIGINT`, arrêt coordonné multi-thread |
| **systemd** | Services avec dépendances (`Requires=`), restart automatique |
| **Plateforme** | Raspberry Pi — kernel 6.12, Raspberry Pi OS Trixie (Debian 13) |
| **Langage** | C (POSIX) |

> **Note kernel 6.12** : Le kernel 6.12 a supprimé l'accès GPIO via sysfs.
> Ce projet utilise **libgpiod v2** (API moderne recommandée) à la place de l'ancienne interface `/sys/class/gpio/`.

---

## Hardware

### Composants

| Composant | GPIO | Pin physique |
|---|---|---|
| Bouton poussoir | GPIO17 | Pin 11 |
| LED + résistance 220Ω | GPIO27 | Pin 13 |
| Buzzer actif | GPIO22 | Pin 15 |

### Câblage

```
Raspberry Pi                    Composants
──────────────────────────────────────────────
Pin 11 (GPIO17) ──────────────  Bouton ──── Pin 9  (GND)
Pin 13 (GPIO27) ──────────────  Résistance 220Ω ── LED ── Pin 14 (GND)
Pin 15 (GPIO22) ──────────────  Buzzer (+) ──────── Pin 20 (GND)
```

---

## Installation et exécution

### 1. Prérequis

```bash
sudo apt update
sudo apt install -y build-essential libgpiod-dev
```

### 2. Cloner et compiler

```bash
git clone https://github.com/rayen-yadir/posix-alarm-system.git
cd posix-alarm-system

cd daemon && make && cd ..
cd monitor && make && cd ..
cd cli && make && cd ..
```

### 3. Lancer les 3 processus

**Terminal 1 — Daemon principal**
```bash
sudo ./daemon/alarm_daemon &
journalctl -f | grep alarm
```

**Terminal 2 — Logger**
```bash
sudo ./monitor/alarm_logger &
```

**Terminal 3 — Contrôle CLI**
```bash
sudo ./cli/alarm_ctl arm
sudo ./cli/alarm_ctl status
# Appuie sur le bouton GPIO17 → LED clignote + buzzer sonne 5s
sudo ./cli/alarm_ctl status
sudo ./cli/alarm_ctl disarm
```

### 4. Installer avec systemd (démarrage automatique au boot)

```bash
sudo cp scripts/alarm-daemon.service /etc/systemd/system/
sudo cp scripts/alarm-logger.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable alarm-daemon.service alarm-logger.service
sudo systemctl start alarm-daemon.service alarm-logger.service
```

---

## Résultat attendu

```bash
$ sudo ./cli/alarm_ctl status
État          : ARMED
Déclenchements: 1
LED           : OFF
Buzzer        : OFF
Dernier décl. : Tue Jun 23 20:56:58 2026
```

Logs en temps réel :
```
alarm_daemon: GPIO initialises avec libgpiod v2 (/dev/gpiochip0)
alarm_daemon: Thread GPIO monitor demarre (pin 17)
alarm_daemon: Demon pret - etat initial: DISARMED
alarm_daemon: Bouton appuye detecte !
alarm_daemon: Activation de l'alarme (LED + buzzer)
alarm_logger: Changement d'etat: ARMED -> TRIGGERED
alarm_logger: Declenchement #1 detecte
alarm_logger: Changement d'etat: TRIGGERED -> ARMED
```

---

## Auteur

**Rayen Yadir** — Ingénieur Linux Embarqué / BSP

[![GitHub](https://img.shields.io/badge/GitHub-rayen--yadir-black?logo=github)](https://github.com/rayen-yadir)
