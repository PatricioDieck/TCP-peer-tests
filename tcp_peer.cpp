// tcp_peer.cpp
// A single program that can either:
//   - wait for a connection (--listen PORT), or
//   - make a connection   (--connect HOST PORT).
// After connected, you can type lines and press Enter to send;
// incoming lines are printed to the screen.
//
// Build:  clang++ -std=c++20 tcp_peer.cpp -o tcp_peer
//
// Run examples:
//   Terminal A: ./tcp_peer --listen 3333
//   Terminal B: ./tcp_peer --connect 127.0.0.1 3333
//
// Notes:
// - This uses plain TCP (no HTTP). Both sides are equal once connected.
// - Each "message" is a line of text ending in '\n' (newline).
// - No extra libraries; uses the OS socket calls available on Linux/macOS.

#include <arpa/inet.h>   // inet_pton, inet_ntop, htons, htonl
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <sys/socket.h>  // socket, bind, listen, accept, connect, send, recv
#include <sys/select.h>  // select to wait on stdin and socket together
#include <unistd.h>      // read, write, close
#include <cerrno>        // errno
#include <cstring>       // std::memset, std::strerror
#include <iostream>      // std::cout, std::cerr
#include <string>        // std::string

// -------------- tiny helpers --------------

// send_all_bytes: ensure we send the whole string (TCP may send in chunks)
static bool send_all_bytes(int socket_fd, const std::string& text) {
  const char* data = text.data();
  size_t left = text.size();
  while (left > 0) {
    ssize_t n = ::send(socket_fd, data, left, 0);
    if (n < 0) {
      if (errno == EINTR) continue; // interrupted; try again
      std::cerr << "send error: " << std::strerror(errno) << "\n";
      return false;
    }
    if (n == 0) return false;       // peer closed unexpectedly
    data += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

// connect_to_peer: make an outgoing TCP connection to host:port
static int connect_to_peer(const std::string& host, int port) {
  // Create a TCP socket
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
    return -1;
  }

  // Fill the server address (IPv4)
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));

  // Convert human text ("127.0.0.1") to binary
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "inet_pton failed for host: " << host << "\n";
    ::close(fd);
    return -1;
  }

  // Try to connect
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "connect() failed: " << std::strerror(errno) << "\n";
    ::close(fd);
    return -1;
  }

  return fd; // success: this is the connected socket
}

// listen_and_accept: wait for one incoming TCP connection on port
static int listen_and_accept(int port) {
  // Create a TCP socket
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
    return -1;
  }

  // Allow quick restart on same port
  int reuse = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  // Bind to all network interfaces on given port
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));

  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "bind() failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd);
    return -1;
  }

  // Start listening; backlog = 1 (we accept only one peer in this toy)
  if (::listen(listen_fd, 1) < 0) {
    std::cerr << "listen() failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd);
    return -1;
  }

  std::cout << "listening on port " << port << " ... waiting for one peer\n";

  // Wait for one incoming connection
  sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
  if (conn_fd < 0) {
    std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
    ::close(listen_fd);
    return -1;
  }

  // Optional: print where they came from
  char ip_text[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &peer_addr.sin_addr, ip_text, sizeof(ip_text));
  int peer_port = ntohs(peer_addr.sin_port);
  std::cout << "connected to peer " << ip_text << ":" << peer_port << "\n";

  // We no longer need the listening socket (single connection toy)
  ::close(listen_fd);
  return conn_fd; // success: this is the connected socket
}

// -------------- main --------------

int main(int argc, char** argv) {
  // Very simple argument handling:
  //   --listen PORT
  //   --connect HOST PORT
  if (argc >= 3 && std::string(argv[1]) == "--listen") {
    int port = std::stoi(argv[2]);
    int socket_fd = listen_and_accept(port);
    if (socket_fd < 0) return 1;

    std::cout << "type a message and press Enter to send; Ctrl+D to quit\n";

    // We will use select() to watch both:
    //   - your keyboard (stdin, fd=0)
    //   - the network socket (socket_fd)
    std::string incoming_buffer; // holds partial bytes until a full line arrives

    while (true) {
      // Build a set of fds we want to watch
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(0, &read_set);              // 0 = stdin
      FD_SET(socket_fd, &read_set);      // network socket

      // Wait until either stdin OR the socket has data
      int max_fd = (socket_fd > 0 ? socket_fd : 0);
      int ready = ::select(max_fd + 1, &read_set, nullptr, nullptr, nullptr);
      if (ready < 0) {
        if (errno == EINTR) continue;    // interrupted by signal; retry
        std::cerr << "select() failed: " << std::strerror(errno) << "\n";
        break;
      }

      // If there's keyboard input ready, read a line and send it to the peer
      if (FD_ISSET(0, &read_set)) {
        std::string line;
        if (!std::getline(std::cin, line)) {
          std::cout << "stdin closed; goodbye\n";
          break;
        }
        // Add the newline that marks the end of our message
        line.push_back('\n');
        if (!send_all_bytes(socket_fd, line)) {
          std::cerr << "failed to send to peer\n";
          break;
        }
      }

      // If the socket has data, read some bytes and print full lines as they arrive
      if (FD_ISSET(socket_fd, &read_set)) {
        char chunk[4096];
        ssize_t n = ::recv(socket_fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
          std::cout << "peer disconnected\n";
          break;
        }
        if (n < 0) {
          if (errno == EINTR) continue;
          std::cerr << "recv() failed: " << std::strerror(errno) << "\n";
          break;
        }

        // Append these bytes to our buffer
        incoming_buffer.append(chunk, static_cast<size_t>(n));

        // Pull out complete lines (messages end with '\n')
        while (true) {
          size_t pos = incoming_buffer.find('\n');
          if (pos == std::string::npos) break;
          std::string one_line = incoming_buffer.substr(0, pos);
          incoming_buffer.erase(0, pos + 1);
          std::cout << "[peer] " << one_line << "\n";
        }
      }
    }

    ::close(socket_fd);
    return 0;
  }
  else if (argc >= 4 && std::string(argv[1]) == "--connect") {
    std::string host = argv[2];
    int port = std::stoi(argv[3]);
    int socket_fd = connect_to_peer(host, port);
    if (socket_fd < 0) return 1;

    std::cout << "connected to " << host << ":" << port << "\n";
    std::cout << "type a message and press Enter to send; Ctrl+D to quit\n";

    std::string incoming_buffer;

    while (true) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(0, &read_set);
      FD_SET(socket_fd, &read_set);

      int max_fd = (socket_fd > 0 ? socket_fd : 0);
      int ready = ::select(max_fd + 1, &read_set, nullptr, nullptr, nullptr);
      if (ready < 0) {
        if (errno == EINTR) continue;
        std::cerr << "select() failed: " << std::strerror(errno) << "\n";
        break;
      }

      if (FD_ISSET(0, &read_set)) {
        std::string line;
        if (!std::getline(std::cin, line)) {
          std::cout << "stdin closed; goodbye\n";
          break;
        }
        line.push_back('\n');
        if (!send_all_bytes(socket_fd, line)) {
          std::cerr << "failed to send to peer\n";
          break;
        }
      }

      if (FD_ISSET(socket_fd, &read_set)) {
        char chunk[4096];
        ssize_t n = ::recv(socket_fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
          std::cout << "peer disconnected\n";
          break;
        }
        if (n < 0) {
          if (errno == EINTR) continue;
          std::cerr << "recv() failed: " << std::strerror(errno) << "\n";
          break;
        }

        incoming_buffer.append(chunk, static_cast<size_t>(n));
        while (true) {
          size_t pos = incoming_buffer.find('\n');
          if (pos == std::string::npos) break;
          std::string one_line = incoming_buffer.substr(0, pos);
          incoming_buffer.erase(0, pos + 1);
          std::cout << "[peer] " << one_line << "\n";
        }
      }
    }

    ::close(socket_fd);
    return 0;
  }

  // If we got here, the arguments were wrong; show help.
  std::cerr << "Usage:\n"
            << "  " << argv[0] << " --listen <port>\n"
            << "  " << argv[0] << " --connect <host> <port>\n";
  return 1;
}
