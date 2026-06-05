#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <unistd.h>
#include <cstring>

int main() {
    // 1. Создать сокет
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // 2. Привязать к порту
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));

    // 3. Начать слушать
    listen(server_fd, 10);

    // 4. Основной цикл: принимать клиентов
    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);

        // Читаем и отвечаем (эхо)
        char buf[1024]{};
        while (true) {
            ssize_t n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) break; // клиент отключился
            // write(client_fd, buf, n);
            printf("%s", buf);
        }

        printf("Client with id=%d quiet\n", client_fd);
        close(client_fd); // закрываем соединение с этим клиентом
    }

    close(server_fd);
}