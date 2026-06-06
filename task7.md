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
#include <linux/futex.h>
#include <syscall.h>

class Mutex {
public:
    void lock() {
        int expected = 0;

        // Fast path: пытаемся захватить (0 → 1)
        if (state_.compare_exchange_strong(
                expected, 1,
                std::memory_order_acquire)) {
            return; // захватили без syscall ✓
        }

        // Slow path: есть конкуренция
        // Ставим состояние 2 (есть ожидающие)
        // и уходим спать
        while (state_.exchange(2, std::memory_order_acquire) != 0) {
            // Спим ТОЛЬКО если state == 2 (ядро проверит это атомарно)
            syscall(SYS_futex, &state_, FUTEX_WAIT, 2, nullptr);
            // После пробуждения — снова пробуем захватить
        }
    }

    void unlock() {
        // Если state был 2 (есть ожидающие) — нужен WAKE
        if (state_.fetch_sub(1, std::memory_order_release) != 1) {
            // state был 2, значит кто-то ждёт
            state_.store(0, std::memory_order_release);
            syscall(SYS_futex, &state_, FUTEX_WAKE, 1, nullptr);
        }
        // Если state был 1 (никто не ждал) — просто стал 0, syscall не нужен
    }

private:
    std::atomic<int> state_{0};
};
```