# socks5-fz

A lightweight **SOCKS5 proxy (server + client)** written in **C**, built on **epoll (level-triggered)** and **non-blocking sockets**, driven by a single-threaded event loop and a TCP tunnel state machine.

This project is an enhanced fork (by **wFZ**) of [bhhbazinga/socks5](https://github.com/bhhbazinga/socks5). It adds a **tunnel reuse / keep-alive mechanism** and a **companion SOCKS5 client** (`clientdemo`) in addition to the original server.

> Only UNIX/Linux platforms are supported (requires `epoll`, POSIX sockets).

---

## Features

### SOCKS5 Server (`socks5`)
- Single-threaded event loop based on `epoll` (level-triggered) + non-blocking sockets
- No authentication and Username/Password authentication (RFC 1929)
- IPv4 / IPv6 / domain-name resolution via `getaddrinfo`
- `CONNECT` command
- **Dual listen ports:**
  - `-p` : standard SOCKS5 proxy port (for proxifiers / browsers / `curl`)
  - `-P` : private tunnel port (for pairing with the built-in client)
- **Tunnel reuse (wFZ extension):** idle established tunnels are paired with pending requests through a connection pool, avoiding a new TCP connection for every request
- HTTP `Connection: keep-alive` detection (`parsingHeader`) to decide whether a tunnel socket should be kept alive and reused

### SOCKS5 Client (`clientdemo`)
- A companion client / *mediator* that actively connects to the server's private tunnel port (`-P`)
- Non-blocking `connect` with `getaddrinfo` multi-address fallback
- Pairs with the server-side connection pool to form reusable tunnels

### Common infrastructure
- `buff.c` — growable I/O buffer tackling TCP packet fragmentation/coalescing
- `list.h` — Linux-kernel-style intrusive doubly linked list
- `Utils.c` — connection-pool management: AppList (active tunnels) / waitList (tunnels waiting to be paired) / FdList (raw fds)
- `ventry.h` — `container_of`-style macro (`vbody_entry`) compatible with both GCC and non-GCC compilers

---

## Build

```sh
make
```

Produces two binaries in the project root:

| Binary | Description |
|--------|-------------|
| `socks5`     | SOCKS5 proxy server |
| `clientdemo` | SOCKS5 tunnel client / mediator |

Clean:

```sh
make clean
```

Requires `gcc` and standard POSIX headers. Compile flags: `-Wall -g -std=c99`.

---

## Usage

### Server

```
Usage:
  -a <ip>  bind address
  -p <port>       proxy listen port
  -P <port>       private tunnel listen port (optional, for client pairing)
  -u <username>   username (optional, enables auth)
  -k <password>   password (optional, enables auth)
```

```sh
# Basic proxy, no auth
./socks5 -a 0.0.0.0 -p 1080

# With username/password authentication
./socks5 -a 0.0.0.0 -p 1080 -u abc123 -k qwe123

# With private tunnel port for the built-in client
./socks5 -a 0.0.0.0 -p 1080 -P 6080
```

### Client (`clientdemo`)

```
Usage:
  -a <ip>  server address
  -p <port>       server media/private port to connect to
  -P <port>       remote port (optional)
  -u <username>   username (optional)
  -k <password>   password (optional)
```

```sh
./clientdemo -a 192.168.1.40 -p 1080 -u abc123 -k qwe123
```

---

## Quick test

```sh
# 1. Start the server
./socks5 -a 0.0.0.0 -p 6080

# 2. Route traffic through it
curl --socks5 127.0.0.1:6080 http://www.baidu.com
curl --socks5 127.0.0.1:6080 http://192.168.150.128:8090
```

---

## Architecture

```
                          ┌───────────────────────────────┐
 client/proxifier ──────▶ │  socks5  server  (epoll LT)   │
 (SOCKS5 CONNECT)         │  state machine:               │
                          │   open → auth → request       │
                          │   → connecting → connected    │
                          │   → waitting (reuse)          │
                          └──────────────┬────────────────┘
                                         │ private tunnel port (-P)
                          ┌──────────────▼────────────────┐
      clientdemo ────────▶│  connection pool / pairing    │
    (mediator)            │  AppList ↔ waitList           │
                          └───────────────────────────────┘
```

### Tunnel state machine

| State | Description |
|-------|-------------|
| `open_state` | Client sent the SOCKS5 greeting (`VER`/`NMETHODS`) |
| `auth_state` | Username/password negotiation (when enabled) |
| `request_state` | Parsing the CONNECT request (`CMD`/`ATYP`/addr/port) |
| `connecting_state` | Connecting to the remote target (non-blocking) |
| `connected_state` | Bidirectional data relay between client and remote |
| `tunnel_waitting` | (wFZ) Tunnel finished writing and is kept alive, waiting to be paired with the next request |

Each tunnel pair (`priv_sock` ↔ `remote_sock`) is tracked in `AppList` / `waitList`; `waitLinePairing()` runs after every epoll round to match idle tunnels with pending requests, reusing the established TCP connection.

---

## Project layout

```
├── socks5.c        # SOCKS5 proxy server (main entry, state machine, tunnel pool)
├── clientdemo.c    # SOCKS5 tunnel client / mediator
├── mysocks5.h      # shared headers: protocol structs, tunnel/sock/server types
├── util.h          # list-based pool APIs
├── buff.c / buff.h # growable I/O buffer
├── Utils.c         # AppList / waitList / FdList pool implementations
├── list.h          # intrusive doubly linked list
├── ventry.h        # container_of macro (vbody_entry)
├── leveltrg_demo.c # educational demo of epoll level-triggered mode
├── Makefile
└── screenshot/     # usage screenshots
```

---

## TODO

- Asynchronous DNS resolution and cache (currently `getaddrinfo` blocks)
- Encrypted transmission
- `BIND` and `ASSOCIATE` command support
- Boundary checks for tunnel pool entries

---

## License & acknowledgment

Enhanced fork of [bhhbazinga/socks5](https://github.com/bhhbazinga/socks5) (wFZ). See the LICENSE of the original project.
