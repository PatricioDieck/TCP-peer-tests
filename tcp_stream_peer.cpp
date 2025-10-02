// tcp_stream_peer.cpp
// Real-time keystroke streaming between two peers over TCP.
//   Terminal A: ./tcp_stream_peer --listen 3333
//   Terminal B: ./tcp_stream_peer --connect 127.0.0.1 3333
// Every key you press is sent immediately (no waiting for Enter).
//
// Build (macOS/Linux):
//   clang++ -std=c++20 tcp_stream_peer.cpp -o tcp_stream_peer
//
// NOTE: This uses POSIX system calls (macOS/Linux). On Windows, you'd use
//       different console and networking APIs.

// ----- system + networking includes (provided by the OS) -----
#include <arpa/inet.h>   // inet_pton, inet_ntop, htons, htonl
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <sys/socket.h>  // socket, bind, listen, accept, connect, send, recv
#include <sys/select.h>  // select (wait on keyboard + socket)
#include <unistd.h>      // read, write, close
#include <termios.h>     // terminal settings (turn off line buffering)
#include <cerrno>        // errno
#include <cstring>       // std::memset, std::strerror
#include <iostream>      // std::cout, std::cerr
#include <string>        // std::string

// ============================ Terminal helpers ============================
// We put the terminal (your shell) into "raw" mode so:
// - Each key is delivered immediately (no waiting for Enter).
// - The terminal doesn't auto-echo characters (we control output).
// We keep signals (ISIG) on so Ctrl-C still quits.

struct TerminalRawGuard {
  termios original{};
  bool active{false};

  // Put stdin into raw-ish mode (no canonical line editing, no echo)
  bool enable() {
    // Get current terminal settings for stdin (fd 0)
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
      std::cerr << "tcgetattr failed: " << std::strerror(errno) << "\n";
      return false;
    }
    termios raw = original;

    // Turn off canonical mode (ICANON): deliver input byte-by-byte
    raw.c_lflag &= ~ICANON;
    // Turn off local echo (ECHO): terminal won't print keys automatically
    raw.c_lflag &= ~ECHO;
    // Keep ISIG so Ctrl-C still sends an interrupt (so you can exit)
    // raw.c_lflag |= ISIG;  // (usually already on)

    // Minimum 1 byte to return from read, no timeout (VMIN=1, VTIME=0)
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      std::cerr << "tcsetattr failed: " << std::strerror(errno) << "\n";
      return false;
    }
    active = true;
    return true;
  }

  // Restore original terminal settings when we exit
  ~TerminalRawGuard() {
    if (active) {
      tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }
  }
};

// ============================ TCP helpers ============================

// Send the full buffer; TCP may send partial chunks, so we loop until done.
static bool send_all_bytes(int socket_fd, const char* data, size_t len) {
  size_t left = len;
  const char* p = data;
  while (left > 0) {
    ssize_t n = ::send(socket_fd, p, left, 0);
    if (n < 0) {
      if (errno == EINTR) continue; // interrupted; try again
      std::cerr << "send error: " << std::strerror(errno) << "\n";
      return false;
    }
    if (n == 0) return false;       // connection likely closed
    p += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

// Connect to a remote host:port (returns a connected socket fd, or -1 if failed)
static int connect_to_peer(const std::string& host, int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
    return -1;
  }

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(static_cast<uint16_t>(port));

  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "inet_pton failed for host: " << host << "\n";
    ::close(fd);
    return -1;
  }

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "connect() failed: " << std::strerror(errno) << "\n";
    ::close(fd);
    return -1;
  }
  return fd;
}

// Listen on a port and accept one connection (returns connected socket fd)
static int listen_and_accept(int port) {
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
    return -1;
  }

  int reuse = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY); // bind on all local addresses
  addr.sin_port        = htons(static_cast<uint16_t>(port));

  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "bind() failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd);
    return -1;
  }

  if (::listen(listen_fd, 1) < 0) { // backlog 1 is fine for this demo
    std::cerr << "listen() failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd);
    return -1;
  }

  std::cout << "listening on port " << port << " ... waiting for one peer\n";

  sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
  if (conn_fd < 0) {
    std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd);
    return -1;
  }

  char ip_text[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &peer_addr.sin_addr, ip_text, sizeof(ip_text));
  int peer_port = ntohs(peer_addr.sin_port);
  std::cout << "connected to peer " << ip_text << ":" << peer_port << "\n";

  ::close(listen_fd); // single connection for this minimal demo
  return conn_fd;
}

// ============================== Main logic ==============================

int main(int argc, char** argv) {
  // Usage:
  //   --listen <port>
  //   --connect <host> <port>
  if (argc >= 3 && std::string(argv[1]) == "--listen") {
    int port = std::stoi(argv[2]);
    int sock = listen_and_accept(port);
    if (sock < 0) return 1;

    // Put terminal into raw mode so we send each key immediately.
    TerminalRawGuard raw;
    if (!raw.enable()) {
      std::cerr << "failed to enable raw terminal mode\n";
      ::close(sock);
      return 1;
    }

    std::cout << "Real-time: type to send. Press Ctrl-C to quit.\n";

    // We will wait on both: keyboard (fd=0) and network (sock)
    while (true) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(STDIN_FILENO, &read_set);
      FD_SET(sock, &read_set);
      int max_fd = (sock > STDIN_FILENO ? sock : STDIN_FILENO);

      // Wait until either keyboard or socket has data
      int ready = ::select(max_fd + 1, &read_set, nullptr, nullptr, nullptr);
      if (ready < 0) {
        if (errno == EINTR) continue; // interrupted by signal; retry
        std::cerr << "select() failed: " << std::strerror(errno) << "\n";
        break;
      }

      // If a key was pressed, read it and send it immediately
      if (FD_ISSET(STDIN_FILENO, &read_set)) {
        char ch;
        ssize_t n = ::read(STDIN_FILENO, &ch, 1);
        if (n == 1) {
          // Optional: locally echo your keystroke so you see what you typed
          // (We print to stdout directly since echo is off)
          ::write(STDOUT_FILENO, &ch, 1);

          if (!send_all_bytes(sock, &ch, 1)) {
            std::cerr << "\nfailed to send to peer\n";
            break;
          }
        } else if (n == 0) {
          // stdin closed (rare in a terminal); exit
          std::cout << "\nstdin closed\n";
          break;
        } else { // n < 0
          if (errno == EINTR) continue;
          std::cerr << "\nread(stdin) failed: " << std::strerror(errno) << "\n";
          break;
        }
      }

      // If the network socket has data, read a chunk and print it as-is
      if (FD_ISSET(sock, &read_set)) {
        char buf[4096];
        ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n == 0) {
          std::cout << "\npeer disconnected\n";
          break;
        }
        if (n < 0) {
          if (errno == EINTR) continue;
          std::cerr << "\nrecv() failed: " << std::strerror(errno) << "\n";
          break;
        }
        // Print exactly what arrived (raw stream)
        ::write(STDOUT_FILENO, buf, static_cast<size_t>(n));
      }
    }

    ::close(sock);
    return 0;
  }
  else if (argc >= 4 && std::string(argv[1]) == "--connect") {
    std::string host = argv[2];
    int port = std::stoi(argv[3]);
    int sock = connect_to_peer(host, port);
    if (sock < 0) return 1;

    TerminalRawGuard raw;
    if (!raw.enable()) {
      std::cerr << "failed to enable raw terminal mode\n";
      ::close(sock);
      return 1;
    }

    std::cout << "Real-time: type to send. Press Ctrl-C to quit.\n";

    while (true) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(STDIN_FILENO, &read_set);
      FD_SET(sock, &read_set);
      int max_fd = (sock > STDIN_FILENO ? sock : STDIN_FILENO);

      int ready = ::select(max_fd + 1, &read_set, nullptr, nullptr, nullptr);
      if (ready < 0) {
        if (errno == EINTR) continue;
        std::cerr << "select() failed: " << std::strerror(errno) << "\n";
        break;
      }

      if (FD_ISSET(STDIN_FILENO, &read_set)) {
        char ch;
        ssize_t n = ::read(STDIN_FILENO, &ch, 1);
        if (n == 1) {
          ::write(STDOUT_FILENO, &ch, 1);
          if (!send_all_bytes(sock, &ch, 1)) {
            std::cerr << "\nfailed to send to peer\n";
            break;
          }
        } else if (n == 0) {
          std::cout << "\nstdin closed\n";
          break;
        } else {
          if (errno == EINTR) continue;
          std::cerr << "\nread(stdin) failed: " << std::strerror(errno) << "\n";
          break;
        }
      }

      if (FD_ISSET(sock, &read_set)) {
        char buf[4096];
        ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n == 0) {
          std::cout << "\npeer disconnected\n";
          break;
        }
        if (n < 0) {
          if (errno == EINTR) continue;
          std::cerr << "\nrecv() failed: " << std::strerror(errno) << "\n";
          break;
        }
        ::write(STDOUT_FILENO, buf, static_cast<size_t>(n));
      }
    }

    ::close(sock);
    return 0;
  }

  std::cerr << "Usage:\n"
            << "  " << argv[0] << " --listen <port>\n"
            << "  " << argv[0] << " --connect <host> <port>\n";
  return 1;
}
