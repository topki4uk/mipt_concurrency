#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>

int main() {
    // 1. Создать сокет
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    // 2. Указать адрес сервера
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    //  ↑ преобразует строку "127.0.0.1" в бинарный формат

    // 3. Подключиться
    connect(sock, (sockaddr*)&server_addr, sizeof(server_addr));
    std::cout << "Connected to server!\n";

    // 4. Обмен данными
    std::string message = "Hello, server!";
    write(sock, message.c_str(), message.size());

    // 5. Закрыть соединение
    close(sock);
}