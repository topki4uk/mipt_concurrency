#include <iostream>
#include <unordered_map>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>

constexpr int PORT = 8080;
constexpr int BACKLOG = 10;
constexpr size_t BUFFER_SIZE = 1024;
constexpr int QUEUE_DEPTH = 256;


enum class EventType {
    Accept,
    Read,
    Write,
};

struct Event {
    EventType type;
    int fd;
    char buffer[BUFFER_SIZE];
    size_t length;
};


void add_accept(io_uring& ring, int server_socket, sockaddr_in& client_addr, socklen_t& client_len) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "submission queue is full\n";
        return;
    }

    Event* event = new Event{EventType::Accept, server_socket};

    io_uring_prep_accept(
        sqe,
        server_socket,
        reinterpret_cast<sockaddr*>(&client_addr),
        &client_len,
        0
    );

    io_uring_sqe_set_data(sqe, event);
    io_uring_submit(&ring);
}

void add_read(io_uring& ring, int fd) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "submission queue is full\n";
        return;
    }

    Event* event = new Event{EventType::Read, fd};

    io_uring_prep_read(sqe, fd, event->buffer, BUFFER_SIZE, 0);

    io_uring_sqe_set_data(sqe, event);
    io_uring_submit(&ring);
}


void add_write(io_uring& ring, int fd, const char* buffer, size_t length) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "submission queue is full\n";
        return;
    }

    Event* event = new Event{EventType::Write, fd};

    memcpy(event->buffer, buffer, length);
    event->length = length;

    io_uring_prep_write(sqe, fd, event->buffer, length, 0);

    io_uring_sqe_set_data(sqe, event);
    io_uring_submit(&ring);
}


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

    io_uring ring;
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        std::cerr << "io_uring_queue_init() failed\n";
        return 1;
    }

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    add_accept(ring, server_socket, client_addr, client_len);

    while (true) {
        io_uring_cqe* cqe;

        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            std::cerr << "io_uring_wait_cqe() failed\n";
            return 1;
        }

        Event* event = reinterpret_cast<Event*>(io_uring_cqe_get_data(cqe));

        int result = cqe->res;

        io_uring_cqe_seen(&ring, cqe);

        switch (event->type) {
            case EventType::Accept: {
                delete event;

                if (result < 0) {
                    std::cerr << "accept() failed\n";
                } else {
                    int client_socket = result;
                    std::cout << "New client: " << client_socket << "\n";

                    add_read(ring, client_socket);
                }

                add_accept(ring, server_socket, client_addr, client_len);
                break;
            }

            case EventType::Read: {
                int fd = event->fd;

                if (result <= 0) {
                    std::cout << "Client disconnected: " << fd << "\n";
                    close(fd);
                    delete event;
                } else {
                    add_write(ring, fd, event->buffer, result);
                    delete event;
                }
                break;
            }

            case EventType::Write: {
                int fd = event->fd;
                delete event;

                if (result < 0) {
                    std::cerr << "write() failed\n";
                    close(fd);
                } else {
                    add_read(ring, fd);
                }
                break;
            }
        }
    }

    io_uring_queue_exit(&ring);
    close(server_socket);
    return 0;
}