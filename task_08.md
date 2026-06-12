## Мьютекс на трех состояниях (мотивировка: уменьшение числа сисколлов, проезд по памяти в деструкторе примитива OneShot). Реализация мьютекса на трех состояниях.

### Проблема 1: лишний `syscall` при `unlock`

С двумя состояниями (0 = свободен, 1 = захвачен) `unlock()` не знает, есть ли спящие потоки. Приходится всегда звать `futex_wake` — даже когда никто не ждёт:

```cpp
void unlock() {
    state_.store(0, release);
    futex_wake(&state_, 1); // лишний syscall
}
```

### Проблема 2: проезд по памяти в деструкторе `OneShot`

`OneShot` — это примитив «одноразовой синхронизации»: 
один поток публикует результат, другие ждут его. Пример использования:

```cpp
OneShot<int> result;

// Поток-производитель
result.set(42);

// Поток-потребитель
int v = result.get();
```

### Попытка со счётчиком ожидающих и почему она ломается

Чтобы не делать лишний `futex_wake`, добавим счётчик `waiting`:

```cpp
void lock() {
    if (state.exchange(1) == 0) return;
    waiting.fetch_add(1);
    while (state.exchange(1) == 1) futex_wait(&state, 1);
    waiting.fetch_sub(1);
}
void unlock() {
    state.store(0);
    if (waiting.load() > 0) futex_wake(&state, 1);
}
```

#### Как выглядит гонка?

* **Поток 1:** делает `store(0)` и проверяет `waiting` (ещё 0) → `futex_wake` не зовёт.
* **Поток 2:** в этот момент инкрементирует `waiting`, видит занятость и уходит в `futex_wait` — и засыпает навсегда (`lost wakeup`), потому что проверка `waiting` случилась раньше инкремента.

**Вывод:** счётчик и состояние нужно объединить в одно атомарное слово и менять согласованно. Это подводит нас к мьютексу на трёх состояниях.

### Три состояния мьютекса

```cpp
// Состояния:
static constexpr int FREE     = 0; // свободен
static constexpr int LOCKED   = 1; // захвачен, нет ожидающих
static constexpr int SLEEPING = 2; // захвачен, есть спящие потоки
```

### Итоговая реализация

```cpp
#include <atomic>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

class Mutex {
    static constexpr int FREE     = 0;
    static constexpr int LOCKED   = 1;
    static constexpr int SLEEPING = 2;

    std::atomic<int> state_{FREE};

    static void futex_wait(std::atomic<int>* addr, int expected) {
        syscall(SYS_futex, addr, FUTEX_WAIT, expected, nullptr);
    }

    static void futex_wake(std::atomic<int>* addr, int count) {
        syscall(SYS_futex, addr, FUTEX_WAKE, count, nullptr);
    }

public:
    void lock() {
        int expected = FREE;
        if (state_.compare_exchange_strong(
                expected, LOCKED,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return;
        }

        while (state_.exchange(SLEEPING,
                               std::memory_order_acquire) != FREE) {
            futex_wait(&state_, SLEEPING);
        }
    }

    void unlock() {
        int prev = state_.fetch_sub(1, std::memory_order_release);

        if (prev == SLEEPING) {
            state_.store(FREE, std::memory_order_release);
            futex_wake(&state_, 1);
        }
    }
};
```