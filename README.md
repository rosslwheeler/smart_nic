# Smart NIC Model

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
[![CI](https://github.com/rosslwheeler/smart_nic/actions/workflows/ci.yml/badge.svg)](https://github.com/rosslwheeler/smart_nic/actions/workflows/ci.yml)

A cycle-free behavioral model of an advanced Ethernet NIC in C++20 for functional simulation and testing. Features queueing, DMA, hardware offloads (TSO/LRO/checksum), SR-IOV virtualization, RoCEv2/RDMA, PTP time sync, and Tracy-based tracing.

## Purpose

This project provides a software-only NIC model suitable for:

- **Learning** -- explore how real NICs work internally (DMA engines, descriptor rings, RSS, interrupt coalescing, RDMA)
- **Driver development** -- test NIC driver logic without physical hardware
- **Functional simulation** -- validate packet processing pipelines, queue management, and offload behavior

## Features

### Core NIC
- **PCIe device model** -- BAR/window modeling, config space, MSI-X capability
- **DMA engine** -- Host memory abstraction with IOMMU awareness, scatter-gather lists
- **Descriptor rings** -- TX/RX descriptor lifecycle, doorbells, completion queues
- **Packet pipeline** -- TX/RX paths with checksum offload, TSO/LRO

### Queueing and Scheduling
- **Multiple queue pairs** -- Configurable TX/RX queue pairs
- **RSS** -- Receive Side Scaling with Toeplitz hashing
- **Traffic classes** -- Weighted round-robin scheduling

### Interrupts
- **MSI-X** -- Per-queue interrupt vector mapping
- **Interrupt coalescing** -- Packet threshold and timer-based moderation

### Virtualization
- **SR-IOV** -- Physical Function / Virtual Function split
- **Per-VF resources** -- MAC/VLAN filters, queue allocation
- **Mailbox** -- PF-VF communication channel

### RoCEv2 / RDMA
- **Protection domains and memory regions**
- **Queue pairs** -- RC transport with SEND/RECV, RDMA WRITE, RDMA READ
- **Congestion control** -- DCQCN with CNP/PFC
- **Completion queues** -- Work completion tracking

### Advanced Features
- **PTP clock** -- IEEE 1588 nanosecond timestamping, clock synchronization, drift compensation
- **Flow control** -- IEEE 802.3x pause frames, PFC (802.1Qbb), backpressure
- **Statistics** -- Per-queue and per-port counters
- **Error injection** -- Configurable fault injection for testing
- **Tracy tracing** -- Full function-level tracing via Tracy profiler

### Driver Library
A high-level driver layer (`nic_driver` namespace) wraps the NIC model with a familiar API:
- **Ethernet API** -- `send_packet()`, `process()`, `get_stats()`
- **RDMA API** (libibverbs-style) -- `create_pd()`, `register_mr()`, `create_cq()`, `create_qp()`, `post_send()`, `post_recv()`, `poll_cq()`
- **PacketRouter** -- Routes RoCEv2 packets between driver instances for testing

## Architecture

The model is organized into 7 major subsystems under the `nic` namespace:

| Subsystem | Namespace | Key Headers |
|-----------|-----------|-------------|
| Bus & Config | `nic::pcie` | `device.h`, `config_space.h`, `bar.h`, `msix.h` |
| Memory & DMA | `nic` | `host_memory.h`, `dma_engine.h`, `descriptor_ring.h` |
| Packet Pipeline | `nic` | `tx_rx.h`, `queue_pair.h`, `checksum.h`, `offload.h` |
| Queueing & Scheduling | `nic` | `queue_manager.h`, `rss.h` |
| Interrupts | `nic` | `interrupt_dispatcher.h`, `msix.h` |
| RoCEv2 / RDMA | `nic::rocev2` | `engine.h`, `queue_pair.h`, `packet.h` |
| Advanced Features | `nic` | `ptp_clock.h`, `flow_control.h`, `stats_collector.h` |

```
smart_nic/
├── include/nic/          # Public headers (52 headers)
│   └── rocev2/           # RoCEv2/RDMA headers (14 headers)
├── src/                  # Implementation (37 source files)
│   └── rocev2/           # RoCEv2 implementation (10 files)
├── tests/                # Unit and integration tests
│   ├── rocev2/           # RoCEv2 tests
│   └── driver/           # Driver integration tests
├── driver/               # High-level driver library
│   ├── include/nic_driver/
│   ├── src/
│   └── examples/         # Echo server example
├── docs/                 # Documentation
├── libs/bit_fields/      # Bit field utilities (submodule)
└── third-party/tracy/    # Tracy profiler (submodule)
```

## Requirements

- C++20 compiler (GCC 10+, Clang 11+)
- CMake 3.21+
- bit_fields library submodule (included)
- Tracy profiler submodule (included)

## Getting Started

```bash
# Clone with submodules
git clone --recursive https://github.com/rosslwheeler/smart_nic.git
cd smart_nic

# Build
make all              # configure + build (Debug)

# Run tests
make test-notrace     # without Tracy capture
make test             # with Tracy capture (builds tracy-capture first)
```

## Build Options

```bash
make configure              # Run CMake (BUILD_TYPE defaults to Debug)
make build                  # Compile library and tests
make test-notrace           # Run tests without Tracy capture
make test                   # Run tests with Tracy capture
make clean                  # Remove build directory

# Quality
make coverage               # Build with coverage, run tests, generate HTML report
make asan                   # Build with AddressSanitizer, run tests

# Tracy tools
make tracy-profiler         # Build Tracy GUI profiler
make tracy-capture          # Build Tracy CLI capture tool
```

CMake options:
| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TYPE` | `Debug` | Build type (Debug/Release) |
| `NIC_ENABLE_TRACY` | `ON` | Enable Tracy tracing |
| `NIC_WARNINGS_AS_ERRORS` | `ON` | Treat warnings as errors |
| `NIC_ENABLE_COVERAGE` | `OFF` | Enable code coverage |
| `NIC_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer |
| `NIC_BUILD_DRIVER` | `ON` | Build driver library |
| `NIC_BUILD_EXAMPLES` | `ON` | Build example applications |

## Documentation

| Document | Description |
|----------|-------------|
| [User's Guide](docs/users_guide.md) | Architecture, networking concepts, DMA, PCIe, interrupts, RoCEv2 |
| [Tutorial](docs/tutorial.md) | 8 hands-on lessons from first packet to custom queue schedulers |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, code style, and PR guidelines.

## License

MIT License -- see [LICENSE](LICENSE) for details.