#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <thread>
#include <vector>
#include <atomic>

constexpr int NUM_WORKERS = 4;
constexpr int MAX_EVENTS  = 64;

// Установить O_NONBLOCK на fd
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Один воркер: крутит epoll_wait и обрабатывает события
void worker(int epfd) {
    epoll_event events[MAX_EVENTS];

    while (true) {
        // Блокируемся до появления готовых fd — CPU не тратим
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (ev & (EPOLLHUP | EPOLLERR)) {
                // Соединение разорвано
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                continue;
            }

            if (ev & EPOLLIN) {
                // Читаем все доступные данные (ET режим)
                while (true) {
                    char buf[4096];
                    ssize_t bytes = read(fd, buf, sizeof(buf));

                    if (bytes > 0) {
                        // Эхо: отправляем обратно
                        write(fd, buf, bytes);
                    } else if (bytes == 0) {
                        // EOF
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        break;
                    } else {
                        if (errno == EAGAIN) break; // прочитали всё
                        // Реальная ошибка
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        close(fd);
                        break;
                    }
                }
            }
        }
    }
}

int main() {
    // Создаём epoll-ы для воркеров
    std::vector<int> worker_epfds(NUM_WORKERS);
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_epfds[i] = epoll_create1(0);
    }

    // Запускаем воркеры
    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_WORKERS; i++) {
        workers.emplace_back(worker, worker_epfds[i]);
    }

    // Серверный сокет
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 128);

    // Accept loop: принимаем клиентов, раздаём воркерам
    std::atomic<int> next_worker{0};

    while (true) {
        int client_fd = accept4(server_fd, nullptr, nullptr,
                                SOCK_NONBLOCK); // сразу неблокирующий
        if (client_fd < 0) continue;

        // Round-robin по воркерам
        int w = next_worker.fetch_add(1) % NUM_WORKERS;

        // Регистрируем в epoll воркера
        epoll_event ev;
        ev.events  = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLERR;
        ev.data.fd = client_fd;
        epoll_ctl(worker_epfds[w], EPOLL_CTL_ADD, client_fd, &ev);
    }
}