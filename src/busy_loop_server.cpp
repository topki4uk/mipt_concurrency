// g++ busy_loop_echo_server.cpp -std=c++17 -pthread -o server

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <queue>

const int PORT = 8080;
const int BACKLOG = 10;
const std::size_t BUFFER_SIZE = 1024;

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);

  if (flags == -1) {
    return -1;
  }

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (server_fd < 0) {
    std::cerr << "socket() failed\n";
    return 1;
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);

  if (bind(server_fd, (sockaddr*) &addr, sizeof(addr)) < 0) {
    std::cerr << "bind() failed\n";
    return 1;
  }

  if (listen(server_fd, BACKLOG) < 0) {
    std::cerr << "listen() failed\n";
    return 1;
  }

  set_nonblocking(server_fd);

  std::cout << "Busy loop polling echo server listening on port " << PORT << std::endl;

  std::queue<int> clients;

  while (true) {
    while (true) {
      int client_fd = accept(server_fd, nullptr, nullptr);

      if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }

        std::cerr << "accept() failed\n";
        break;
      }

      set_nonblocking(client_fd);

      clients.push(client_fd);
    }

    std::size_t size = clients.size();

    for (std::size_t i = 0; i < size; ++i) {
      int client_fd = clients.front();
      clients.pop();

      char buffer[BUFFER_SIZE];
      ssize_t length = read(client_fd, buffer, sizeof(buffer));

      if (length > 0) {
        write(client_fd, buffer, length);
        clients.push(client_fd);
      } else if (length == 0) {
        std::cout << "Client " << client_fd << " disconnected" << std::endl;
        close(client_fd);
      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          clients.push(client_fd);
        } else {
          std::cerr << "Error on client " << client_fd << ": " << strerror(errno) << std::endl;
          close(client_fd);
        }
      }
    }
  }

  close(server_fd);

  return 0;
}