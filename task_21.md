## Модель памяти C++. Понятия sequenced-before, happens-before, synchronizes-with. Разновидности memory order. Барьеры памяти, инструкция mfence.

### Зачем нужна модель памяти

Без модели памяти компилятор и процессор могут переупорядочивать операции для оптимизации. То, что выглядит последовательным в коде, может выполняться в другом порядке:

```cpp
int x = 0, y = 0;

// Поток A:        // Поток B:
x = 1;            y = 1;
print(y);         print(x);
```

Модель памяти C++ определяет правила: когда и какие переупорядочивания допустимы, и как потоки видят записи друг друга.

### Sequenced-Before

**Sequenced-before** — отношение между операциями **внутри одного потока**. Если A sequenced-before B, то A выполняется до B и B видит результат A:

```cpp
// В одном потоке:
int x = 5;        // A
int y = x + 1;    // B
```

Это самое простое отношение — обычный порядок выполнения строк кода. Определяет порядок **в одном потоке**, ничего не говорит о разных потоках.

### Synchronizes-With

**Synchronizes-with** — отношение между операциями **разных потоков**.

```cpp
std::atomic<bool> ready{false};
int data = 0;

// Поток A (producer):
data = 42;                                    // (1)
ready.store(true, std::memory_order_release); // (2)

// Поток B (consumer):
while (!ready.load(std::memory_order_acquire)); // (3)
std::cout << data;                              // (4)
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

### Разновидности Memory Order

* `memory_order_relaxed`

```cpp
counter.fetch_add(1, std::memory_order_relaxed);

// Порядок между операциями НЕ гарантирован:
x.store(1, std::memory_order_relaxed);
y.store(1, std::memory_order_relaxed);
```

* `memory_order_acquire`

```cpp
auto val = ptr.load(std::memory_order_acquire);
```

* `memory_order_release`

```cpp
data = 42;
ptr.store(new_val, std::memory_order_release);
```

* `memory_order_acq_rel`

```cpp
int old = state.exchange(NEW, std::memory_order_acq_rel);
```

* `memory_order_seq_cst`

```cpp
// Значение по умолчанию для всех атомарных операций
x.store(1);  // == x.store(1, memory_order_seq_cst)
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
std::atomic_thread_fence(std::memory_order_release);

std::atomic_thread_fence(std::memory_order_acquire);

// Использование:
data = 42;
std::atomic_thread_fence(std::memory_order_release);
flag.store(true, std::memory_order_relaxed);
```

Fence сильнее чем просто `store(release)`: он применяется ко всем предшествующим операциям, а не только к одному атому.

### Инструкция `mfence` (x86)

На уровне железа x86 имеет три вида барьеров:

```text
LFENCE — Load Fence:  все loads до LFENCE завершены до loads после
SFENCE — Store Fence: все stores до SFENCE видны до stores после
MFENCE — Full Fence:  полный барьер — и loads и stores
```

Что генерирует компилятор

```cpp
x.store(1, std::memory_order_seq_cst);
// → MOV [x], 1
// → MFENCE

x.store(1, std::memory_order_release);
// → MOV [x], 1  (x86 store уже имеет release семантику)

auto v = x.load(std::memory_order_acquire);
// → MOV eax, [x]  (x86 load уже имеет acquire семантику)
```
