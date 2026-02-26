<p align="center">
  <img src="resources/logo" alt="RosScope Logo" width="320" />
</p>

<p align="center">
  <a href="https://github.com/00PrabalK00/RosScope"><img alt="GitHub Repo" src="https://img.shields.io/badge/GitHub-00PrabalK00%2FRosScope-181717?style=for-the-badge&logo=github"></a>
  <a href="https://github.com/00PrabalK00/RosScope/issues"><img alt="GitHub Issues" src="https://img.shields.io/github/issues/00PrabalK00/RosScope?style=for-the-badge&logo=github"></a>
  <a href="https://github.com/00PrabalK00/RosScope/stargazers"><img alt="GitHub Stars" src="https://img.shields.io/github/stars/00PrabalK00/RosScope?style=for-the-badge&logo=github"></a>
  <a href="https://github.com/00PrabalK00/RosScope/commits/main"><img alt="Last Commit" src="https://img.shields.io/github/last-commit/00PrabalK00/RosScope?style=for-the-badge&logo=github"></a>
</p>

<p align="center">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white">
  <img alt="Qt6" src="https://img.shields.io/badge/Qt-6-41CD52?style=for-the-badge&logo=qt&logoColor=white">
  <img alt="ROS 2" src="https://img.shields.io/badge/ROS-2-22314E?style=for-the-badge&logo=ros&logoColor=white">
  <img alt="Ubuntu 22.04+" src="https://img.shields.io/badge/Ubuntu-22.04%2B-E95420?style=for-the-badge&logo=ubuntu&logoColor=white">
  <img alt="CMake 3.16+" src="https://img.shields.io/badge/CMake-3.16%2B-064F8C?style=for-the-badge&logo=cmake&logoColor=white">
</p>

# RosScope

RosScope is a Qt desktop app for ROS 2 runtime monitoring and control on Ubuntu.
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

Binary: `build/RosScope`

## Run

```bash
source /opt/ros/<distro>/setup.bash
# optional overlay:
# source /path/to/ws/install/setup.bash
./build/RosScope
```

## Fleet Targets JSON (resources)

Use [resources/example_fleet_targets.json](resources/example_fleet_targets.json) as the template for remote fleet monitoring/actions.

Expected format: JSON array of target objects.

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

Field reference:

- `name` (required): display name used in Fleet table and actions
- `host` (required): IP/hostname of the target robot
- `user` (optional): SSH username (`user@host`); leave empty to use current user
- `port` (optional): SSH port, default `22`
- `domain_id` (optional): ROS domain for that target, default `"0"`
- `ros_setup` (optional): remote ROS setup script path, default `/opt/ros/humble/setup.bash`

How to use it:

1. Copy and edit the template:
   `cp resources/example_fleet_targets.json fleet_targets.json`
2. Update hosts/users/domain IDs for your robots.
3. In the app, click `Fleet -> Load Fleet Targets` and select your JSON file.

Auto-load option:

- If `fleet_targets.json` exists in the project root, RosScope loads it automatically on startup.

SSH notes:

- Use key-based SSH auth (recommended) and ensure host keys are trusted.
- Quick check from your machine:
  `ssh -p 22 <user>@<host> 'echo ok'`

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
