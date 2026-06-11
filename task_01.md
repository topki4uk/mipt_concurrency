## Сокеты. Системные вызовы socket, bind, listen, accept. Реализация синхронного однопоточного TCP-сервера.

**Сокет** — это абстракция операционной системы, представляющая собой один конец двустороннего канала связи между двумя процессами 
(в том числе по сети). TCP-сервер строится на четырёх ключевых системных вызовах: socket, bind, listen, accept.

### Что такое сокет?
**Сокет** — это файловый дескриптор, через который можно читать и писать данные так же, как из обычного файла, 
но данные передаются по сети. TCP-соединение однозначно определяется четвёркой (`IP-адрес источника`, `порт источника`, 
`IP-адрес назначения`, `порт назначения`) — это означает, что на одном порту сервер может держать несколько соединений одновременно с разными клиентами.

### Системные вызовы

### `socket()`

```bash
man 2 socket
```

```cpp
#include <sys/socket.h>

int socket(int domain, int type, int protocol);
```

#### Трактовка параметров

* `domain` - задаёт домен соединения, т.е. выбирает набор протоколов, которые будут использоваться для создания соединения (например, `PF_UNIX/PF_LOCAL` или `PF_INET/PF_INET6`)
* `type` - задает семантику коммуникации  (например, `SOCK_STREAM` или `SOCK_DGRAM`)
* `protocol` - задаёт конкретный протокол, который работает с сокетом (обычно просто `0`, т.е. ядро выбирает протокол)

Создаёт новый сокет и возвращает файловый дескриптор. На этом этапе сокет ещё не привязан ни к какому адресу.

```cpp
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
// AF_INET  — семейство IPv4
// SOCK_STREAM — потоковый (TCP) сокет
```

### `bind()`

```bash
man 2 bind
```

```cpp
#include <sys/socket.h>

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

#### Трактовка параметров

* `sockfd` - используемый сокет

* `my_addr` - локальный адрес

    ```cpp
    #include <sys/socket.h>

    struct sockaddr {
        sa_family_t     sa_family;      /* Address family */
        char            sa_data[];      /* Socket address */
    };
    ```

* `addrlen` - длина `my_addr`

Привязывает сокет к конкретному IP-адресу и порту. Сервер указывает, на каком порту он будет ждать клиентов.

```cpp
struct sockaddr_in addr;
addr.sin_family = AF_INET;
addr.sin_port = htons(8080);       // порт 8080
addr.sin_addr.s_addr = INADDR_ANY; // принимаем с любого интерфейса

bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
```

`htons()` переводит номер порта из порядка байт хоста в сетевой порядок (big-endian).

### `listen()`

```bash
man 2 listen
```

Отмечу, что этот метод применим только к сокетам типа `SOCK_STREAM` или `SOCK_SEQPACKET`. Потому что оба типа относятся к устойчивым протоколам.

```cpp
#include <sys/socket.h>

int listen(int sockfd, int backlog);
```

Переводит сокет в режим ожидания входящих соединений. Второй параметр — размер очереди ожидающих подключений (`backlog`).

```cpp
listen(server_fd, /*backlog=*/ 10);
```

### `accept()`

```bash
man 2 accept
```

```cpp
#include <sys/socket.h>

int accept(int sockfd, struct sockaddr *_Nullable restrict addr,
            socklen_t *_Nullable restrict addrlen);
```

#### Трактовка параметров

* `sockfd` - fd сокета сервера
* `addr` -  указатель на структуру `sockaddr`
* `addrlen` - передаётся по ссылке; перед вызовом он содержит размер структуры, а после вызова - действительную длину адреса в байтах

Блокируется и ждёт, пока не подключится клиент. Когда клиент приходит, возвращает новый файловый дескриптор для 
общения именно с этим клиентом. Исходный `server_fd` продолжает слушать.

```cpp
struct sockaddr_in client_addr;
socklen_t client_len = sizeof(client_addr);

int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
// client_fd — дескриптор соединения с конкретным клиентом
```

