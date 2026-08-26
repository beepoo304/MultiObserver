# MultiObserver Installer — compatibility matrix

Installer target: `examples/simple_repeater`.

## Verified source trees

| MeshCore | Result |
|---|---|
| 1.15.0 | compatible |
| 1.16.0 | compatible |
| 1.17.1 | compatible |

The three supplied source archives were inspected before implementing the
installer.

The same structural integration points are present in all three:

- `MyMesh::logRx(mesh::Packet*, int, float)`
- `MyMesh::logTx(mesh::Packet*, int)`
- `_cli.handleCommand(sender_timestamp, command, reply)`
- `the_mesh.begin(fs)`
- `the_mesh.loop()`
- `the_mesh.self_id.pub_key`
- `the_mesh.getNodePrefs()->node_name`
- `mesh::Utils::toHex(...)`

The installer therefore uses structural fingerprints instead of hard-coded
line numbers or version-specific source offsets.

## MeshCore files modified by the installer

Only these application files are patched:

- `examples/simple_repeater/main.cpp`
- `examples/simple_repeater/MyMesh.h`
- `examples/simple_repeater/MyMesh.cpp`
- `examples/simple_repeater/UITask.cpp`

The build configuration is updated only in:

- `variants/heltec_v3/platformio.ini`

The MeshCore `src/` tree is not modified.

MultiObserver itself is copied into:

- `lib/MultiObserver/`
- `examples/simple_repeater/MOBridge.h`
- `examples/simple_repeater/MOBridge.cpp`
- `scripts/multiobserver_cpp17.py`

## Safety

- `--dry-run` performs validation without writing.
- Installation creates a timestamped backup of every patched MeshCore file and
  the Heltec build configuration.
- Installation failure triggers automatic rollback.
- Existing complete installation is idempotent.
- Partial marker state is rejected rather than guessed.
- Ambiguous or missing integration anchors abort installation.
- Prepared identities are written only to the selected target tree and are
  never retained in the MO repository.
