# TerminalChat

TerminalChat is a C++20 terminal-based real-time chat application built around a modern text user interface, a single-process non-blocking TCP server, and SQLite-backed persistence. The project is split into a client, a server, and a shared protocol layer, with FTXUI handling the terminal experience and nlohmann/json carrying the wire format between processes.

## Overview

The application is designed for fast local chat sessions with a clean terminal workflow:

- The **client** presents a full-screen TUI built with FTXUI.
- The **server** uses POSIX sockets, `poll()`, and non-blocking I/O to handle multiple connections in a single process.
- **SQLite3** stores user accounts, room metadata, and message history.
- **libsodium** is used for secure password hashing.
- **nlohmann/json** serializes packets between client and server.

The repository follows a straightforward layout:

- `src/client/` contains the FTXUI terminal client.
- `src/server/` contains authentication, room routing, and persistence logic.
- `src/shared/` contains the packet model and JSON serialization helpers.
- `CMakeLists.txt` defines the build targets and pulls third-party dependencies with FetchContent.

## Key Features & Architecture

### Modern Terminal UI

The client uses FTXUI to provide a full-screen terminal interface with multiple views, including authentication, room selection, and the chat screen. Password entry is masked, and keyboard-driven navigation keeps the experience fast and compact.

### Event-Driven Networking

The server runs as a single process and relies on non-blocking sockets plus `poll()` to multiplex connections without a thread-per-client model. This keeps the runtime simple and predictable while still handling multiple active users.

### Data Persistence

SQLite3 stores the durable application state:

- registered users
- room metadata
- message history

Room data is hydrated from the database when the server starts so existing rooms remain available after a restart.

### Message History

The server stores chat messages and can return recent room history in chronological order when a client enters a room. This gives each room immediate context instead of starting with an empty timeline.

### Security

Passwords are hashed with libsodium before being stored, so the database never keeps plaintext credentials.

## Tech Stack

- **Language:** C++20
- **UI Library:** FTXUI
- **Networking:** POSIX sockets, `poll()`
- **Database:** SQLite3
- **Cryptography:** libsodium
- **Serialization:** nlohmann/json
- **Build System:** CMake 3.24+

## Project Structure

```text
TerminalChat/
├── CMakeLists.txt
├── src/
│   ├── client/
│   │   └── main.cpp
│   ├── server/
│   │   ├── auth.cpp
│   │   ├── auth.hpp
│   │   └── main.cpp
│   └── shared/
│       └── protocol.hpp
└── build/                # Generated build directory
```

## Getting Started

### Prerequisites

Make sure the following tools and libraries are available on your system:

- CMake 3.24 or newer
- a C++20 compiler
- SQLite3 development headers and library
- libsodium development headers and library
- pkg-config

On macOS, you can typically install the native dependencies with Homebrew:

```bash
brew install cmake sqlite3 libsodium pkg-config
```

### Clone the Repository

```bash
git clone <your-repository-url>
cd TerminalChat
```

### Configure and Build

From the project root:

```bash
cmake -S . -B build
cmake --build build --target chat_server chat_client
```

The CMake configuration fetches FTXUI and nlohmann/json automatically.

### Run the Application

Start the server in one terminal:

```bash
./build/chat_server
```

Start the client in a second terminal:

```bash
./build/chat_client
```

The client connects to `127.0.0.1:8080` by default.

## How It Works

### Client Flow

1. The client connects to the local server over TCP.
2. The user registers or logs in from the authentication screen.
3. The room selection view lets the user create or join a room.
4. The chat view renders the current room conversation and accepts new messages.
5. Incoming packets are parsed on a background reader thread and posted back into the FTXUI event loop for safe UI updates.

### Server Flow

1. The server opens a listening socket on port `8080`.
2. `poll()` monitors the listener and connected clients.
3. Incoming JSON packets are parsed and dispatched by packet type.
4. Authentication is validated against SQLite.
5. Messages are stored and broadcast to the room participants.

## Future Scope & Roadmap

- Migrate from `poll()` to `epoll()` for better horizontal scaling.
- Add a background worker thread pool to offload blocking SQLite I/O.
- Implement end-to-end encryption for direct messages using libsodium primitives.
- Add Docker and Docker Compose support for one-command deployment.

## Notes

- The project uses a shared JSON packet format between client and server.
- The database file is stored under `data/chat.db` at runtime.
- The build output is generated in `build/`.
