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
- A staged Wi-Fi/MQTT watchdog with a configurable startup grace period,
  bounded recovery actions and loop-safe post-reboot silent monitoring.
- An encrypted private AlertChannel that uses MeshCore's native outbound queue
  for startup, outage and recovery notifications.

## Current status

The V2 implementation is active and builds successfully for the supported
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
- staged Wi-Fi, MQTT1 and MQTT2 supervision with one-shot reboot attribution
- a current-day watchdog event log with the last five entries available by CLI
- 128-bit and 256-bit private MeshCore AlertChannel keys

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
│           ├── MOCli.h
│           ├── MOEtap2Prefs.cpp
│           ├── MOEtap2Prefs.h
│           ├── MOAlertChannel.cpp
│           ├── MOAlertChannel.h
│           ├── MOWatchdog.cpp
│           └── MOWatchdog.h
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

This is the recommended complete path for a new device. Commands marked
**MultiObserver directory** are run in the cloned `MultiObserver` repository.
Commands marked **prepared MeshCore directory** are run in the MeshCore source
tree selected by the installer.

### Prerequisites

- A clean MeshCore repeater source tree. MeshCore **1.15.0**, **1.16.0** and
  **1.17.1** have been installed and compiled successfully with this release.
- Python 3 and PlatformIO (`py -3 -m platformio`).
- A USB-connected Heltec V3 board when you are ready to flash it.

### 0. Get MultiObserver

Open PowerShell and clone the repository where you keep your projects:

```powershell
git clone https://github.com/beepoo304/MultiObserver.git
cd .\MultiObserver
```

If you already downloaded or cloned MultiObserver, simply open PowerShell in
that directory instead.

### 1. Apply MultiObserver to MeshCore

In the **MultiObserver directory**, start the interactive installer:

```powershell
.\Run-Installer.cmd
```

When prompted, enter:

1. The full path to a **clean MeshCore Repeater source tree** (for example,
   `C:\Projects\MeshCore-repeater-v1.17.1`).
2. The repeater name.
3. The 64-character public identity key.
4. The 128-character private identity key.

The installer creates a rollback backup, adds the minimal application/build
hooks, prepares the MeshCore filesystem data (`data/identity/_main.id` and
`data/prefs.json`), then verifies the resulting tree. Keep the identity keys
private: never commit or paste them into a public issue.

For non-interactive or troubleshooting use:

```powershell
python installer/install.py <MeshCore-path> --source-root <MultiObserver-path>
python installer/verify.py <MeshCore-path>
```

### 2. Build the prepared MeshCore tree

In PowerShell, change to the **prepared MeshCore directory selected in step
1**. Replace the example path with your own:

```powershell
cd "C:\Projects\MeshCore-repeater-v1.17.1"
py -3 -m platformio run -e Heltec_v3_repeater
```

Wait for `SUCCESS` before continuing.

### 3. Flash firmware and configuration

With the Heltec V3 connected by USB, and still in the **prepared MeshCore
directory**, upload both the firmware and the filesystem:

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

Still in the **prepared MeshCore directory**, open the serial monitor at 115200
baud:

```powershell
py -3 -m platformio device monitor -b 115200
```

Press `Ctrl+C` to leave the monitor.

### 5. Configure Wi-Fi and brokers

Use the MeshCore CLI in the serial monitor to configure Wi-Fi and each broker.
The values below are placeholders: use your own network and broker values.

```text
set wifi.ssid YourWiFiName
set wifi.pwd YourWiFiPassword

set mqtt1.on
set mqtt1.host mqtt.example.org
set mqtt1.port 443
set mqtt1.transport wss
set mqtt1.username YourMqttUser
set mqtt1.pwd YourMqttPassword

set mqtt2.on
set mqtt2.host backup-mqtt.example.org
set mqtt2.port 8883
set mqtt2.transport tcp
set mqtt2.username YourMqttUser
set mqtt2.pwd YourMqttPassword
```

Verify the current configuration and connection state with:

```text
get wifi.status
get mqtt1.status
get mqtt2.status
```

Configuration is retained on the device. Restart Wi-Fi or an individual
broker after changing settings when needed:

```text
restart.wifi
restart.mqtt1
restart.mqtt2
```

## MultiObserver CLI command reference

Enter these commands in the MeshCore serial monitor. All settings are saved
persistently. Commands and values are case-sensitive where shown.

### Wi-Fi

| Command | Description |
| --- | --- |
| `set wifi.ssid <SSID>` | Save the Wi-Fi network name and reconnect. |
| `set wifi.pwd <password>` | Save the Wi-Fi password and reconnect. |
| `get wifi.ssid` | Show the configured Wi-Fi network name. |
| `get wifi.status` | Show Wi-Fi state, IP address and diagnostic details. |
| `restart.wifi` | Restart Wi-Fi using the saved credentials. |

### MQTT broker 1

| Command | Description |
| --- | --- |
| `set mqtt1.on` | Enable broker 1 and start connecting. |
| `set mqtt1.off` | Disable broker 1 and disconnect it. |
| `set mqtt1.host <hostname>` | Set the broker hostname. |
| `set mqtt1.port <1-65535>` | Set the broker port. |
| `set mqtt1.transport tcp` | Use direct MQTT over TCP. |
| `set mqtt1.transport wss` | Use secure MQTT over WebSocket (WSS/TLS). |
| `set mqtt1.username <username>` | Set the MQTT username. |
| `set mqtt1.pwd <password>` | Set the MQTT password. |
| `get mqtt1.status` | Show enabled/disabled state, connection state, host, failures and last error. |
| `restart.mqtt1` | Restart broker 1 using the saved settings. |

### MQTT broker 2

Broker 2 uses the same commands as broker 1. Replace `mqtt1` with `mqtt2`:

| Command | Description |
| --- | --- |
| `set mqtt2.on` / `set mqtt2.off` | Enable or disable broker 2. |
| `set mqtt2.host <hostname>` | Set the broker hostname. |
| `set mqtt2.port <1-65535>` | Set the broker port. |
| `set mqtt2.transport tcp` | Use direct MQTT over TCP. |
| `set mqtt2.transport wss` | Use secure MQTT over WebSocket (WSS/TLS). |
| `set mqtt2.username <username>` | Set the MQTT username. |
| `set mqtt2.pwd <password>` | Set the MQTT password. |
| `get mqtt2.status` | Show broker 2 connection and diagnostic state. |
| `restart.mqtt2` | Restart broker 2 using the saved settings. |

### Observer location / IATA code

| Command | Description |
| --- | --- |
| `set mqtt.iata <ABC>` | Set the three-letter uppercase IATA location code used by MQTT telemetry. |
| `get mqtt.iata` | Show the saved IATA location code. |

For example, use `set mqtt.iata KTW` for Katowice. The code must contain
exactly three uppercase letters.

### Watchdog

The watchdog starts after a configurable 120-300 second grace period. Wi-Fi
health requires a connected station, a non-zero local address and a non-zero
default gateway; no Internet probe is generated. Enabled, fully configured
MQTT brokers are supervised only while Wi-Fi is healthy. MQTT health requires
both an active session and fresh acknowledgement of a periodic status publish.

| Command | Description |
| --- | --- |
| `get wdg.status` | Show WDG mode, phase, service health and pending reboot state. |
| `set wdg.on` | Enable WDG persistently and start a fresh grace period. |
| `set wdg.off` | Disable WDG, cancel escalation and cancel a pending WDG reboot. |
| `get wdg.grace` | Show the saved startup grace period in seconds. |
| `set wdg.grace <120-300>` | Save the grace period and restart WDG grace. |
| `restart.wdg` | Reset only the WDG state machine and grace timer; do not reboot the ESP. |
| `get wdg.log` | Return the last five short events from the current repeater-clock day. |
| `set wdg.log 1` | Clear the WDG event log. |

Wi-Fi recovery escalates after 3, 5 and 30 minutes. MQTT1 and MQTT2 are
supervised independently and escalate after 3, 5, 15 and 30 minutes. The last
stage queues an encrypted alert and schedules one ESP reboot three minutes
later. A persistent one-shot marker distinguishes that reboot from manual
restart or power loss. If the failed service is still down after the next
grace period, WDG enters silent mode and checks every 15 minutes without
further restarts or reboot loops. A `*.restore` alert is sent only after
recovery from silent mode; its duration starts at entry into silent mode.

#### WDG escalation sequence

WDG always gives Wi-Fi priority. If Wi-Fi is down, MQTT1 and MQTT2 are treated
as dependent services and their independent escalation timers do not run.
Every successful recovery clears that service's current escalation state.

Wi-Fi is checked every 5 seconds:

| Consecutive stage | Action if Wi-Fi is still down |
| --- | --- |
| Monitor for 3 minutes | Call the existing MultiObserver Wi-Fi restart. |
| Monitor for another 5 minutes | Restart Wi-Fi a second time. |
| Monitor for another 30 minutes | Queue `Restart.ESP.wifi.down`. |
| Wait another 3 minutes | Persist the one-shot WDG marker and reboot the ESP once. |

Each enabled and correctly configured MQTT broker is checked every 15 seconds
while Wi-Fi is healthy. MQTT1 and MQTT2 have separate state machines:

| Consecutive stage | Action if that MQTT broker is still unhealthy |
| --- | --- |
| Monitor for 3 minutes | Restart that MQTT client. |
| Monitor for another 5 minutes | Restart that MQTT client again. |
| Monitor for another 15 minutes | Restart that MQTT client a third time. |
| Monitor for another 30 minutes | Queue `Restart.ESP.mqtt1.down` or `Restart.ESP.mqtt2.down`. |
| Wait another 3 minutes | Persist the one-shot WDG marker and reboot the ESP once. |

Only one reboot can be pending globally. Disabling WDG or running
`restart.wdg` cancels a pending reboot and clears all escalation states;
`restart.wdg` then starts a new grace period without rebooting the ESP.
A successful recovery during the final three-minute delay also cancels the
pending reboot owned by that service.

On boot, WDG consumes and deletes its reboot marker immediately. After grace,
`AlertChannel newStart` is queued. If a supervised service is still down and
that consumed marker identified this as the WDG-requested reboot, WDG enters
silent mode. Silent mode performs no service restart and schedules no further
ESP reboot. It checks every 15 minutes. Once a failed service recovers, AC
queues `wifi.restore`, `mqtt1.restore` or `mqtt2.restore` with a duration
measured only from entry into silent mode. Normal/manual boot and power loss do
not activate silent mode.

### Private AlertChannel

Create a private channel in a MeshCore client and copy its secret key. Standard
MeshCore private channels normally expose a 32-hex-character (128-bit) key;
64-hex-character (256-bit) keys are accepted as well. Treat this key as a
secret. MultiObserver never publishes it to serial diagnostics.

| Command | Description |
| --- | --- |
| `get channel.status` | Show whether AlertChannel is off, ready or missing a key/sender. |
| `set channel.on` | Enable AlertChannel persistently. |
| `set channel.off` | Disable AlertChannel persistently. |
| `get channel.key` | Return the saved channel key over the authenticated MeshCore CLI. |
| `set channel.key <32-or-64-hex>` | Save a private MeshCore channel key. |
| `test.channel` | Queue `test alertchannel`, time and concise WDG state (maximum 130 characters). |

After each grace period AlertChannel attempts to queue `AlertChannel newStart`
with repeater time. Alert messages are encrypted as MeshCore group datagrams
and handed directly to the repeater's existing scoped outbound queue.
MultiObserver does not maintain a second queue, retry alerts, wait for ACKs or
control the radio directly.

#### How AlertChannel works

1. `set channel.key` validates and persistently stores a 128-bit or 256-bit
   MeshCore private-channel secret. The key is never printed in serial logs.
2. `set channel.on` enables delivery. A missing/invalid key, disabled channel
   or unavailable MeshCore sender makes an alert fail immediately and produces
   only a short diagnostic; the watchdog itself never blocks.
3. AC adds the repeater-clock date and time and limits the complete alert text
   to 130 characters.
4. The thin `MOBridge` passes the decoded key and text to `MyMesh`.
5. `MyMesh` calculates the standard channel hash and calls MeshCore's
   `createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, ...)`.
6. The encrypted packet is submitted with `sendFloodScoped(...)`, so ordering,
   airtime and actual transmission remain owned by MeshCore's normal outbound
   dispatcher.

AC intentionally does not know whether the radio later transmitted the packet.
There is no AC retry, delivery ACK or parallel queue. This prevents an alert
failure from creating a second watchdog/reboot loop and keeps MultiObserver a
thin application layer over MeshCore.

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
