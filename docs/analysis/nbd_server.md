# NBD Server Analysis

## 1. Module Overview

The project includes a custom, full-featured NBD (Network Block Device) server, implemented entirely in userspace. This server provides an alternative mechanism to the `vhost-user-blk` backend for exposing the HFSS simulator as a block device to clients like QEMU. It listens on a TCP port, speaks the NBD protocol, and translates NBD I/O commands into direct calls to the simulator's core FTL engine.

The implementation is self-contained and handles the NBD handshake, option negotiation, and data transmission phases. It demonstrates a deep integration with the simulator's core, including support for both single-threaded and multi-threaded I/O submission paths.

## 2. Key Files & Directories

-   **`src/vhost/hfsss_nbd_server.c`**: The main source file containing the entire NBD server implementation, including the `main` function, protocol handshake logic, and I/O command processing loop.
-   **`src/vhost/nbd_async.c` / `include/vhost/nbd_async.h`**: These files provide an asynchronous I/O submission layer, allowing the NBD server to dispatch I/O requests to the FTL's worker threads and poll for completion, enabling higher throughput.
-   **`src/pcie/nvme_uspace.h`**: The header file that defines the interface to the core simulator engine (`nvme_uspace_dev`), which the NBD server calls to execute block operations.

## 3. Core Data Structures & Functions

-   **`nbd_request` (struct)**: A packed struct that mirrors the on-wire format of an NBD request from a client. It includes fields for magic number, command type, offset, and length.
-   **`nbd_reply` (struct)**: A packed struct for on-wire replies, containing a magic number, error code, and the original request handle.
-   **`main()`**: The server entry point. It parses command-line arguments (port), initializes the `nvme_uspace_dev` simulator core, creates a listening TCP socket, and enters an `accept()` loop.
-   **`nbd_handshake()`**: Implements the "new-style fixed" NBD handshake protocol. It negotiates options with the client and prepares for the transmission phase.
-   **`nbd_serve()`**: The core I/O processing loop. It reads `nbd_request` structs from the client, dispatches them based on the command type (`NBD_CMD_READ`, `NBD_CMD_WRITE`, etc.), and calls the appropriate simulator functions.
-   **`nvme_uspace_read/write/flush/trim()`**: These are the key integration functions. `nbd_serve` calls these functions on the `nvme_uspace_dev` instance to perform the actual I/O against the simulated FTL.

## 4. Control Flow

1.  The `hfsss-nbd-server` executable is launched, listening on a specified TCP port (e.g., 10809).
2.  The `main` function initializes the entire HFSS core via `nvme_uspace_dev_init()`.
3.  The server waits for a TCP connection from a client (e.g., QEMU).
4.  Once a client connects, `nbd_handshake()` is called to perform the NBD negotiation.
5.  After a successful handshake, control passes to `nbd_serve()`.
6.  `nbd_serve()` enters a loop, reading requests from the client socket.
7.  A `switch` statement handles each command type:
    *   For `NBD_CMD_READ`, it calls `nvme_uspace_read()` and writes the resulting data back to the socket.
    *   For `NBD_CMD_WRITE`, it reads the write payload from the socket, performs a read-modify-write operation if the request is not page-aligned, and calls `nvme_uspace_write()`.
    *   `NBD_CMD_FLUSH` and `NBD_CMD_TRIM` are similarly mapped to their `nvme_uspace_*` counterparts.
8.  The loop continues until the client sends `NBD_CMD_DISC` or a connection error occurs.

## 5. Dependencies

-   **POSIX Sockets API**: The server is built on the standard Berkeley sockets API (`socket`, `bind`, `listen`, `accept`, `read`, `write`).
-   **HFSS Core Engine**: The NBD server is tightly coupled to the `nvme_uspace_dev` interface for all its block device operations.
-   **Pthreads**: Required for multi-threaded I/O mode.

## 6. Architecture & Design

-   **Monolithic Server Design**: The server logic is largely contained within a single source file. It follows a classic single-process, iterative server model (one client at a time), with an `accept` loop.
-   **Protocol Encapsulation**: The implementation correctly encapsulates NBD protocol details, including byte order conversions (`htonll`) and wire-format structures.
-   **Abstraction Layering**: There is a clean separation between the NBD protocol handling layer (`nbd_serve`) and the backend storage engine (`nvme_uspace_dev`). The server acts as an adapter, translating NBD requests into the internal API of the simulator.
-   **Read-Modify-Write for Unaligned I/O**: The server correctly handles potentially unaligned NBD requests by implementing a read-modify-write strategy, ensuring that I/O submitted to the page-oriented FTL is always correctly aligned.
-   **Optional Asynchronous I/O**: The ability to switch between direct synchronous calls (`nvme_uspace_write`) and asynchronous submission (`ftl_mt_submit`) is a sophisticated feature that allows the server to leverage the FTL's full performance capabilities.

## 7. Potential Improvements & Notes

-   **Single Client Limitation**: The current design appears to handle only one client connection at a time. To support multiple simultaneous QEMU instances, the server would need to be re-architected to use multiple threads or processes (e.g., by forking after `accept`).
-   **Error Handling**: While basic error codes (`NBD_EIO`) are returned, the error propagation from the FTL back to the NBD client could be more granular. The FTL might provide more specific error information that is currently being abstracted away.
-   **Hardcoded Export**: The server exposes a single, hardcoded block device. A more advanced implementation could allow for configuring and exporting multiple different block devices (namespaces) with different properties.
