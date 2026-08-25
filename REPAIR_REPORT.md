# MultiObserver repair pass — 2026-08-25

Applied corrections derived from RULES.md, the saved implementation discussion and the supplied EastMesh/MeshCore sources.

## Corrected
- MQTT startup/runtime state logging.
- MQTT online/offline status publication.
- MQTT LWT on status topic, QoS 1, retained.
- periodic STATUS publication (300 s) when a status snapshot is available.
- numeric STATUS stats fields.
- MQTT runtime status returned by `get mqtt1.status` / `get mqtt2.status`.
- explicit MQTT restart semantics with runtime force-enable, without changing persisted configuration.
- RAW packet publication as a separate `/raw` stream.
- TX packet metadata path for RSSI/SNR/time/date via the bridge overload.
- fixed broken `MOMQTT.h` declaration and missing MQTT constants.
- status snapshot bridge API using actual MeshCore repeater statistics.
- startup markers for MO, WiFi and both MQTT runtimes.
- installer TX/status hooks.
- installer first-boot identity preparation: `/identity/_main.id` (128 bytes) and `/prefs.json` with repeater name.
- installer input validation for repeater name, public key and private key.
- installer idempotency/verification checks.

## Installer verification
Dry-run + install + verify:
- MeshCore 1.15.0: PASS
- MeshCore 1.16.0: PASS
- MeshCore 1.17.1: PASS

Second installer run:
- 1.15.0: already installed / no duplicate patch
- 1.16.0: already installed / no duplicate patch
- 1.17.1: already installed / no duplicate patch

PlatformIO build was not executed because `pio` is not available in this environment.
