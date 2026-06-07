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
// Потребитель:
int old = seq_.load();   // запомнили текущее значение
mtx.unlock();            // отпустили мьютекс
futex_wait(&seq_, old);  // спим, пока seq_ == old
mtx.lock();              // захватили снова

// Производитель:
seq_.fetch_add(1);       // изменили seq_
futex_wake(&seq_, all);  // будим всех
```

Здесь возникает критическое состояние гонки:

```text
Потребитель:                    Производитель:

int old = seq_.load(); // = 0
mtx.unlock();
                                ready = true;
                                seq_.fetch_add(1); // seq_ = 1
                                futex_wake(...)    // будит... никого
// Потребитель ещё не спит!

futex_wait(&seq_, 0);
// seq_ уже != 0, ядро вернёт EAGAIN
// Потребитель никогда не получит уведомление 💥
```

Именно эту гонку решает счётчик поколений.

### Счётчик поколений

Идея: `wait()` запоминает значение счётчика под мьютексом (до его освобождения), и спит только если счётчик не изменился.
Производитель увеличивает счётчик тоже под мьютексом.

```text
Ключевой инвариант:
  - seq_ читается потребителем ПОД мьютексом
  - seq_ пишется производителем ПОД мьютексом
  - значит между «прочитал» и «лёг спать» notify прийти не может
    (производитель ждёт мьютекс)
```

```text
Потребитель:                    Производитель:

mtx.lock()
int old = seq_.load()           // хочет сделать notify,
mtx.unlock()                    // но ждёт мьютекс
futex_wait(&seq_, old)
                                // получил мьютекс
                                ready = true;
                                seq_.fetch_add(1);
                                mtx.unlock();
                                futex_wake(&seq_, all);
// потребитель просыпается ✓
```

### Итог

```cpp
#include <atomic>
#include <linux/futex.h>
#include <sys/syscall.h>

class ConditionVariable {
    // Счётчик поколений: каждый notify_all/notify_one его увеличивает
    std::atomic<int> seq_{0};

    static void futex_wait(std::atomic<int>* addr, int expected) {
        syscall(SYS_futex, addr, FUTEX_WAIT, expected, nullptr);
    }
    static void futex_wake_all(std::atomic<int>* addr) {
        syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, nullptr);
    }
    static void futex_wake_one(std::atomic<int>* addr) {
        syscall(SYS_futex, addr, FUTEX_WAKE, 1, nullptr);
    }

public:
    // lock — уже захваченный мьютекс (передаётся владельцем)
    template<typename Mutex, typename Predicate>
    void wait(Mutex& mtx, Predicate pred) {
        while (!pred()) {
            // 1. Читаем seq_ ПОД мьютексом
            int old = seq_.load(std::memory_order_relaxed);

            // 2. Отпускаем мьютекс и ложимся спать атомарно
            //    (производитель не сможет notify_one до шага 3)
            mtx.unlock();

            // 3. Спим ТОЛЬКО если seq_ не изменился
            //    (если notify пришёл между unlock и wait — seq_ уже другой,
            //     ядро вернёт EAGAIN, мы не заснем)
            futex_wait(&seq_, old);

            // 4. Проснулись — захватываем мьютекс обратно
            mtx.lock();

            // 5. Перепроверяем условие (цикл while)
        }
    }

    // Простой wait без предиката (опасен — см. spurious wakeups)
    template<typename Mutex>
    void wait(Mutex& mtx) {
        int old = seq_.load(std::memory_order_relaxed);
        mtx.unlock();
        futex_wait(&seq_, old);
        mtx.lock();
    }

    void notify_one() {
        // Увеличиваем счётчик — все текущие wait() увидят изменение
        seq_.fetch_add(1, std::memory_order_relaxed);
        futex_wake_one(&seq_);
    }

    void notify_all() {
        seq_.fetch_add(1, std::memory_order_relaxed);
        futex_wake_all(&seq_);
    }
};
```

### Объяснение

#### Почему счётчик защищает от гонки?

```text
БЕЗ счётчика поколений:       С счётчиком поколений:

wait() запоминает...           wait() запоминает seq_=5
  ...ничего конкретного          (под мьютексом)
unlock()                       unlock()
                               notify приходит:
                                 seq_ становится 6
futex_wait(addr, ???)          futex_wait(&seq_, 5)
  addr изменился → EAGAIN        seq_(6) != 5 → EAGAIN
  ← мы пропустили notify!        ← мы НЕ спим, идём
                                   перепроверять условие ✓
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