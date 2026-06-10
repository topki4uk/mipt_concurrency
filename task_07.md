## Системный вызов futex. Реализация std::mutex через futex. Идея реализации futex в ядре ОС.

### Идея `futex`: fast path в userspace

`Futex` (Fast Userspace Mutex) — гибрид: большую часть времени работает без `syscall`, и 
уходит в ядро только когда поток действительно нужно усыпить.

```text
lock() без конкуренции:      lock() с конкуренцией:
                             
  CAS в userspace              CAS в userspace
  ↓ успех                      ↓ неудача
  вошли (0 syscall!)           futex_wait() → syscall → сон
```

Ключевая идея: атомарная переменная в userspace является и замком, и сигналом для ядра. 
Ядро смотрит на ту же переменную, что и userspace-код.

### Системный вызов `futex`

```cpp
#include <linux/futex.h>

int futex(int *uaddr,    // адрес атомарной переменной в userspace
          int futex_op,  // операция: WAIT, WAKE, ...
          int val,       // ожидаемое значение (для WAIT)
          ...);
```

Две основные операции:
* `FUTEX_WAIT`
```cpp
futex(uaddr, FUTEX_WAIT, val, NULL);    // усыпить поток
```

Ядро делает следующее:
1. **Атомарно** проверяет: `*uaddr == val`?
2. Если **нет** — сразу возвращается (кто-то уже изменил переменную, не нужно спать)
3. Если **да** — добавляет поток в очередь ожидания и усыпляет его

* `FUTEX_WAKE` 
```cpp
futex(uaddr, FUTEX_WAKE, n, NULL);  // разбудить потоки
// n = сколько потоков разбудить (обычно 1)
```

Ядро берёт n потоков из очереди ожидания по адресу `uaddr` и переводит их в состояние «готов к выполнению».

### Идея реализации `futex` в ядре ОС

Ядро хранит хеш-таблицу очередей ожидания, где ключ — физический адрес атомарной переменной:

```text
Хеш-таблица ядра:

физ. адрес &lock1  →  [поток 3] → [поток 7] → NULL
физ. адрес &lock2  →  [поток 1] → NULL
физ. адрес &lock3  →  NULL
```

`FUTEX_WAIT` внутри ядра

```text
1. Вычислить bucket = hash(физ_адрес(uaddr))
2. Захватить spinlock на bucket
3. Атомарно прочитать *uaddr
4. Если *uaddr != val → отпустить spinlock, вернуть EAGAIN
5. Добавить текущий поток в очередь bucket
6. Отпустить spinlock
7. Вызвать schedule() — переключиться на другой поток
```

`FUTEX_WAKE` внутри ядра

```text
1. Вычислить bucket = hash(физ_адрес(uaddr))
2. Захватить spinlock на bucket
3. Взять n потоков из очереди
4. Отпустить spinlock
5. Перевести потоки в TASK_RUNNABLE (планировщик их запустит)
```

```text
Ядро Linux:

  futex_wait(uaddr, val)          futex_wake(uaddr, 1)
       │                                  │
       ▼                                  ▼
  hash(uaddr) → bucket            hash(uaddr) → bucket
       │                                  │
  spinlock(bucket)                spinlock(bucket)
       │                                  │
  *uaddr == val?                   взять поток из очереди
  ├─ нет → return EAGAIN           отпустить spinlock
  └─ да  → добавить в очередь     wake_up(поток)
           отпустить spinlock
           schedule()  ← сон
```

Важная деталь: внутри ядра для защиты самой очереди используется обычный spinlock — но это spinlock ядра, 
который держится микросекунды, а не весь мьютекс целиком.

### Реализация `std::mutex` через `futex`

```cpp
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "futex.hpp"

class Mutex2State {
public:
    void lock() {
        // Пытаемся сразу захватить mutex:
        // 0 -> свободен, 1 -> занят
        if (state_.exchange(1) == 0)
            return;

        // Если mutex уже занят, ждем пробуждения через futex
        // и после пробуждения снова пытаемся захватить его.
        do {
            futex_wait(&state_, 1);
        } while (state_.exchange(1) != 0);
    }

    void unlock() {
        // Освобождаем mutex
        state_.store(0); // между store и wake может успеть другой поток

        // Будим один ожидающий поток
        futex_wake(&state_, 1); // системный вызов делается всегда
    }

private:
    // 0 - mutex свободен
    // 1 - mutex захвачен
    std::atomic<int> state_{0};
};
```