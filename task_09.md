## Реализация condition_variable через futex. Счетчик поколений. Проблема spurious wakeups.

### Что такое `condition_variable`?

`condition_variable` позволяет потоку заснуть, пока не выполнится некоторое условие, и проснуться когда другой поток это условие изменит. Классический интерфейс:

```cpp
std::mutex mtx;
std::condition_variable cv;
bool ready = false;

// Потребитель
std::unique_lock<std::mutex> lock(mtx);
cv.wait(lock, []{ return ready; }); // спать, пока ready == false

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

#### Проблема

1. Поток A держит mutex и видит `ready == false`.

2. Поток A делает `unlock()` и собирается вызвать `futex_wait(...)`.

3. До `futex_wait` поток B захватывает `mutex`, делает `ready = true`, вызывает `wake()` и уходит.

4. Поток A только теперь вызывает `futex_wait(...)` и засыпает, хотя нужное событие уже произошло.

Именно эту гонку решает счётчик поколений.

### Счётчик поколений

Идея: `wait()` запоминает значение счётчика под мьютексом (до его освобождения), и спит только если счётчик не изменился.

`seq_` — это номер поколения, то есть версия состояния ожидания: каждый `notify_one/notify_all` увеличивает его на 1. Поток в `wait()` читает `old = seq_` под mutex и потом говорит ядру: “усыпи меня только если `seq_` всё ещё равно old”.

### Итог

```cpp
#include <atomic>
#include <mutex>
#include <linux/futex.h>

class CondVar {
public:
    void wait(std::unique_lock<std::mutex>& lock) {
        // запоминаем текущее поколение
        int expected = gen_counter_.load();
        // освобождаем мьютекс
        lock.unlock();
        // если поколение не изменилось (то есть не было notify),
        // то засыпаем
        syscall(SYS_futex, &gen_counter_, FUTEX_WAIT, expected, nullptr, nullptr, 0);
        // проснулись и снова захватили мьютекс
        lock.lock();
    }

    void notify_one() {
        // увеличиваем поколение
        gen_counter_.fetch_add(1);
        // будим один ожидающий поток
        syscall(SYS_futex, &gen_counter_, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    void notify_all() {
        // увеличиваем поколение
        gen_counter_.fetch_add(1);
        // будим все ожидающие потоки
        syscall(SYS_futex, &gen_counter_, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    }

private:
    // счетчик поколений
    std::atomic<int> gen_counter_{0};
};
```

### Проблема `spurious wakeups`

`Spurious wakeup` (ложное пробуждение) — поток просыпается из `futex_wait` без вызова 
`notify_one/notify_all`. Это не баг реализации, а гарантированная возможность на уровне ОС и POSIX.

Откуда берутся
* **Сигналы (signals):** `SIGINT`, `SIGALRM` и другие сигналы прерывают `futex_wait`. Ядро возвращает `EINTR` — поток просыпается, хотя никто не звал wake.
* **Linux-специфика:** планировщик может в редких случаях разбудить поток "на всякий случай" при миграции между ядрами.
* `compare_exchange_weak` по аналогии — архитектурный механизм LL/SC на ARM допускает ложные пробуждения аналогично.

```cpp
// НЕПРАВИЛЬНО — spurious wakeup сломает логику:
cv.wait(lock); // проснулся без notify → идём дальше, хотя ready == false!
use(data);     // data не готова 💥

// ПРАВИЛЬНО — всегда проверяем условие в цикле:
while (!ready) {
    cv.wait(lock);
}
use(data); // гарантированно готова ✓
```

### Как wait(pred) защищает от `spurious wakeup`?

Перегрузка с предикатом — это просто синтаксический сахар над циклом:
```cpp
// cv.wait(lock, pred) эквивалентно:
while (!pred()) {
    cv.wait(lock);
}
```

Поэтому даже при spurious wakeup поток проверит условие и ляжет спать снова, если оно ложно:

```text
Spurious wakeup:

futex_wait → EINTR (сигнал)
mtx.lock()
while (!pred())  ← pred() == false, условие не выполнено
  → снова mtx.unlock() + futex_wait
  → продолжаем ждать ✓
```