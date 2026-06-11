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
Минусы: дорогой scan при retire (O(N·P)), memory fence на каждое чтение
```

2. Epoch-Based Reclamation (EBR)

Глобальный счётчик эпох. Поток при входе в критическую секцию фиксирует текущую эпоху. Узел удаляется только когда все потоки прошли через новую эпоху:

```text
Эпоха 0: поток A удаляет узел → кладёт в retired[0]
Эпоха 1: все потоки обновили local_epoch ≥ 1
Эпоха 2: все потоки обновили local_epoch ≥ 2
         → retired[0] точно никто не видит → можно delete

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
// DATA RACE! ptr_ и ctrl_block_ обновляются не атомарно вместе
```

### `std::atomic<std::shared_ptr<T>>`

C++20 специализация `atomic` для `shared_ptr`. Гарантирует атомарность операций над самим `shared_ptr` — то есть атомарное обновление пары (`ptr_`, `ctrl_block_`):

```cpp
std::atomic<std::shared_ptr<T>> atomic_ptr;

// Потокобезопасно:
auto local = atomic_ptr.load();                    // атомарное чтение
atomic_ptr.store(std::make_shared<T>(42));         // атомарная запись
atomic_ptr.compare_exchange_strong(expected, new_val); // атомарный CAS
```

Что изменилось по сравнению с обычным `shared_ptr`

|                        | `shared_ptr` | `atomic<shared_ptr>` |
|------------------------|--------------|----------------------|
| Инкремент счётчика     | Атомарный    | Атомарный            |
| Чтение ptr+ctrl вместе | Не атомарно  | Атомарно             |
| Запись ptr+ctrl вместе | Не атомарно  | Атомарно             |
| CAS на сам указатель   | Невозможен   | Есть                 |
| Конкурентное чтение    | Безопасно    | Безопасно            |
| Конкурентная запись    | UB           | Безопасно            |

### Идея реализации: Split Reference Counting

Проблема наивной реализации через мьютекс:

```cpp
// Наивно (НЕ lock-free):
struct atomic_shared_ptr {
    std::mutex mtx_;
    std::shared_ptr<T> ptr_;

    std::shared_ptr<T> load() {
        std::lock_guard lock(mtx_);
        return ptr_; // атомарно копируем под мьютексом
    }
};
```

Это работает, но не lock-free. Большинство реализаций (libstdc++, libc++) именно так и делают — через внутренний spinlock или мьютекс.

Split Reference Counting — настоящая идея

```cpp
// Указатель с внешним счётчиком
struct CountedPtr {
    int             external_count; // сколько потоков держат этот ptr
    ControlBlock*   ctrl;           // блок управления shared_ptr
};

std::atomic<CountedPtr> atomic_head;
```

```text
Поток хочет load():
  1. Атомарно читает CountedPtr И инкрементирует external_count
     (одним atomic fetch_add на packed struct)
  2. Теперь объект защищён: его не удалят пока external_count > 0
  3. Читает данные
  4. Декрементирует external_count
     Если external_count + internal_count == 0 → delete

Поток хочет store(new_ptr):
  1. CAS на CountedPtr: заменяет весь пакет (ptr + счётчик) атомарно
  2. У старого ptr: уменьшает internal_count на (external_count - 1)
     (external_count потоков всё ещё держат старый ptr)
```

```cpp
// Упрощённая схема:
template<typename T>
struct AtomicSharedPtr {
    struct alignas(16) CountedPtr {
        int            ext_count = 0;
        ControlBlock*  ctrl      = nullptr;
    };

    std::atomic<CountedPtr> ptr_; // требует 128-bit CAS (CMPXCHG16B на x86)

    std::shared_ptr<T> load() {
        CountedPtr old = ptr_.load(std::memory_order_acquire);

        // Атомарно инкрементируем ext_count
        CountedPtr inc = old;
        do {
            inc = old;
            inc.ext_count++;
        } while (!ptr_.compare_exchange_weak(old, inc,
                     std::memory_order_acquire));

        // Теперь у нас есть "защита" — создаём shared_ptr
        std::shared_ptr<T> result(/* из ctrl */);

        // Возвращаем ext_count обратно
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