## Модель памяти C++. Понятия sequenced-before, happens-before, synchronizes-with. Разновидности memory order. Барьеры памяти, инструкция mfence.

### Зачем нужна модель памяти

Без модели памяти компилятор и процессор могут переупорядочивать операции для оптимизации. То, что выглядит последовательным в коде, может выполняться в другом порядке:

```cpp
int x = 0, y = 0;

// Поток A:        // Поток B:
x = 1;            y = 1;
print(y);         print(x);

// Возможный результат: оба напечатают 0!
// Процессор переупорядочил store(x=1) и load(y)
```

Модель памяти C++ определяет правила: когда и какие переупорядочивания допустимы, и как потоки видят записи друг друга.

### Sequenced-Before

**Sequenced-before** — отношение между операциями **внутри одного потока**. Если A sequenced-before B, то A выполняется до B и B видит результат A:

```cpp
// В одном потоке:
int x = 5;        // A
int y = x + 1;    // B

// A sequenced-before B: B гарантированно видит x = 5
```

Это самое простое отношение — обычный порядок выполнения строк кода. Определяет порядок **в одном потоке**, ничего не говорит о разных потоках.

### Synchronizes-With

**Synchronizes-with** — отношение между операциями **разных потоков**. Возникает когда:

* `atomic store` с `release` synchronizes-with `atomic load` с `acquire` того же атома, который прочитал записанное значение

```cpp
std::atomic<bool> ready{false};
int data = 0;

// Поток A (producer):
data = 42;                                    // (1)
ready.store(true, std::memory_order_release); // (2)
// store(release) будет synchronizes-with load(acquire)

// Поток B (consumer):
while (!ready.load(std::memory_order_acquire)); // (3)
// (3) synchronizes-with (2) — когда load вернул true
std::cout << data; // (4) гарантированно видит 42
```

```text
(1) sequenced-before (2)
(2) synchronizes-with (3)
(3) sequenced-before (4)
→ (1) happens-before (4): data=42 видна в (4)
```

### Happens-Before

**Happens-before** — транзитивное замыкание sequenced-before и synchronizes-with. Если A happens-before B, то **все записи A видны в B**:

```text
Happens-before = sequenced-before ∪ synchronizes-with (транзитивно)

Если: A sequenced-before B, B synchronizes-with C, C sequenced-before D
То:   A happens-before D  ✓
```

Это ключевое понятие: только через happens-before гарантируется видимость данных между потоками. Если happens-before нет — это data race (UB).

```text
Отношения:

  Один поток        Два потока
  ┌──────────┐      ┌─────────────────────────┐
  │ A        │      │ store(release) ─────────┼──► load(acquire)
  │ ↓ seq-b  │      │       synchronizes-with │
  │ B        │      └─────────────────────────┘
  └──────────┘
       ↓
  happens-before (объединение)
```

### Разновидности Memory Order

* `memory_order_relaxed`

Только атомарность операции. Никаких гарантий порядка относительно других операций:

```cpp
// Подходит для счётчиков где важен только итог, не порядок:
counter.fetch_add(1, std::memory_order_relaxed);

// Порядок между операциями НЕ гарантирован:
x.store(1, std::memory_order_relaxed);
y.store(1, std::memory_order_relaxed);
// Другой поток может увидеть y=1 раньше x=1
```

* `memory_order_acquire`

Применяется к load. Никакое чтение/запись после этого load не может быть перемещено до него:

```cpp
auto val = ptr.load(std::memory_order_acquire);
// Всё что после этой строки — гарантированно после load
// "Захватываем" все записи которые произошли до соответствующего release
```

* `memory_order_release`

Применяется к store. Никакое чтение/запись до этого store не может быть перемещено после него:

```cpp
data = 42;  // гарантированно до store
ptr.store(new_val, std::memory_order_release);
// "Публикуем" все записи выше для потока который сделает acquire
```

#### Acquire-Release пара — главный паттерн

```text
Producer:                    Consumer:
────────────────────────     ────────────────────────
data = 42          ──┐       while(!flag.load(acq));  ─┐
flag.store(1, rel) ──┘    ┌──                          │
                          │  assert(data == 42) ←──────┘
                    release "сбрасывает" барьер
                    acquire "поднимает" барьер
```

* `memory_order_acq_rel`

Для операций read-modify-write (CAS, fetch_add, exchange). Одновременно acquire для чтения и release для записи:

```cpp
// exchange, compare_exchange, fetch_add при конкуренции:
int old = state.exchange(NEW, std::memory_order_acq_rel);
// Видит все записи до предыдущего release
// Публикует свою запись для следующего acquire
```

* `memory_order_seq_cst`

**Последовательная согласованность** — самый сильный порядок. Все операции с seq_cst образуют **единый глобальный порядок**, видимый всем потокам одинаково:

```cpp
// Значение по умолчанию для всех атомарных операций
x.store(1);  // == x.store(1, memory_order_seq_cst)

// Глобальный порядок: все потоки видят операции в одном порядке
// Самый медленный: требует mfence на x86
```

### Итого

| memory_order | Применение         | Запрещает переупорядочивание             |
| ------------ | ------------------ | ---------------------------------------- |
| relaxed      | load / store       | Ничего                                   |
| acquire      | load               | Load↑ (после load ничего не уходит до)   |
| release      | store              | ↓Store (до store ничего не уходит после) |
| acq_rel      | RMW                | Оба направления                          |
| seq_cst      | load / store / RMW | Всё + глобальный порядок                 |

### Барьеры памяти

**Барьер памяти (memory fence)** — явная инструкция, запрещающая процессору переупорядочивать операции через неё. 
В C++ — `std::atomic_thread_fence`:

```cpp
// Эквивалент release без привязки к конкретному атому:
std::atomic_thread_fence(std::memory_order_release);

// Эквивалент acquire:
std::atomic_thread_fence(std::memory_order_acquire);

// Использование:
data = 42;
std::atomic_thread_fence(std::memory_order_release); // барьер
flag.store(true, std::memory_order_relaxed);          // достаточно relaxed
                                                       // барьер уже стоит выше
```

Fence сильнее чем просто `store(release)`: он применяется ко всем предшествующим операциям, а не только к одному атому.

### Инструкция `mfence` (x86)

На уровне железа x86 имеет три вида барьеров:

```text
LFENCE — Load Fence:  все loads до LFENCE завершены до loads после
SFENCE — Store Fence: все stores до SFENCE видны до stores после
MFENCE — Full Fence:  полный барьер — и loads и stores
```

```text
LFENCE — Load Fence:  все loads до LFENCE завершены до loads после
SFENCE — Store Fence: все stores до SFENCE видны до stores после
MFENCE — Full Fence:  полный барьер — и loads и stores
```

Что генерирует компилятор

```cpp
// seq_cst store → компилятор генерирует mfence:
x.store(1, std::memory_order_seq_cst);
// → MOV [x], 1
// → MFENCE

// release store → просто store (x86 делает это бесплатно!):
x.store(1, std::memory_order_release);
// → MOV [x], 1  (x86 store уже имеет release семантику)

// acquire load → просто load (x86 делает это бесплатно!):
auto v = x.load(std::memory_order_acquire);
// → MOV eax, [x]  (x86 load уже имеет acquire семантику)
```

Важный факт: на x86 release/acquire бесплатны — архитектура x86 имеет модель TSO (Total Store Order), где store+load уже упорядочены нужным образом. `mfence` нужен только для `seq_cst`.

На ARM всё иначе — там нужны явные барьеры `DMB` даже для acquire/release:

```text
ARM acquire load:   LDAR  (Load-Acquire)
ARM release store:  STLR  (Store-Release)
ARM full fence:     DMB ISH
```
