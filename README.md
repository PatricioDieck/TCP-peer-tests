# 🐣 BabyTCP 

A minimal implementation of TCP peer-to-peer communication in C++, built from scratch to understand the networking layer that powers Bitcoin's P2P protocol.

No HTTP. No WebSockets. No frameworks. Just raw TCP sockets and the system calls that Bitcoin Core uses under the hood.

## What is this?

A simple program that demonstrates TCP peer-to-peer communication with line-delimited messaging.

The program is symmetric: after connection, either peer can send/receive data freely. This mirrors Bitcoin's P2P network, where nodes communicate bidirectionally without a client-server hierarchy.

## Build

```bash
clang++ -std=c++20 tcp_peer.cpp -o tcp_peer
```

## Usage

Terminal A (listener):
```bash
./tcp_peer --listen 3333
```

Terminal B (connector):
```bash
./tcp_peer --connect 127.0.0.1 3333
```


## Mental Model: TCP in the Context of Bitcoin Communication

### The Core Abstraction

Think of TCP as a **reliable, ordered pipe of bytes** between two programs.

Once connected, bytes you `send()` on one side arrive on the other side in the same order. That's TCP's promise: reliable, ordered, byte stream.

### No Message Boundaries

TCP doesn't know what a "message" is—it gives you a continuous stream of bytes.

If you send `"HELLO\nWORLD\n"`, the receiver might get:
- `"HELLO\nWOR"` in the first `recv()`
- `"LD\n"` in the second

This is normal. You must reassemble the stream into complete messages.

This is why protocols like Bitcoin's P2P invent their own **framing rules**:

1. **Delimiter-based** (newline, null byte, etc.)
   - Used by: SMTP, IRC, Redis
   - Example: `"TX <hex>\n"`
   - This is what `tcp_peer` implements

2. **Length-prefix** (first N bytes = message size)
   - Used by: Bitcoin Core (24-byte header with length + command)
   - More efficient for binary protocols
   - No scanning for delimiters

3. **Fixed-size records**
   - Rare; only when all messages are the same length

There's no universal standard. Each protocol defines its own framing.

### Symmetric Communication

At the TCP layer, the only asymmetry is who initiates:

- **Server**: calls `bind()` to a port (e.g., 8333) and waits (`listen()`, `accept()`)
- **Client**: calls `connect()` to that IP:port

After the three-way handshake (SYN/SYN-ACK/ACK), both sides are equal peers.

Either can send/receive data freely. This is why in Bitcoin, both peers gossip transactions and blocks the same way—no hierarchy.

### Key Properties

**Reliability**: TCP detects packet loss and retransmits. You never see dropped or duplicated data.

**Ordering**: TCP reorders packets so you always read bytes in the correct sequence.

**Flow control & congestion control**: TCP prevents one side from overwhelming the other or the network.

**Full-duplex**: Both sides can send simultaneously (bidirectional streaming).

### Why TCP Instead of HTTP/WebSockets?

With HTTP/WebSockets, you inherit protocol rules: headers, upgrades, request/response patterns, MIME types.

With raw TCP, you have full control:
- Define your own message format
- Send data whenever you want (no request/response requirement)
- Push data instantly, no polling
- Choose your own framing (delimiter, length-prefix, or custom binary)

Bitcoin Core uses raw TCP for exactly this reason: complete control over the protocol, optimized for P2P gossip.

### Bidirectional Streaming in Practice

Each peer has a single socket connected to the other.

Both peers run read loops and write loops on the same connection.

In Bitcoin's P2P network:
1. Node A connects to Node B on port 8333
2. Both nodes can now send/receive simultaneously
3. When Node A receives a new transaction, it validates and forwards to all connected peers
4. When Node B receives a new block, same thing

You don't "open ports for each direction"—a single TCP connection is already bidirectional.

### The Complete Picture

TCP provides:
- `socket()` - create a socket
- `connect()` - dial a peer
- `send()` - write bytes to the pipe
- `recv()` - read bytes from the pipe
- `close()` - tear down the connection

Everything higher-level (HTTP, WebSockets, Bitcoin P2P) is just conventions layered on top of these primitives.

## Acknowledgments

Inspired by the simplicity and educational approach of [karpathy/nanoGPT](https://github.com/karpathy/nanoGPT). This project aims to demystify the networking layer that Bitcoin builds upon.

## Notes

- Works on macOS & Linux (POSIX sockets)
- No external dependencies
- Single connection per instance (minimalist by design)
- No encryption (Bitcoin Core uses TCP with no transport-layer encryption; application-layer encryption happens via the P2P protocol itself)