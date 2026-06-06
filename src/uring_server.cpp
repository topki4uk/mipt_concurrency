#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>

constexpr int QUEUE_DEPTH = 256;
constexpr int BUF_SIZE    = 4096;

// Тип операции для user_data
enum class OpType : uint8_t { ACCEPT, READ, WRITE };

struct Request {
    OpType   type;
    int      fd;
    char     buf[BUF_SIZE];
    sockaddr_in client_addr;
    socklen_t   client_len = sizeof(client_addr);
};

// Поставить async accept в очередь
void add_accept(io_uring* ring, int server_fd) {
    auto* req  = new Request{OpType::ACCEPT, server_fd};
    auto* sqe  = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, server_fd,
                         (sockaddr*)&req->client_addr,
                         &req->client_len, 0);
    io_uring_sqe_set_data(sqe, req);
}

// Поставить async read в очередь
void add_read(io_uring* ring, int client_fd) {
    auto* req = new Request{OpType::READ, client_fd};
    auto* sqe = io_uring_get_sqe(ring);
    io_uring_prep_read(sqe, client_fd,
                       req->buf, BUF_SIZE, 0);
    io_uring_sqe_set_data(sqe, req);
}

// Поставить async write в очередь
void add_write(io_uring* ring, Request* req, int bytes) {
    req->type = OpType::WRITE;
    auto* sqe = io_uring_get_sqe(ring);
    io_uring_prep_write(sqe, req->fd,
                        req->buf, bytes, 0);
    io_uring_sqe_set_data(sqe, req);
}

void event_loop(int server_fd) {
    io_uring ring;
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    // Стартуем с одного accept
    add_accept(&ring, server_fd);
    io_uring_submit(&ring);

    while (true) {
        // Ждём хотя бы одно завершение
        io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);

        // Обрабатываем все готовые CQE за раз
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {
            count++;
            auto* req = (Request*)io_uring_cqe_get_data(cqe);
            int   res = cqe->res;

            switch (req->type) {

            case OpType::ACCEPT:
                if (res >= 0) {
                    int client_fd = res;
                    add_read(&ring, client_fd); // сразу читаем нового клиента
                }
                add_accept(&ring, server_fd);  // сразу принимаем следующего
                delete req;
                break;

            case OpType::READ:
                if (res > 0) {
                    add_write(&ring, req, res); // эхо: пишем обратно
                } else {
                    // EOF или ошибка
                    close(req->fd);
                    delete req;
                }
                break;

            case OpType::WRITE:
                if (res > 0) {
                    add_read(&ring, req->fd);   // читаем следующую порцию
                } else {
                    close(req->fd);
                    delete req;
                }
                break;
            }
        }

        io_uring_cq_advance(&ring, count); // освободить count слотов в CQ
        io_uring_submit(&ring);            // отправить новые задания
    }
}