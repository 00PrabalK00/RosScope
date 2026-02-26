# Roscoppe

Roscoppe is a native Ubuntu desktop application for ROS 2 runtime introspection, diagnostics, safety monitoring, and control.
Built and maintained by Prabal Khare.

The project is designed for heavy robotics workloads and multi-workspace development where standard `htop` and ad-hoc `ros2` CLI loops are too slow and fragmented.

## Core Capabilities

- Linux process monitoring with ROS-aware filtering
- Node to process mapping and ROS domain grouping
- ROS graph introspection (topics, services, actions, QoS)
- Dynamic workflow-agnostic node role classification from runtime behavior
- Runtime anomaly detection
- TF and Nav2 monitoring
- Safety watchdog and soft safety boundary checks
- Snapshot export and snapshot diff
- Session recorder for runtime replay analysis
- Runtime presets for expected topology and thresholds
- Remote fleet monitoring and remote restart/kill actions

Detection is dynamic and does not rely on hardcoded package names.
ROS process classification is based on runtime signals such as:
- ROS environment variables (`ROS_DOMAIN_ID`, `ROS_VERSION`, `AMENT_PREFIX_PATH`, `COLCON_PREFIX_PATH`)
- ROS command-line patterns (`--ros-args`, `__node:=`, `__ns:=`)
- loaded ROS 2/DDS runtime libraries from `/proc/<pid>/maps`

## Architecture

The implementation is modular and split into layers:

1. System Monitoring Core
- `ProcessManager`
- `SystemMonitor`

2. ROS Integration Layer
- `RosInspector`

3. Diagnostics and Intelligence
- `HealthMonitor`
- `DiagnosticsEngine`

4. Control and Operations
- `ControlActions`
- `SnapshotManager`
- `SnapshotDiff`
- `SessionRecorder`
- `RemoteMonitor`

5. UI and Orchestration
- `MainWindow`
- `RuntimeWorker` (dedicated background thread, non-blocking UI)

Detailed module design:
- `docs/ARCHITECTURE.md`

## Project Layout

```text
include/rrcc/
  command_runner.hpp
  control_actions.hpp
  diagnostics_engine.hpp
  health_monitor.hpp
  main_window.hpp
  process_manager.hpp
  remote_monitor.hpp
  ros_inspector.hpp
  runtime_worker.hpp
  session_recorder.hpp
  snapshot_diff.hpp
  snapshot_manager.hpp
  system_monitor.hpp

src/
  main.cpp
  services/
    command_runner.cpp
    control_actions.cpp
    diagnostics_engine.cpp
    health_monitor.cpp
    process_manager.cpp
    remote_monitor.cpp
    ros_inspector.cpp
    runtime_worker.cpp
    session_recorder.cpp
    snapshot_diff.cpp
    snapshot_manager.cpp
    system_monitor.cpp
  ui/
    main_window.cpp
```

## Requirements

Target OS:
- Ubuntu 22.04 or newer

Required packages:
- `build-essential`
- `cmake` (3.16+)
- `qt6-base-dev`
- `qt6-base-dev-tools`
- `iproute2`
- `usbutils`

Recommended tools:
- ROS 2 (Humble/Iron/Jazzy), `ros2` in `PATH`
- `can-utils`
- NVIDIA drivers and `nvidia-smi` if GPU telemetry is needed
- `openssh-client` for remote fleet features

Install dependencies:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  qt6-base-dev \
  qt6-base-dev-tools \
  iproute2 \
  usbutils \
  can-utils \
  openssh-client
```

## Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Binary:
- `build/roscoppe`

## Run

Source ROS and optional overlays before launch:

```bash
source /opt/ros/<distro>/setup.bash
source /path/to/overlay_ws/install/setup.bash
./build/roscoppe
```

## UI Overview

Tabs:
- Processes
- ROS Domains
- Nodes and Topics
- TF and Nav2
- System and Hardware
- Logs
- Diagnostics
- Performance
- Safety
- Workspaces
- Fleet

Modes:
- Engineer mode: full controls, diagnostics, and deep introspection tabs
- Operator mode: constrained operational view with emergency controls and health status

## Advanced Runtime Features

Implemented intelligence services include:

- Parameter drift tracker:
  - Detects parameter hash changes over time
- Universal node profiling:
  - node name, namespace, domain, PID, executable, package, workspace
  - publishers/subscribers/services/actions
  - lifecycle capability/state
  - parameter inventory and plugin-related parameter hints
- Dynamic behavior classification:
  - controller/planner/perception/state-estimation/task-executor roles inferred from runtime message patterns
- Topic rate analyzer:
  - Tracks expected vs observed rates
  - Flags drops and publisher underperformance
- QoS mismatch detector:
  - Flags mixed/incompatible QoS profile sets per topic
- Lifecycle timeline:
  - Tracks state transitions and stuck transitional states
- Executor load monitor:
  - Flags high CPU/thread ROS processes and potential callback pressure
- Cross-correlation timeline:
  - Correlates CPU spikes with TF and topic health degradations
- Memory leak detection:
  - Detects persistent upward memory trends per node
- DDS participant inspector:
  - Tracks participant churn and ghost/zombie signals
- Network saturation monitor:
  - Tracks interface throughput and high-traffic topics
- Soft safety boundary:
  - Warns on TF degradation, low IMU/costmap rate conditions
- Workspace tools:
  - Overlay chain view
  - Duplicate package detection
  - Mixed distro/ABI mismatch suspicion
- Runtime fingerprinting and launch validation:
  - Deterministic signature generation
  - Rogue/missing node checks against expected profile
- Dependency impact map:
  - Downstream impact estimate for node failures
- Dynamic graph integrity checks:
  - subscribers without publishers
  - service clients without servers
  - action clients without servers
  - isolated nodes and circular dependencies
  - PID present but node missing in graph (misinitialized process)

## Operational Features

Header controls include:
- Snapshot JSON/YAML export
- Snapshot diff (file vs file)
- Save/load runtime preset
- Start/stop/export session recorder
- Watchdog enable/disable
- Fleet target loading and refresh
- Remote restart and remote kill action triggers
- Emergency stop for ROS runtime

## Runtime Presets

Preset files are stored in:
- `presets/<name>.json`
- Example template: `resources/example_preset.json`

A preset stores:
- expected profile (expected nodes/topics/rates)
- watchdog state
- remote target list
- selected domain context

## Session Recording

Session data is stored in:
- `sessions/<session_name>_<timestamp>.json`

Recorded samples include full runtime snapshots with:
- process/domain/graph state
- diagnostics
- safety signals
- system and fleet state

## Fleet Configuration

Remote fleet targets are loaded from a JSON array file, for example `fleet_targets.json`:

```json
[
  {
    "name": "robot_1",
    "host": "192.168.1.10",
    "user": "ubuntu",
    "port": 22,
    "domain_id": "0",
    "ros_setup": "/opt/ros/humble/setup.bash"
  }
]
```

Example template:
- `resources/example_fleet_targets.json`

The app can:
- collect remote robot health summaries
- restart a remote domain daemon
- trigger remote ROS kill pattern action

## Snapshot and Diff

Snapshot files are written to:
- `snapshots/roscoppe_snapshot_<timestamp>.json`
- `snapshots/roscoppe_snapshot_<timestamp>.yaml`

Diff supports:
- node additions/removals
- topic additions/removals
- domain changes
- parameter hash changes

## Notes and Limitations

- Node to PID mapping is best-effort due ROS 2 runtime model differences across launch styles.
- Some deep metrics are inference-based when direct DDS or executor internals are unavailable.
- Remote actions require SSH access and command availability on the target host.
- Some operations may require elevated privileges depending on local system security policy.

## Troubleshooting

- `ros2: command not found`
  - Source ROS setup script before starting Roscoppe.
- Empty ROS graph
  - Verify `ROS_DOMAIN_ID`, DDS networking, and overlay sourcing.
- Remote fleet unreachable
  - Validate SSH key access, hostname, and network route.
- Qt build errors
  - Verify `qt6-base-dev` and that CMake can resolve Qt6.
