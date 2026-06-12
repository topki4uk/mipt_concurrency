#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

constexpr int PORT = 8080;
constexpr int BACKLOG = 10;
constexpr size_t BUFFER_SIZE = 1024;
constexpr int MAX_EVENTS = 64;

int main() {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "bind() failed\n";
        return 1;
    }

    if (listen(server_socket, BACKLOG) < 0) {
        std::cerr << "listen() failed\n";
        return 1;
    }

    std::cout << "Echo server listening on port " << PORT << "\n";

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        std::cerr << "epoll_create1() failed\n";
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_socket;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_socket, &ev);

    std::vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        int ready = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
        if (ready < 0) {
            std::cerr << "epoll_wait() failed\n";
            return 1;
        }

        for (int i = 0; i < ready; ++i) {
            int fd = events[i].data.fd;

            if (fd == server_socket) {
                int client_socket = accept(server_socket, nullptr, nullptr);
                if (client_socket < 0) {
                    std::cerr << "accept() failed\n";
                    continue;
                }
                std::cout << "New client: " << client_socket << "\n";

                epoll_event client_ev{};
                client_ev.events = EPOLLIN;
                client_ev.data.fd = client_socket;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_socket, &client_ev);
            } else {
                char buffer[BUFFER_SIZE];
                ssize_t bytes = read(fd, buffer, sizeof(buffer));
                
                if (bytes > 0) {
                    write(fd, buffer, bytes);
                } else {
                    std::cout << "Client disconnected: " << fd << "\n";
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                }
            }
        }
    }

    close(server_socket);
    close(epfd);
    return 0;
}