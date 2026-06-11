## Мьютекс на трех состояниях (мотивировка: уменьшение числа сисколлов, проезд по памяти в деструкторе примитива OneShot). Реализация мьютекса на трех состояниях.

### Проблема 1: лишний `syscall` при `unlock`

С двумя состояниями (0 = свободен, 1 = захвачен) `unlock()` не знает, есть ли спящие потоки. Приходится всегда звать `futex_wake` — даже когда никто не ждёт:

```cpp
// Два состояния — unlock всегда делает syscall
void unlock() {
    state_.store(0, release);
    futex_wake(&state_, 1); // ← лишний syscall, даже если никто не спит
}
```

### Проблема 2: проезд по памяти в деструкторе `OneShot`

`OneShot` — это примитив «одноразовой синхронизации»: 
один поток публикует результат, другие ждут его. Пример использования:

```cpp
OneShot<int> result;

// Поток-производитель
result.set(42);  // сигнализирует всем ждущим

// Поток-потребитель
int v = result.get();  // блокируется до set()
```

Критическая ситуация: что происходит после `set()`? Потребитель проснулся, получил значение, и может немедленно уничтожить объект 
`result` (например, он на стеке). Производитель при этом ещё выполняет код внутри `set()` — и обращается к памяти уже уничтоженного объекта:

```text
Поток 1 (producer):          Поток 2 (consumer):

result.set(42):
  state_.store(DONE)   →→→   result.get() возвращает 42
  // потребитель             result уничтожен (деструктор)!
  futex_wake(...)      →→→   ~OneShot() вызван
  // обращение к state_  ← ПРОЕЗД ПО ПАМЯТИ 💥
  // после деструктора!
```

С двумя состояниями нет способа узнать: есть ли ещё кто-то внутри `set()`, прежде чем разрушать объект. Третье состояние решает оба этих вопроса.

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

Инвариант: `SLEEPING` означает — «когда `unlock()` освободит мьютекс, обязательно нужно позвать `futex_wake`». 
Если состояние `LOCKED` — никто не спит, `futex_wake` не нужен.

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
        // Fast path: FREE → LOCKED (без syscall)
        int expected = FREE;
        if (state_.compare_exchange_strong(
                expected, LOCKED,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return; // ✓ захватили без конкуренции
        }

        // Slow path: есть конкуренция
        // Переводим в SLEEPING и уходим спать
        // (даже если до нас уже SLEEPING — это нормально)
        while (state_.exchange(SLEEPING,
                               std::memory_order_acquire) != FREE) {
            // Спим только если состояние действительно SLEEPING
            // (ядро атомарно проверит это)
            futex_wait(&state_, SLEEPING);
        }
        // Вышли из цикла: state был FREE, мы его забрали (exchange вернул FREE)
    }

    void unlock() {
        // Атомарно уменьшаем: LOCKED(1)→FREE(0) или SLEEPING(2)→?(1)
        int prev = state_.fetch_sub(1, std::memory_order_release);

        if (prev == SLEEPING) {
            // Были спящие потоки — нужно разбудить одного
            // Сначала явно ставим FREE (fetch_sub поставил 1, а не 0)
            state_.store(FREE, std::memory_order_release);
            futex_wake(&state_, 1); // syscall только здесь
        }
        // Если prev == LOCKED — просто стали FREE, никто не ждёт → 0 syscall
    }
};
```

### Разбор работы 

#### `lock()`

```text
Нет конкуренции:
  CAS(FREE→LOCKED) ✓ → return
  [0 syscalls, 1 атомарная операция]

Конкуренция, но мьютекс освобождается быстро:
  exchange(SLEEPING) → вернул LOCKED или FREE?
  ├─ FREE → вышли из while, захватили (не пошли спать)
  └─ LOCKED/SLEEPING → идём в futex_wait

В futex_wait:
  Ядро проверяет: state == SLEEPING?
  ├─ нет (кто-то уже сделал unlock) → сразу вернуться (EAGAIN)
  └─ да → усыпить поток
```

#### `unlock()`

```text
state == LOCKED (1):            state == SLEEPING (2):

fetch_sub(1) → state = 0        fetch_sub(1) → state = 1
prev = 1 = LOCKED               prev = 2 = SLEEPING

prev != SLEEPING →              prev == SLEEPING →
  ничего не делаем                store(FREE) → state = 0
  [0 syscalls] ✓                  futex_wake(1)
                                  [1 syscall — только при конкуренции]
```