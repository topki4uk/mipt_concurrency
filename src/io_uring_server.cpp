#include <iostream>
#include <unordered_map>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>

// Порт, на котором сервер будет слушать входящие подключения
constexpr int PORT = 8080;

// Максимальное количество ожидающих подключений в listen()
constexpr int BACKLOG = 10;

// Размер буфера для чтения/записи данных клиента
constexpr size_t BUFFER_SIZE = 1024;

// Глубина очереди io_uring — сколько операций можно держать одновременно
constexpr int QUEUE_DEPTH = 256;


// Типы событий, которые мы ассоциируем с запросами в io_uring
enum class EventType {
    Accept, // Ожидание нового клиентского подключения
    Read,   // Чтение данных из сокета клиента
    Write,  // Запись данных в сокет клиента
};


// Структура пользовательских данных, привязанных к каждому SQE.
// Через неё мы понимаем, какое именно событие завершилось.
struct Event {
    EventType type;             // Тип операции
    int fd;                     // Файловый дескриптор сокета
    char buffer[BUFFER_SIZE];   // Буфер для чтения или записи
    size_t length;              // Длина данных для записи
};


// Добавляет в io_uring асинхронную операцию accept()
void add_accept(io_uring& ring, int server_socket, sockaddr_in& client_addr, socklen_t& client_len) {
    // Получаем свободный submission queue entry
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "submission queue is full\n";
        return;
    }

    // Создаём объект события и помечаем его как Accept
    Event* event = new Event{EventType::Accept, server_socket};

    // Подготавливаем accept-запрос
    io_uring_prep_accept(
        sqe,
        server_socket,
        reinterpret_cast<sockaddr*>(&client_addr),
        &client_len,
        0
    );

    // Сохраняем указатель на событие в user_data,
    // чтобы потом получить его при завершении операции
    io_uring_sqe_set_data(sqe, event);

    // Отправляем SQE в ядро
    io_uring_submit(&ring);
}


// Добавляет асинхронное чтение из клиентского сокета
void add_read(io_uring& ring, int fd) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "submission queue is full\n";
        return;
    }

    // Создаём событие чтения
    Event* event = new Event{EventType::Read, fd};

    // Готовим read-запрос:
    // читаем до BUFFER_SIZE байт в event->buffer
    io_uring_prep_read(sqe, fd, event->buffer, BUFFER_SIZE, 0);

    // Привязываем событие к SQE
    io_uring_sqe_set_data(sqe, event);

    // Отправляем запрос
    io_uring_submit(&ring);
}


// Добавляет асинхронную запись в клиентский сокет
void add_write(io_uring& ring, int fd, const char* buffer, size_t length) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "submission queue is full\n";
        return;
    }

    // Создаём событие записи
    Event* event = new Event{EventType::Write, fd};

    // Копируем данные в собственный буфер события,
    // потому что исходный buffer может стать недействительным
    // к моменту выполнения асинхронной операции
    memcpy(event->buffer, buffer, length);
    event->length = length;

    // Готовим write-запрос
    io_uring_prep_write(sqe, fd, event->buffer, length, 0);

    // Привязываем событие к SQE
    io_uring_sqe_set_data(sqe, event);

    // Отправляем запрос
    io_uring_submit(&ring);
}


int main() {
    // Создаём TCP-сокет IPv4
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

    // Разрешаем быстро повторно использовать адрес после перезапуска сервера
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Настраиваем адрес сервера
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;         // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // Принимать подключения на всех интерфейсах
    server_addr.sin_port = htons(PORT);       // Порт в сетевом порядке байт

    // Привязываем сокет к адресу и порту
    if (bind(server_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "bind() failed\n";
        return 1;
    }

    // Переводим сокет в режим прослушивания входящих подключений
    if (listen(server_socket, BACKLOG) < 0) {
        std::cerr << "listen() failed\n";
        return 1;
    }

    std::cout << "Echo server listening on port " << PORT << "\n";

    // Инициализируем io_uring
    io_uring ring;
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        std::cerr << "io_uring_queue_init() failed\n";
        return 1;
    }

    // Буфер под адрес клиента для accept()
    // Важно: здесь используется один и тот же client_addr/client_len для всех accept'ов
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    // Ставим первую операцию accept в очередь,
    // чтобы сервер начал принимать подключения
    add_accept(ring, server_socket, client_addr, client_len);

    while (true) {
        io_uring_cqe* cqe;

        // Ждём завершения любой операции из очереди
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            std::cerr << "io_uring_wait_cqe() failed\n";
            return 1;
        }

        // Достаём пользовательские данные, которые привязали к SQE
        Event* event = reinterpret_cast<Event*>(io_uring_cqe_get_data(cqe));

        // Результат операции:
        // для accept() — новый fd клиента,
        // для read()/write() — число обработанных байт,
        // при ошибке — отрицательный код
        int result = cqe->res;

        // Сообщаем io_uring, что CQE обработан
        io_uring_cqe_seen(&ring, cqe);

        switch (event->type) {
            case EventType::Accept: {
                // Операция accept завершилась, событие можно удалить
                delete event;

                if (result < 0) {
                    std::cerr << "accept() failed\n";
                } else {
                    // result содержит fd нового клиентского сокета
                    int client_socket = result;
                    std::cout << "New client: " << client_socket << "\n";

                    // Сразу начинаем читать данные от нового клиента
                    add_read(ring, client_socket);
                }

                // Независимо от результата снова ставим accept,
                // чтобы продолжать принимать новых клиентов
                add_accept(ring, server_socket, client_addr, client_len);
                break;
            }

            case EventType::Read: {
                int fd = event->fd;

                if (result <= 0) {
                    // result == 0 означает EOF: клиент закрыл соединение
                    // result < 0 означает ошибку чтения
                    std::cout << "Client disconnected: " << fd << "\n";
                    close(fd);
                    delete event;
                } else {
                    // Если что-то прочитали, отправляем те же данные обратно клиенту
                    // Это и есть поведение echo server
                    add_write(ring, fd, event->buffer, result);
                    delete event;
                }
                break;
            }

            case EventType::Write: {
                int fd = event->fd;
                delete event;

                if (result < 0) {
                    // Ошибка записи — закрываем соединение
                    std::cerr << "write() failed\n";
                    close(fd);
                } else {
                    // После успешной записи снова ждём данные от клиента
                    add_read(ring, fd);
                }
                break;
            }
        }
    }

    // До этих строк программа не дойдёт из-за бесконечного цикла,
    // но в корректной версии сервера здесь должна быть очистка ресурсов
    io_uring_queue_exit(&ring);
    close(server_socket);
    return 0;
}