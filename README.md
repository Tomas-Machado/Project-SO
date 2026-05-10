# Project-SO
# Multi-Runner Task Orchestration System

This project implements an asynchronous task management system based on a client-server architecture for a Unix environment. It was developed as part of the Operating Systems course at the University of Minho.

## 🚀 Overview

The system consists of two main components:
- **Controller (Server):** Manages task scheduling, concurrency control, and logging.
- **Runner (Client):** Submits commands to the server, manages execution pipelines, and handles I/O redirections.

The architecture is designed to be non-blocking, utilizing a multi-level process hierarchy to ensure the server remains responsive even under heavy load.

## 🛠️ Key Features

- **Asynchronous Execution:** Commands are dispatched to background monitors, allowing the server to accept new requests instantly.
- **Concurrency Control:** Limits the number of simultaneous active tasks to prevent CPU exhaustion.
- **Fair Scheduling:** Supports both **FCFS** (First-Come, First-Served) and **Round-Robin** (User-based) policies to prevent starvation.
- **Complex Pipelines:** Full support for shell-like pipelines (`|`) and I/O redirections (`>`, `<`, `2>`).
- **Resilience:** Implements atomic data persistence in `registo.csv` and robust error handling for failed commands or invalid inputs.
- **IPC Mechanism:** Uses Named Pipes (FIFOs) for control signaling and Anonymous Pipes for data flow between commands.

## 🏗️ Architecture

The system operates through five critical states (Handshake protocol) to accurately distinguish between "Time in Queue" and "Actual Execution Time".

1. **Type 1 (New):** Task submission.
2. **Type 2 (Status):** Querying executing and scheduled tasks.
3. **Type 3 (Shutdown):** Controlled server exit after flushing the queue.
4. **Type 4 (Finished):** Resource release notification.
5. **Type 5 (Handshake):** Start of binary execution.

## 💻 Getting Started

### Prerequisites
- GCC Compiler
- GLib 2.0 library (`libglib2.0-dev`)
- Linux/Unix environment

### Compilation
To compile the entire project, run:
```bash
make
```
This will create the necessary folders (bin, obj, tmp) and generate the controller and runner executables.

Running the System
Start the Controller:

Bash
./bin/controller <parallel-commands> <sched-policy>
parallel-commands: Max simultaneous tasks.

sched-policy: 0 for FCFS, 1 for Round-Robin.

Execute a Command:

Bash
./bin/runner -e <user-id> "command | with | pipeline > out.txt"
Check Status:

Bash
./bin/runner -c
Shutdown Server:

Bash
./bin/runner -s
📊 Testing
A complete stress test script is included. To run the automated battery of tests:

Bash
chmod +x teste_stress.sh
./teste_stress.sh
