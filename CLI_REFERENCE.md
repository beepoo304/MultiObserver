# MultiObserver CLI reference

This document is the compact command reference for the MultiObserver commands
implemented by `MOCli`. It covers MultiObserver only; the upstream MeshCore
repeater may provide additional commands of its own.

The reference applies to MultiObserver v0.2.x and contains 41 commands.

## Opening the CLI

Connect the repeater by USB and run the monitor from the prepared MeshCore
directory:

```powershell
py -3 -m platformio device monitor -b 115200
```

If more than one serial device is connected, select the port explicitly:

```powershell
py -3 -m platformio device monitor -p COM3 -b 115200
```

Enter one command per line. Commands are case-sensitive where shown and must
not exceed 159 characters. Press `Ctrl+C` to close the monitor.

MultiObserver settings changed by `set` commands are stored persistently and
survive a normal reboot or power loss. Passwords cannot be read back. The
private AlertChannel key can be read with `get channel.key`, so use that
command only on a trusted serial or authenticated MeshCore CLI session.

## Common replies

| Reply | Meaning |
| --- | --- |
| `OK` | The value or state was accepted and saved, or the requested runtime restart was started. |
| `ERR` | Validation, persistence or queue submission failed. The command was recognized but not completed. |
| `CLEAR` | The WDG event log is empty, or it was cleared successfully. |
| `QUEUED` | MeshCore's native outbound queue accepted the encrypted AC message. It is not a delivery ACK. |

An unsupported or malformed command may be left to the upstream MeshCore CLI
instead of producing a MultiObserver reply.

## Wi-Fi commands

| Command | Reply | Description |
| --- | --- | --- |
| `set wifi.ssid <SSID>` | `OK` / `ERR` | Save a non-empty SSID and reconnect using the saved credentials. |
| `set wifi.pwd <password>` | `OK` / `ERR` | Save a non-empty Wi-Fi password and reconnect. |
| `get wifi.ssid` | `SSID <value>` | Show the configured SSID. An empty value is returned as `SSID `. |
| `get wifi.status` | status line | Show lifecycle state, SSID, Wi-Fi status code and NTP state; a connected reply also contains IP, channel and RSSI. |
| `restart.wifi` | `OK` | Restart only the MultiObserver Wi-Fi connection using saved credentials. |

Wi-Fi is healthy for WDG when the station is connected and both its local IP
address and default gateway are non-zero. WDG does not generate an Internet
probe.

## MQTT broker 1 commands

| Command | Reply | Description |
| --- | --- | --- |
| `set mqtt1.on` | `OK` / `ERR` | Persistently enable MQTT1 and start/restart its client. |
| `set mqtt1.off` | `OK` / `ERR` | Persistently disable MQTT1 and disconnect its client. |
| `set mqtt1.host <hostname>` | `OK` / `ERR` | Save a non-empty hostname and restart MQTT1. Do not include a URI scheme. |
| `set mqtt1.port <1-65535>` | `OK` / `ERR` | Save a numeric TCP port and restart MQTT1. |
| `set mqtt1.transport tcp` | `OK` / `ERR` | Use direct MQTT over TCP and restart MQTT1. |
| `set mqtt1.transport wss` | `OK` / `ERR` | Use MQTT over secure WebSocket/TLS and restart MQTT1. The URI path is `/mqtt`. |
| `set mqtt1.username <username>` | `OK` / `ERR` | Save a non-empty username and restart MQTT1. |
| `set mqtt1.pwd <password>` | `OK` / `ERR` | Save a non-empty password and restart MQTT1. |
| `get mqtt1.status` | status line | Show ON/OFF, runtime state, host, reconnect-failure count and last error code. |
| `restart.mqtt1` | `OK` | Restart only MQTT1 using all saved settings. |

## MQTT broker 2 commands

MQTT2 has an independent configuration and runtime state. It uses exactly the
same command structure as MQTT1.

| Command | Reply | Description |
| --- | --- | --- |
| `set mqtt2.on` | `OK` / `ERR` | Persistently enable MQTT2 and start/restart its client. |
| `set mqtt2.off` | `OK` / `ERR` | Persistently disable MQTT2 and disconnect its client. |
| `set mqtt2.host <hostname>` | `OK` / `ERR` | Save a non-empty hostname and restart MQTT2. Do not include a URI scheme. |
| `set mqtt2.port <1-65535>` | `OK` / `ERR` | Save a numeric TCP port and restart MQTT2. |
| `set mqtt2.transport tcp` | `OK` / `ERR` | Use direct MQTT over TCP and restart MQTT2. |
| `set mqtt2.transport wss` | `OK` / `ERR` | Use MQTT over secure WebSocket/TLS and restart MQTT2. The URI path is `/mqtt`. |
| `set mqtt2.username <username>` | `OK` / `ERR` | Save a non-empty username and restart MQTT2. |
| `set mqtt2.pwd <password>` | `OK` / `ERR` | Save a non-empty password and restart MQTT2. |
| `get mqtt2.status` | status line | Show ON/OFF, runtime state, host, reconnect-failure count and last error code. |
| `restart.mqtt2` | `OK` | Restart only MQTT2 using all saved settings. |

Possible MQTT runtime states are `DISABLED`, `WAITING_WIFI`, `WAITING_TIME`,
`DISCONNECTED`, `CONNECTING`, `CONNECTED`, `BACKOFF` and `ERROR`.

An MQTT broker is supervised by WDG only when it is enabled and has a valid
host and port. While Wi-Fi is down, both brokers are treated as dependent
services and their independent escalation timers do not advance.

## Observer location / IATA commands

| Command | Reply | Description |
| --- | --- | --- |
| `set mqtt.iata <ABC>` | `OK` / `ERR` | Save the three-letter uppercase IATA code used in MQTT telemetry. |
| `get mqtt.iata` | `<ABC>` | Show the saved IATA code. |

The IATA value must contain exactly three uppercase alphabetic characters,
for example `KTW`.

## Watchdog commands

| Command | Reply | Description |
| --- | --- | --- |
| `get wdg.status` | status line | Show WDG ON/OFF, phase, Wi-Fi/MQTT health and whether a reboot is pending. |
| `set wdg.on` | `OK` / `ERR` | Persistently enable WDG and start a fresh grace period. |
| `set wdg.off` | `OK` / `ERR` | Persistently disable WDG, clear escalation state and cancel a pending WDG reboot. |
| `get wdg.grace` | `WDG GRACE <seconds>` | Show the saved startup grace period. |
| `set wdg.grace <120-300>` | `OK` / `ERR` | Save a valid grace period and restart the WDG state machine in GRACE. |
| `restart.wdg` | `OK` | Reset only WDG escalation and start a fresh grace period. The ESP is not rebooted. |
| `get wdg.log` | log / `CLEAR` | Return at most the last five short events from the current local repeater day. |
| `set wdg.log 1` | `CLEAR` / `ERR` | Clear the persistent WDG event log. The literal value `1` is required. |

WDG phases shown by `get wdg.status` are:

| Phase | Meaning |
| --- | --- |
| `GRACE` | Startup protection is active; WDG is waiting before supervision begins. |
| `NORMAL` | Normal staged supervision and recovery are active. |
| `SILENT` | The previous reboot was requested by WDG and a failed service is checked every 15 minutes without another reboot loop. |

MQTT status inside the WDG reply is `OFF` when that broker is not configured
for supervision. A pending final-stage reboot is reported as `reboot=PENDING`.

The full Wi-Fi/MQTT escalation timings, reboot marker and silent-mode recovery
rules are documented in the [main README](README.md#watchdog).

## Private AlertChannel commands

| Command | Reply | Description |
| --- | --- | --- |
| `get channel.status` | channel status | Show `CHANNEL OFF`, `CHANNEL READY` or `CHANNEL KEY/SENDER MISSING`. |
| `set channel.on` | `OK` / `ERR` | Persistently enable AlertChannel. A valid key and MeshCore sender are still required for READY. |
| `set channel.off` | `OK` / `ERR` | Persistently disable AlertChannel. |
| `get channel.key` | key / `CHANNEL KEY CLEAR` | Return the saved private-channel key. Treat the reply as secret. |
| `set channel.key <32-or-64-hex>` | `OK` / `ERR` | Validate and persist a 128-bit or 256-bit private-channel key written as hexadecimal. |
| `test.channel` | `QUEUED` / `ERR` | Create an encrypted test message containing local repeater time and concise WDG status, then submit it to MeshCore's native queue. |

`QUEUED` means the native MeshCore outbound queue accepted the packet. AC does
not control the radio, wait for airtime, retry independently or wait for an
ACK. Delivery therefore cannot be inferred from the CLI reply alone.

After each completed WDG grace period, an enabled and ready AC attempts to
queue:

```text
AlertChannel newStart YYYY-MM-DD HH:MM:SS
```

AC and WDG-log text uses local repeater time. The default build uses automatic
CET/CEST daylight-saving rules. MeshCore packet epochs and MQTT timestamps
remain UTC.

## Recommended first configuration

Use placeholders until entering the commands on your own trusted device. Do
not commit real credentials or keys to a public repository.

```text
set wifi.ssid YourWiFiName
set wifi.pwd YourWiFiPassword

set mqtt1.host mqtt-primary.example.org
set mqtt1.port 443
set mqtt1.transport wss
set mqtt1.username YourMqttUser
set mqtt1.pwd YourMqttPassword
set mqtt1.on

set mqtt2.host mqtt-secondary.example.org
set mqtt2.port 8883
set mqtt2.transport tcp
set mqtt2.username YourMqttUser
set mqtt2.pwd YourMqttPassword
set mqtt2.on

set mqtt.iata ABC

set channel.key 00112233445566778899AABBCCDDEEFF
set channel.on

set wdg.grace 120
set wdg.on
```

Verify the final state:

```text
get wifi.status
get mqtt1.status
get mqtt2.status
get mqtt.iata
get channel.status
get wdg.grace
get wdg.status
get wdg.log
test.channel
```

The example channel key is a public placeholder and must never be used as a
real private-channel secret.

## Complete command count

| Group | Commands |
| --- | ---: |
| Wi-Fi | 5 |
| MQTT1 | 10 |
| MQTT2 | 10 |
| IATA | 2 |
| WDG | 8 |
| AlertChannel | 6 |
| **Total** | **41** |
