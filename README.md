# Socket Chat

A lightweight, multi-client chat system built in **C++** using POSIX sockets, `epoll` for scalable I/O multiplexing, and UDP for peer discovery.  
Clients can automatically discover the server on the local network or connect directly via host/port. Messages are broadcast to all connected peers in real time.

---

## ✨ Features
- Multi-client chat over TCP sockets  
- Scalable I/O using `epoll` (non-blocking event-driven model)  
- Automatic discovery of the server via UDP broadcast  
- Runs on Linux/macOS and inside WSL on Windows  
- Length-prefixed framed messaging protocol to preserve message boundaries  
- Clean modular design with shared utilities in `src/common/`

---

## 📂 Project structure

    src/
    ├── common/        # Shared protocol and helper utilities
    │   ├── common.hpp
    │   └── common.cpp
    ├── server/        # Server-side implementation
    │   └── server.cpp
    └── client/        # Client-side implementation
        └── client.cpp
    build/             # Build artifacts (executables + object files)
    Makefile           # Build automation
    README.md          # This file

---

## ⚡ Getting started

### Prerequisites
- Linux or macOS (or WSL on Windows)  
- g++ with C++17 support (e.g., g++ >= 7)  
- make

### Build
    make

After building, the executables will be:
- build/src/server/server
- build/src/client/client

---

## 🚀 Running

### Start the server
    ./build/src/server/server

Options:
- -p, --port TCP_PORT        (default: 5050)
- -d, --discover-port UDP_PORT (default: 55555)

### Start the client
    ./build/src/client/client

Options:
- --host IP                  (explicit server IP)
- --port TCP_PORT            (explicit server port)
- -d, --discover-port UDP_PORT (UDP discovery port)

If `--host` is omitted, the client attempts UDP broadcast discovery.

### Local test (example)
Open three terminals:

1. Terminal A — server:
       cd <project-root>
       ./build/src/server/server

2. Terminal B — client 1:
       cd <project-root>
       ./build/src/client/client

3. Terminal C — client 2:
       cd <project-root>
       ./build/src/client/client

Type messages in client terminals and press Enter; messages will be broadcast to the other clients.

---

## 🔍 Technical overview

### Protocols
- **UDP discovery**
  - Client broadcasts the token `CHAT_DISCOVER?` to 255.255.255.255 on the discovery port.
  - Server listens on the discovery UDP port and responds to the sender with: `CHAT_HERE <tcp_port>`.
  - The client uses the response to learn the server IP and TCP port.

- **TCP framing**
  - Each message uses a 4-byte big-endian length prefix followed by the payload bytes.
  - This framing ensures that message boundaries are preserved in the TCP byte stream.

### Core concepts demonstrated
- Non-blocking sockets and `epoll` for scalable single-threaded I/O
- Edge-triggered event handling (EPOLLET) and the need to drain sockets until `EAGAIN`
- Per-client input/output buffers to handle partial reads/writes
- Safe buffering for partial writes and re-flushing when socket becomes writable
- UDP broadcast discovery as a convenient LAN service discovery mechanism

---

## 🛠 Troubleshooting & tips

- If `make` produces only `.o` object files but no executables, ensure the Makefile contains linking steps or run the `g++` link commands manually.
- If you see "No such file or directory" when executing in WSL and the project is in OneDrive, copy the project into WSL home (e.g., `~/project1`) and build there.
- UDP broadcast/discovery may be blocked on some networks (public Wi-Fi). In such cases use `--host` with the server IP.
- Partial or garbled messages usually indicate framing was not used — always use the 4-byte length prefix when sending messages to the server.

---

## 📖 Suggested usage examples

- Start server with custom TCP and discovery ports:
    
        ./build/src/server/server -p 6000 -d 60001

- Start client connecting directly:

        ./build/src/client/client --host 192.168.1.10 --port 6000

- Start client using discovery on custom discovery port:

        ./build/src/client/client -d 60001

---

## 📖 Possible extensions (ideas for follow-ups)
- Add username handling and commands (e.g., `/nick`, `/whisper`)
- Add rooms/channels so messages are scoped to a room
- Add TLS encryption to secure traffic (OpenSSL integration)
- Add authentication and rate-limiting
- Persist chat history (rotate logs)
- Implement a WebSocket front-end (bridge between native sockets and web clients)
- Port to native Windows using Winsock & WSAPoll/IOCP

---

## 📜 Security notes
- This prototype has no authentication or encryption — do not expose it directly to the public internet.  
- Work is ongoing to strengthen security, with future plans to introduce authentication mechanisms and TLS-based encryption so that clients can securely identify and connect to trusted servers.  
- UDP discovery reveals service info on the LAN; that’s fine for trusted networks but may be unwanted on untrusted networks.

---

## 📜 License
MIT License — feel free to fork and extend.

---


