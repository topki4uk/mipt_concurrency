## Системные вызовы select, poll и epoll. Разница между ними. Реализация многопоточного сервера через epoll.

Вместо того чтобы опрашивать каждый fd в цикле, мы говорим ядру: «разбуди меня, 
когда хоть на одном из этих fd появятся данные». Поток спит — CPU свободен. 
Ядро отслеживает события внутри себя и будит поток только при реальной активности.

### `select`

Самый старый (POSIX), появился в BSD 4.2 (1983).

```cpp
int select(int nfds,
           fd_set *readfds,
           fd_set *writefds,
           fd_set *exceptfds,
           struct timeval *timeout);
```

```cpp
fd_set read_fds;
FD_ZERO(&read_fds);
FD_SET(fd1, &read_fds);
FD_SET(fd2, &read_fds);

int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, nullptr);

if (FD_ISSET(fd1, &read_fds)) { /* читаем */ }
if (FD_ISSET(fd2, &read_fds)) { /* читаем */ }
```

### `poll`

Появился как замена `select` (POSIX.1-2001), снимает ограничение 1024.

```cpp
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

struct pollfd {
    int   fd;
    short events;
    short revents;
};
```

#### Возможные биты для `events` в `pollfd`

![alt text](images/poll_bytes.png)

```cpp
std::vector<pollfd> fds = {
    {fd1, POLLIN, 0},
    {fd2, POLLIN | POLLOUT, 0},
};

int ready = poll(fds.data(), fds.size(), -1);

for (auto& pfd : fds) {
    if (pfd.revents & POLLIN)  { /* читаем */ }
    if (pfd.revents & POLLOUT) { /* пишем  */ }
}
```

### `epoll`

Linux-специфичный (Linux 2.5.44, 2002). Решает фундаментальную проблему: $\mathcal{O}(1)$ вместо $\mathcal{O}(N)$.

Ключевая идея: множество отслеживаемых fd хранится в ядре между вызовами. Не нужно каждый раз копировать список. Ядро само поддерживает список готовых fd и отдаёт только их.

#### Примеры в коде

```cpp
int epfd = epoll_create1(0);

int epoll_ctl(int epfd,
              int op,
              int fd,
              struct epoll_event *event);

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
};

// 3. Ждать событий
int epoll_wait(int epfd,
               struct epoll_event *events,
               int maxevents,
               int timeout);
```

```cpp
int epfd = epoll_create1(0);

epoll_event ev;
ev.events = EPOLLIN;
ev.data.fd = client_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

epoll_event ready_events[64];
int n = epoll_wait(epfd, ready_events, 64, -1);

for (int i = 0; i < n; i++) {
    int fd = ready_events[i].data.fd;
    read(fd, buf, sizeof(buf));
}
```

#### LT vs ET режимы

```text
Level-Triggered (LT) — по умолчанию:
  epoll_wait уведомляет, пока данные не прочитаны до конца.
  Проще в реализации, похож на poll.

Edge-Triggered (ET) — флаг EPOLLET:
  Уведомление приходит ОДИН РАЗ при появлении новых данных.
  Нужно читать в цикле до EAGAIN — иначе событие потеряно.
  Эффективнее, но требует аккуратной реализации.
```

### Сравнение `select` / `poll` / `epoll`

|                       | `select`         | `poll`           | `epoll`                |
|-----------------------|------------------|------------------|------------------------|
| Лимит fd              | 1024             | Нет              | Нет                    |
| Копирование fd в ядро | Каждый вызов     | Каждый вызов     | Один раз (`epoll_ctl`) |
| Сложность на N fd     | $\mathcal{O}(N)$ | $\mathcal{O}(N)$ | $\mathcal{O}(1)$       |
| Результат             | Битовая маска    | Весь массив      | Только готовые         |
| Портируемость         | POSIX            | POSIX            | Linux only             |
| Поддержка ET          | Нет              | Нет              | Да                     |

### Внутреннее устройство `epoll` в ядре

```text
epoll instance:

  ┌─────────────────────────────────┐
  │  Red-Black Tree (rb-tree)       │  ← все зарегистрированные fd
  │  epoll_ctl: ADD/MOD/DEL → O(log N)  (вставка/удаление)
  └─────────────────────────────────┘

  ┌─────────────────────────────────┐
  │  Ready List (двусвязный список) │  ← только готовые fd
  │  epoll_wait → читает отсюда     │  O(готовых), не O(всех)
  └─────────────────────────────────┘
```