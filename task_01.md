## Сокеты. Системные вызовы socket, bind, listen, accept. Реализация синхронного однопоточного TCP-сервера.

**Сокет** — это абстракция операционной системы, представляющая собой один конец двустороннего канала связи между двумя процессами 
(в том числе по сети). TCP-сервер строится на четырёх ключевых системных вызовах: socket, bind, listen, accept.

### Системные вызовы

### `socket()`

```bash
man 2 socket
```

```cpp
#include <sys/socket.h>

int socket(int domain, int type, int protocol);
```

```cpp
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
```

### `bind()`

```bash
man 2 bind
```

```cpp
#include <sys/socket.h>

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

```cpp
struct sockaddr_in addr;
addr.sin_family = AF_INET;
addr.sin_port = htons(8080);
addr.sin_addr.s_addr = INADDR_ANY;

bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
```

`htons()` переводит номер порта из порядка байт хоста в сетевой порядок (big-endian).

### `listen()`

```bash
man 2 listen
```

```cpp
#include <sys/socket.h>

int listen(int sockfd, int backlog);
```

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

```cpp
struct sockaddr_in client_addr;
socklen_t client_len = sizeof(client_addr);

int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
```