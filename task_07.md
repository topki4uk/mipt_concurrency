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

### Системный вызов `futex`

```cpp
#include <linux/futex.h>

int futex(int *uaddr,
          int futex_op,
          int val,
          ...);
```

Две основные операции:
* `FUTEX_WAIT`
    ```cpp
    futex(uaddr, FUTEX_WAIT, val, NULL);  // усыпить поток
    ```

* `FUTEX_WAKE` 
    ```cpp
    futex(uaddr, FUTEX_WAKE, n, NULL);  // разбудить потоки
    ```

### Идея реализации `futex` в ядре ОС

Ядро хранит хеш-таблицу очередей ожидания, где ключ — физический адрес атомарной переменной:

```text
Хеш-таблица ядра:

физ. адрес &lock1  →  [поток 3] → [поток 7] → NULL
физ. адрес &lock2  →  [поток 1] → NULL
физ. адрес &lock3  →  NULL
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

**Важная деталь:** внутри ядра для защиты самой очереди используется обычный spinlock — но это spinlock ядра, 
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
        if (state_.exchange(1) == 0)
            return;

        do {
            futex_wait(&state_, 1);
        } while (state_.exchange(1) != 0);
    }

    void unlock() {
        state_.store(0);
        futex_wake(&state_, 1);
    }

private:
    std::atomic<int> state_{0};
};
```