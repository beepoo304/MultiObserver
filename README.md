# MultiObserver

MultiObserver is an open-source Observer extension for MeshCore 1.17.1.

The project is designed as a small application-level extension rather than a
replacement for MeshCore core functionality.

## Current status

The repository currently contains the project skeleton and integration layout.
Implementation is intentionally not started yet.

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
│   └── verify.py
├── lib/
│   └── MultiObserver/
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
└── installer/
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
