# QEMU Integration Analysis

## 1. Module Overview

The project's integration with QEMU is achieved through a sophisticated, decoupled architecture centered around the `vhost-user-blk` protocol. Instead of creating a proprietary QEMU device model, the simulator runs as an independent backend process. QEMU connects to this process using a standard `vhost-user-blk-pci` device, which forwards block I/O requests from the guest OS to the simulator over a Unix socket.

This design provides excellent separation of concerns: QEMU does not need to know any internal details of the SSD simulator, and the simulator can be developed, tested, and run independently of QEMU. The integration point is a well-defined, high-performance virtio-based protocol, standard in the Linux virtualization ecosystem.

## 2. Key Files & Directories

-   **`scripts/run_qemu_nvme.sh`**: This is the primary entry point for launching the simulator with QEMU. It demonstrates the end-to-end workflow: starting the backend server, waiting for it to initialize, and then launching QEMU with the correct parameters to connect to it.
-   **`src/vhost/hfsss_vhost_main.c`**: The source code for the `hfsss-vhost-blk` executable. This program acts as the bridge between the simulator core and QEMU.
-   **`src/pcie/nvme_uspace.c` / `.h`**: This appears to be the top-level module for the entire userspace SSD simulator instance, encapsulating the FTL, HAL, and media layers. The `vhost` server is initialized with an instance of `nvme_uspace_dev`.

## 3. Core Data Structures & Functions

-   **`hfsss-vhost-blk` (executable)**: The main backend process. It parses a socket path, initializes the `nvme_uspace_dev`, and starts the vhost server.
-   **`nvme_uspace_dev` (struct)**: Represents the entire SSD simulator instance. It is passed to the vhost server during initialization, likely containing function pointers for block I/O operations.
-   **`vhost_user_blk_server` (struct, external)**: A data structure representing the vhost server instance. Its definition is not in this project, indicating it comes from an external library.
-   **`vhost_blk_server_init()` / `run()` / `stop()` (functions, external)**: These are the API functions provided by the external vhost library to control the server lifecycle.

## 4. Control Flow

1.  The `run_qemu_nvme.sh` script is executed.
2.  It first launches the `hfsss-vhost-blk` process in the background. This process creates a Unix socket at a known path (e.g., `/tmp/hfsss-vhost.sock`).
3.  Inside `hfsss_vhost_main`, the `nvme_uspace_dev_init()` and `nvme_uspace_dev_start()` functions are called, initializing and starting all the core SSD simulation threads.
4.  `vhost_blk_server_init()` is then called, passing a pointer to the initialized `nvme_uspace_dev` instance. This step likely registers the simulator's I/O functions (e.g., `hfsss_read`, `hfsss_write`) as callbacks for the vhost server.
5.  `vhost_blk_server_run()` is called, and the server begins listening for connections on the Unix socket.
6.  The `run_qemu_nvme.sh` script waits until the socket file is created, then launches `qemu-system-aarch64`.
7.  QEMU is configured with a `-chardev` pointing to the socket and a `-device vhost-user-blk-pci`. This device connects to the `hfsss-vhost-blk` server.
8.  When the guest OS performs block I/O on the virtual device, QEMU translates these into `vhost-user` protocol messages, sends them to the `hfsss-vhost-blk` server, which in turn invokes the registered HFSS callbacks to perform the simulated I/O.

## 5. Dependencies

-   **QEMU**: The primary virtualization platform. Specifically `qemu-system-aarch64` is used.
-   **External `vhost-user` Library**: The project relies on an external, pre-existing library for the `vhost-user-blk` protocol implementation. This is likely provided by the system's QEMU or DPDK development packages. The project does not re-implement this protocol.
-   **Unix Sockets**: The communication channel between QEMU and the simulator backend is a Unix domain socket.

## 6. Architecture & Design

-   **Client-Server Architecture**: QEMU acts as the client (or "frontend"), and the `hfsss-vhost-blk` process acts as the server (or "backend").
-   **Decoupling**: The architecture provides strong decoupling. The simulator can be updated without changing QEMU, and vice-versa. The `vhost-user-blk` protocol is the stable contract between them.
-   **High Performance**: The `vhost-user` protocol is designed for high performance. It allows for zero-copy data transfers and bypasses kernel context switches for I/O, making it much faster than emulated devices that operate entirely within the QEMU process.

## 7. Potential Improvements & Notes

-   **External Dependency Management**: The dependency on an external `vhost` library is not explicitly documented or managed by the build system (e.g., via a git submodule or a package config file). This can make building the project difficult, as developers must know which system packages to install manually (e.g., `libvhost-user-dev`). Adding a check in the `Makefile` or a note in the README would be beneficial.
-   **Hardcoded Socket Path**: The default socket path is hardcoded. While it can be overridden with a command-line argument, making it more configurable via an environment variable could add flexibility.
-   **Single Queue**: The `run_qemu_nvme.sh` script currently configures the device with only one I/O queue (`num-queues=1`). To properly test multi-queue performance, this should be parameterized or increased.
