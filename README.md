# MultiObserver

MultiObserver is an open-source observer extension for **MeshCore Repeater**.
It is designed as a reusable, application-level library that adds Wi-Fi and
dual-broker MQTT observation to a MeshCore repeater without replacing or
forking MeshCore core functionality.

The observer receives repeater traffic, publishes packet and status telemetry
to up to two independently configured MQTT brokers, and exposes persistent
network configuration through the standard MeshCore CLI. It supports secure
MQTT-over-WebSocket connections (WSS/TLS), making it suitable for community or
private telemetry backends.

## Compatibility

MultiObserver is installed into a clean MeshCore repeater source tree and has
been validated with the `Heltec_v3_repeater` environment for:

| MeshCore Repeater release | Status |
| --- | --- |
| 1.15.0 | Installer verified and firmware builds successfully |
| 1.16.0 | Installer verified and firmware builds successfully |
| 1.17.1 | Installer verified and firmware builds successfully |

The integration deliberately keeps MeshCore core code intact. The installer
adds only the minimal build and application hooks; `MOBridge` is the thin
adapter between the repeater application and the MultiObserver library.

## What MultiObserver provides

- **Two independent MQTT broker profiles** with separate host, port,
  transport, username, password and enable/disable settings.
- **Wi-Fi and MQTT configuration through the MeshCore CLI**, stored persistently
  on the device.
- **MQTT over TCP or WSS/TLS**, with connection startup gated on usable Wi-Fi
  and valid system time.
- Packet, RAW and periodic status publication to every enabled broker.
- Serial diagnostics for Wi-Fi, SNTP, MQTT state changes and received/transmitted
  packets.
- Heltec V3 integration: Wi-Fi IP shown on the OLED, an EastMesh-style cached
  battery measurement, and a non-blocking white LED pulse for packet activity.

## Current status

The V1 implementation is active and builds successfully for the supported
MeshCore Repeater releases listed above.

Implemented runtime mechanisms include:

- persistent WiFi and dual-MQTT WSS/TLS configuration through the MeshCore CLI
- explicit WiFi, SNTP and MQTT state machines with transition diagnostics
- MQTT connection gating on both WiFi connectivity and sane system time
- bounded reconnect backoff, heap headroom checks and ESP-TLS error reporting
- packet, RAW and periodic status publication to both enabled brokers
- one-line RX/TX packet diagnostics in the serial monitor
- EastMesh-style 60-second battery sampling cache for Heltec V3 GPIO37
- non-blocking visible packet pulse on the Heltec V3 white GPIO35 LED
- WiFi IP address on the Heltec OLED status screen

Integration architecture:

- MeshCore 1.17.1 core remains unchanged in `src/`
- repeater integration lives in `examples/simple_repeater/`
- MultiObserver functionality lives in `lib/MultiObserver/`
- `MOBridge` provides the application-level adapter between the repeater and
  MultiObserver

## Repository layout

```text
MultiObserver/
├── examples/
│   └── simple_repeater/
│       ├── MOBridge.cpp
│       └── MOBridge.h
├── installer/
│   ├── mo_installer/
│   │   ├── cli.py
│   │   ├── common.py
│   │   ├── patches.py
│   │   ├── storage.py
│   │   ├── validation.py
│   │   └── workflow.py
│   ├── tests/
│   ├── install.py
│   ├── multiobserver_cpp17.py
│   └── verify.py
├── lib/
│   └── MultiObserver/
│       ├── library.json
│       └── src/
│           ├── MO.cpp
│           ├── MO.h
│           ├── MOConfig.cpp
│           ├── MOConfig.h
│           ├── MOWifi.cpp
│           ├── MOWifi.h
│           ├── MOWifiPrefs.cpp
│           ├── MOWifiPrefs.h
│           ├── MOMQTT.cpp
│           ├── MOMQTT.h
│           ├── MOMQTTPrefs.cpp
│           ├── MOMQTTPrefs.h
│           ├── MOCli.cpp
│           └── MOCli.h
├── install.ps1
└── Run-Installer.cmd
```

## Design principles

- preserve MeshCore compatibility
- make the smallest safe integration changes
- avoid unnecessary dependencies
- keep WiFi and MQTT lifecycle deterministic
- keep runtime configuration persistent
- do not duplicate existing MeshCore functionality without a reason
- keep secrets and local credentials out of Git
- keep `install.py` as a thin entry point and isolate patching, validation,
  backup and workflow responsibilities

## License

See `LICENSE`.


## Build-system integration

For `Heltec_v3_repeater`, the installer adds the local MultiObserver library,
enables the ESP32 certificate bundle and compiles the extension as C++17. The
extension uses the ESP-IDF MQTT client, so the unused Arduino
`WiFiClientSecure` library is excluded while the framework `WiFi` library
remains available to `MOWifi`.

The installer verifies that all 18 MeshCore 1.17.1 Heltec V3 environments are
preserved and creates a rollback backup before changing the target tree.

The installer never keeps a prepared device identity in the MO source tree.
When identity arguments are supplied, it writes the generated identity and
name directly into the selected MeshCore target's `data/` directory.

## Quick start (Windows / Heltec V3)

### Prerequisites

- A clean MeshCore repeater source tree. MeshCore **1.15.0**, **1.16.0** and
  **1.17.1** have been installed and compiled successfully with this release.
- Python 3 and PlatformIO (`py -3 -m platformio`).
- A USB-connected Heltec V3 board when you are ready to flash it.

### 1. Apply MultiObserver to MeshCore

From the `MultiObserver` directory, run:

```powershell
.\Run-Installer.cmd
```

Enter the path to the clean MeshCore source tree, the repeater name, and the
public/private identity keys when prompted. The installer creates a rollback
backup, adds the minimal application/build hooks, prepares the MeshCore
filesystem data (`data/identity/_main.id` and `data/prefs.json`), and verifies
the resulting tree.

For non-interactive or troubleshooting use:

```powershell
python installer/install.py <MeshCore-path> --source-root <MultiObserver-path>
python installer/verify.py <MeshCore-path>
```

### 2. Build the prepared MeshCore tree

Change to the **MeshCore tree selected in step 1**, then build the Heltec V3
repeater firmware:

```powershell
py -3 -m platformio run -e Heltec_v3_repeater
```

### 3. Flash firmware and configuration

After the first installation, or after an erase, upload both the firmware and
the prepared filesystem. Run these commands from the prepared MeshCore tree:

```powershell
py -3 -m platformio run -e Heltec_v3_repeater -t upload
py -3 -m platformio run -e Heltec_v3_repeater -t uploadfs
```

To completely erase the connected board first:

```powershell
py -3 -m platformio run -e Heltec_v3_repeater -t erase
```

`upload` flashes the program. `uploadfs` writes the identity, repeater name
and persistent configuration prepared by the installer; it is therefore also
required after an erase. Do not publish identity keys, Wi-Fi passwords or MQTT
credentials in a public repository.

### 4. Open the serial monitor

The serial monitor uses 115200 baud:

```powershell
py -3 -m platformio device monitor -b 115200
```

## EastMesh mechanism reference

EastMesh v2026.8.2 is used as a behavior reference, not as the integration
architecture. MultiObserver matches its essential V1 network mechanisms:
WiFi state transitions, SNTP readiness, MQTT network gating, event/error
diagnostics, connection backoff and one-broker-at-a-time startup. EastMesh-only
features such as gateway ARP watchdogs, WiFi channel hints and WSS preflight
remain outside the current V1 scope.

The Heltec fixes reproduce the observed EastMesh behavior without moving
business logic into MeshCore core: the installer adds only verified
application/build hooks, while `MOBridge` remains the thin adapter to MO.
