# Architecture

## Runtime Model

Roscoppe is a desktop Qt application with a dedicated background runtime thread.

- `MainWindow` handles rendering and user interaction.
- `RuntimeWorker` performs all heavy polling and control operations off the UI thread.
- UI communicates with worker through queued Qt signals and receives immutable snapshot payloads.

This keeps UI responsive under high node counts and avoids blocking during ROS CLI calls.

## Layered Modules

1. System Core
- `ProcessManager`: `/proc` process discovery, ROS-aware process metadata, kill controls
- `SystemMonitor`: CPU/memory/disk/GPU/network/USB/serial/CAN and `dmesg`

2. ROS Integration
- `RosInspector`: dynamic domain/node/topic/service/action introspection, TF/runtime inspection,
  workflow-agnostic role classification, and graph dependency analysis (no package hardcoding)

3. Diagnostics and Intelligence
- `HealthMonitor`: core health flags (duplicate/zombie/conflicts/TF)
- `DiagnosticsEngine`: parameter drift, topic rates, QoS mismatch, lifecycle timeline,
  executor load, correlation timeline, memory leak, DDS/network heuristics,
  workspace conflict scanning, runtime fingerprinting, launch validation, dependency impact

4. Operations and Persistence
- `ControlActions`: local runtime process actions
- `SnapshotManager`: snapshot export (JSON/YAML)
- `SnapshotDiff`: snapshot comparison engine
- `SessionRecorder`: timeline recording and export
- `RemoteMonitor`: SSH-based fleet status and remote control actions

## Data Contract

`RuntimeWorker` publishes a unified snapshot object containing:

- process state
- domain and graph state
- TF/Nav2 state
- system and logs
- health status
- advanced diagnostics
- fleet status
- session status
- watchdog status

All UI tabs render from this snapshot only, with no direct ROS/system calls from the UI thread.

## Extension Points

The architecture exposes stable extension points for:

- AI diagnostics modules (attach to `DiagnosticsEngine` output)
- distributed aggregation (extend `RemoteMonitor`)
- deterministic launch policy enforcement (extend launch validation profile)
- auto-recovery policies (extend `RuntimeWorker::applyWatchdog`)
