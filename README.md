# RosScope

Roscoppe is a Qt desktop app for ROS 2 runtime monitoring and control on Ubuntu.
It provides a single interface for process visibility, ROS graph health, diagnostics, snapshots, and operational actions.

Built and maintained by Prabal Khare.

## What It Does

- Monitors Linux processes with ROS-aware filtering
- Maps ROS nodes to domains, PIDs, executables, and workspaces
- Inspects ROS graph state (topics, QoS, TF/Nav2, lifecycle)
- Surfaces runtime health issues (zombies, conflicts, missing links, QoS issues)
- Supports operational controls (terminate/kill/restart actions)
- Exports snapshots and diffs
- Records sessions for later analysis
- Supports remote fleet checks and actions over SSH

## Requirements

- Ubuntu 22.04+
- CMake 3.16+
- Qt6 (Core, Widgets, Network)
- `iproute2`, `usbutils`
- Optional but recommended:
  - ROS 2 (Humble/Iron/Jazzy) with `ros2` in `PATH`
  - `can-utils`
  - `openssh-client` (fleet features)

Install base dependencies:

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

Binary: `build/roscoppe`

## Run

```bash
source /opt/ros/<distro>/setup.bash
# optional overlay:
# source /path/to/ws/install/setup.bash
./build/roscoppe
```

## UI Modes

- Engineer: full diagnostics and control surface
- Operator: simplified runtime/safety view

## Key Output Files

- Snapshots: `snapshots/`
- Session exports: `sessions/`
- Presets: `presets/`
- Telemetry: `logs/`
- Fleet queue/state: `state/`

## Project Structure

```text
include/rrcc/   # public headers
src/services/   # runtime, diagnostics, control, monitoring services
src/ui/         # main Qt UI
docs/           # architecture and performance notes
```

## Troubleshooting

- If the app shows limited ROS data, verify `ros2` is in `PATH` and environment is sourced.
- If fleet actions fail, verify SSH connectivity and credentials.
- For build issues, remove `build/` and reconfigure:

```bash
rm -rf build
cmake -S . -B build
cmake --build build -j$(nproc)
```

## Documentation

- Architecture: `docs/ARCHITECTURE.md`
- Performance targets: `docs/PERFORMANCE_TARGETS.md`
