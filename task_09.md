## Реализация condition_variable через futex. Счетчик поколений. Проблема spurious wakeups.

### Что такое `condition_variable`?

`condition_variable` позволяет потоку заснуть, пока не выполнится некоторое условие, и проснуться когда другой поток это условие изменит. Классический интерфейс:

```cpp
std::mutex mtx;
std::condition_variable cv;
bool ready = false;

// Потребитель
std::unique_lock<std::mutex> lock(mtx);
cv.wait(lock, []{ return ready; });

// Производитель
{
    std::lock_guard<std::mutex> lock(mtx);
    ready = true;
}
cv.notify_one();
```

### Наивная реализация и её проблема

Первая идея — просто хранить атомарный счётчик уведомлений и делать `futex_wait`:

```cpp
// waiter
while (!ready) {
    mtx.unlock();
    futex_wait(&flag, 0);
    mtx.lock();
}

// notifier
ready = true;
futex_wake(&flag, 1);
```

### Счётчик поколений

`seq_` — это номер поколения, то есть версия состояния ожидания: каждый `notify_one/notify_all` увеличивает его на 1. Поток в `wait()` читает `old = seq_` под mutex и потом говорит ядру: “усыпи меня только если `seq_` всё ещё равно old”.

### Итог

```cpp
#include <atomic>
#include <mutex>
#include <linux/futex.h>

class CondVar {
public:
    void wait(std::unique_lock<std::mutex>& lock) {
        int expected = gen_counter_.load();
        lock.unlock();

        syscall(SYS_futex, &gen_counter_, FUTEX_WAIT, expected, nullptr, nullptr, 0);

        lock.lock();
    }

    void notify_one() {
        gen_counter_.fetch_add(1);
        syscall(SYS_futex, &gen_counter_, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    void notify_all() {
        gen_counter_.fetch_add(1);
        syscall(SYS_futex, &gen_counter_, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    }

private:
    std::atomic<int> gen_counter_{0};
};
```

### Проблема `spurious wakeups`

`Spurious wakeup` (ложное пробуждение) — поток просыпается из `futex_wait` без вызова 
`notify_one/notify_all`.

```cpp
// НЕПРАВИЛЬНО
cv.wait(lock);
use(data);

// ПРАВИЛЬНО
while (!ready) {
    cv.wait(lock);
}
use(data);
```

### Как wait(pred) защищает от `spurious wakeup`?

Перегрузка с предикатом — это просто синтаксический сахар над циклом:
```cpp
// cv.wait(lock, pred) эквивалентно:
while (!pred()) {
    cv.wait(lock);
}
```