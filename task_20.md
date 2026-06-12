## Проблема управления памятью в lock-free структурах. Обзор методов решения проблемы. Класс std::atomic_shared_ptr. В чем отличие от обычного shared_ptr, в чем идея реализации? Можно ли сделать lock-free atomic_shared_ptr?

### Проблема управления памятью в lock-free структурах

В lock-free структурах есть фундаментальная проблема: нельзя безопасно удалить узел, потому что в момент удаления другой поток может уже держать указатель на него, но ещё не опубликовал его как hazard pointer:

```text
Поток A (dequeue):           Поток B (dequeue):

head = load(head_)           
                             head = load(head_)  // тот же адрес!
CAS(head_, head, next)       // успех — A "владеет" узлом
delete head                  // удаляем!
                             next = head->next   // UAF! head уже удалён
```

Проблема называется use-after-free (UAF) — поток B читает уже освобождённую память. С мьютексом такого нет: удаление происходит под блокировкой. В lock-free коде нет момента, когда гарантированно никто не держит указатель.

### Обзор методов решения

1. Hazard Pointers

Каждый поток перед чтением указателя публикует его в глобальном массиве. Удаление откладывается до тех пор, пока ни один поток не держит hazard на этот адрес:

```text
Плюсы:  O(1) память на поток, детерминированное удаление
Минусы: дорогой scan при retire (O(N·P))
```

2. Epoch-Based Reclamation (EBR)

Глобальный счётчик эпох. Поток при входе в критическую секцию фиксирует текущую эпоху. Узел удаляется только когда все потоки прошли через новую эпоху:

```text
Плюсы:  быстро, мало overhead'а на каждую операцию
Минусы: узлы живут дольше, поток может заблокировать продвижение эпохи
```

3. Reference Counting (подсчёт ссылок)

Хранить счётчик прямо в узле. Перед чтением — инкрементировать, после — декрементировать. Удаление когда счётчик = 0:

```text
Проблема: инкремент и чтение указателя не атомарны вместе
          → нужен двойной CAS или split reference counting
```

4. RCU (Read-Copy-Update)

Читатели работают без блокировок совсем. Писатель создаёт копию структуры, модифицирует её, атомарно переключает указатель. Старая копия удаляется когда все читатели вышли из критической секции:

```text
Плюсы:  читатели абсолютно свободны, нет overhead'а
Минусы: писатель дорог (копирование), не подходит для частых записей
```

5. Split Reference Counting

Техника используемая в `atomic<shared_ptr>`: разделить счётчик на внешний (локальный, без атомарности) и внутренний (атомарный, в объекте):

```text
external_count: сколько потоков держат указатель на этот узел
internal_count: сколько раз узел был «взят» через CAS

Удаление: когда external + internal = 0
```

### `std::shared_ptr` — напоминание

`shared_ptr` состоит из двух частей:

```text
shared_ptr<T>:
  ┌──────────────┐
  │  ptr_        │──► управляемый объект T
  │  ctrl_block_ │──► ┌─────────────────────┐
  └──────────────┘    │ strong_count (атом.)│
                      │ weak_count   (атом.)│
                      │ deleter             │
                      └─────────────────────┘
```

Копирование `shared_ptr` — атомарный инкремент `strong_count`. Но сам `shared_ptr` не является потокобезопасным для конкурентной записи: 
если два потока одновременно присваивают один и тот же `shared_ptr` — это data race на `ptr_` и `ctrl_block_`.

```cpp
shared_ptr<T> global_ptr;

// Поток A:              // Поток B:
global_ptr = new_ptr_a;  global_ptr = new_ptr_b;
```

### `std::atomic<std::shared_ptr<T>>`

C++20 специализация `atomic` для `shared_ptr`. Гарантирует атомарность операций над самим `shared_ptr` — то есть атомарное обновление пары (`ptr_`, `ctrl_block_`):

```cpp
std::atomic<std::shared_ptr<T>> atomic_ptr;

auto local = atomic_ptr.load();
atomic_ptr.store(std::make_shared<T>(42));
atomic_ptr.compare_exchange_strong(expected, new_val);
```

### Идея реализации: Split Reference Counting

Проблема наивной реализации через мьютекс:

```cpp
// Наивно (НЕ lock-free):
struct atomic_shared_ptr {
    std::mutex mtx_;
    std::shared_ptr<T> ptr_;

    std::shared_ptr<T> load() {
        std::lock_guard lock(mtx_);
        return ptr_;
    }
};
```

Split Reference Counting — настоящая идея

```cpp
struct CountedPtr {
    int             external_count;
    ControlBlock*   ctrl;  // блок управления shared_ptr
};

std::atomic<CountedPtr> atomic_head;
```

```cpp
// Упрощённая схема:
template<typename T>
struct AtomicSharedPtr {
    struct alignas(16) CountedPtr {
        int            ext_count = 0;
        ControlBlock*  ctrl      = nullptr;
    };

    std::atomic<CountedPtr> ptr_;

    std::shared_ptr<T> load() {
        CountedPtr old = ptr_.load(std::memory_order_acquire);

        CountedPtr inc = old;
        do {
            inc = old;
            inc.ext_count++;
        } while (!ptr_.compare_exchange_weak(old, inc,
                     std::memory_order_acquire));

        std::shared_ptr<T> result(/* из ctrl */);

        old.ctrl->release_external(inc.ext_count);
        return result;
    }
};
```

### Можно ли сделать lock-free `atomic<shared_ptr>`?

**Теоретически — да, практически — очень сложно.**

Проблема: нужен 128-bit атомарный CAS

```text
x86-64: CMPXCHG16B есть (с 2007), но:
  - не на всех CPU (нужна проверка CPUID)
  - медленнее обычного CAS
  - не поддерживается в виде std::atomic напрямую

ARM64: LSE имеет CASP (128-bit CAS), но не везде
```

Что делают реальные реализации

```text
libstdc++ (GCC):    внутренний spinlock на хэше адреса
                    → НЕ lock-free

libc++ (Clang):     аналогично — spinlock
                    → НЕ lock-free

folly (Facebook):   PackedSyncPtr + hazard pointers
                    → lock-free на поддерживаемых платформах

Стандарт C++20:     не гарантирует lock-free!
                    is_lock_free() может вернуть false
```

#### Почему это фундаментально сложно

Даже с CMPXCHG16B есть проблема: между чтением указателя и инкрементом счётчика — не атомарно