## Условные переменные: std::condition_variable. Семафоры, класс std::counting_semaphore. Реализация семафора через condition_variable.

### `std::condition_variable` — зачем она нужна

Мьютекс решает проблему взаимного исключения, но не решает задачу ожидания события.

```cpp
while (queue.empty()) { /* крутимся вхолостую */ }
```

Это расходует $100\%$ CPU впустую.

### Интерфейс `std::condition_variable`

`std::condition_variable` работает только в паре с `std::unique_lock<std::mutex>`.

| Метод                   | Что делает                                                                |
|-------------------------|---------------------------------------------------------------------------|
| `wait(lock)`            | Атомарно освобождает мьютекс и блокирует поток                            |
| `wait(lock, predicate)` | То же, но при пробуждении проверяет условие (защита от `spurious wakeup`) |
| `notify_one()`          | 	Будит один из ожидающих потоков                                          |
| `notify_all()`          | 	Будит все ожидающие потоки                                               |

### Проблема spurious wakeup

Поток может проснуться без вызова `notify` — это называется ложным пробуждением (`spurious wakeup`).

### Очередь производитель–потребитель

```cpp
#include <mutex>
#include <condition_variable>
#include <queue>

std::mutex mtx;
std::condition_variable cv;
std::queue<int> data_queue;

// Производитель
void producer() {
    for (int i = 0; i < 10; ++i) {
        {
            std::unique_lock lock(mtx);
            data_queue.push(i);
        }
        cv.notify_one();
    }
}

// Потребитель
void consumer() {
    while (true) {
        std::unique_lock lock(mtx);
        cv.wait(lock, [] { return !data_queue.empty(); });

        int val = data_queue.front();
        data_queue.pop();
        lock.unlock();

        process(val);
    }
}
```

### Семафоры

**Семафор** — примитив синхронизации с целочисленным счётчиком.

* `acquire()` — уменьшает счётчик на 1; если счётчик = 0, поток блокируется
* `release()` — увеличивает счётчик на 1 и будит один заблокированный поток

**Мьютекс** — это бинарный семафор (счётчик 0 или 1) с добавленным понятием владельца.

### `std::counting_semaphore` (C++20)

```cpp
#include <semaphore>

std::counting_semaphore<5> sem(5);

void worker() {
    sem.acquire();
    // ... критическая секция ...
    sem.release();
}
```