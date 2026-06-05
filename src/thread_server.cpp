#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <thread>
#include <iostream>

// Функция для обслуживания одного клиента (выполняется в отдельном потоке)
void handle_client(int client_fd) {
    char buf[1024];
    while (true) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n <= 0) break;  // клиент отключился или ошибка
        // write(client_fd, buf, n); // эхо: отправляем обратно

        printf("Client with id=%d write message %s", client_fd, buf);
    }

    printf("Client with id=%d quiet\n", client_fd);
    close(client_fd);
    // поток завершается сам по себе
}

int main() {
    // 1. Создать сокет
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Опция для переиспользования порта (избегает "Address already in use")
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Привязать к адресу и порту
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));

    // 3. Начать слушать
    listen(server_fd, 10);
    std::cout << "Server listening on port 8080\n";

    // 4. Основной цикл: принимаем клиентов и спавним потоки
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) continue;  // ошибка accept — пропускаем

        // Создаём поток и detach (поток живёт независимо)
        std::thread(handle_client, client_fd).detach();
    }

    close(server_fd);
}