# MultiObserver

MultiObserver is an open-source Observer extension for MeshCore 1.17.1.

The project is designed as a small application-level extension rather than a
replacement for MeshCore core functionality.

## Current status

The V1 implementation is active and builds successfully for MeshCore 1.17.1
`Heltec_v3_repeater`.

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

Target integration:

- MeshCore 1.17.1 core remains unchanged in `src/`
- repeater integration lives in `examples/simple_repeater/`
- MultiObserver functionality lives in `lib/MultiObserver/`
- `MOBridge` provides the application-level adapter between the repeater and
  MultiObserver

## Repository layout

```text
MO/
├── examples/
│   └── simple_repeater/
│       ├── MOBridge.cpp
│       └── MOBridge.h
├── installer/
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

## Installation

On Windows, run `Run-Installer.cmd` and provide the clean MeshCore source path,
repeater name and identity keys when prompted. The wrapper runs installation
and verification consecutively.

For scripted use:

```powershell
python installer/install.py <MeshCore-path> --source-root <MO-path>
python installer/verify.py <MeshCore-path>
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
