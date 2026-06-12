## Понятие data race. Store buffering. Класс std::atomic и его основные методы. Спинлок. Реализация mutex через спинлок.

### Data Race

**Data race** — ситуация, когда два потока одновременно обращаются к одной памяти, 
хотя бы одно обращение — запись, и нет синхронизации между ними. Результат — неопределённое поведение (UB) по стандарту C++.

```cpp
int counter = 0;

void thread1() { counter++; }
void thread2() { counter++; }
```

`counter++` на уровне машинного кода — это три инструкции:

```asm
MOV  eax, [counter]
ADD  eax, 1
MOV  [counter], eax
```

Между любыми двумя из них планировщик может переключить поток — и оба потока запишут одно и то же значение, потеряв один инкремент.

### Store Buffering

```text
// Начальное состояние: x = 0, y = 0

// Поток 1 (ядро 1)       // Поток 2 (ядро 2)
x = 1;                    y = 1;
int r1 = y;               int r2 = x;
```

Это называется **слабой моделью памяти**. Чтобы её запретить, нужны барьеры памяти (memory barriers / fences), 
которые заставляют сбросить store buffer.

### `std::atomic`

`std::atomic<T>` — обёртка, гарантирующая что операции над `T` атомарны и имеют чётко определённую видимость между потоками.

#### Основные методы

```cpp
std::atomic<int> a{0};

a.store(42);
int v = a.load();
int old = a.exchange(10);

// Compare-And-Swap (CAS) — основа всех lock-free алгоритмов
int expected = 5;
bool ok = a.compare_exchange_strong(expected, 99);

// Арифметика (только для целых)
a.fetch_add(1);
a.fetch_sub(1);
```

### `compare_exchange_weak` vs `strong`

```cpp
while (!a.compare_exchange_weak(expected, new_val)) {}

if (a.compare_exchange_strong(expected, new_val)) { /* точно сработало */ }
```

### Memory order

Каждая операция принимает необязательный параметр `memory_order`:

| `memory_order` | 	Что гарантирует                                          |
|----------------|-----------------------------------------------------------|
| `relaxed`      | 	Только атомарность, никаких барьеров                     |
| `acquire`      | 	Всё написанное до `release` в другом потоке — видно      |
| `release`      | Все мои предыдущие записи видны тому, кто сделает acquire |
| `seq_cst`      | Полный барьер, глобальный порядок (по умолчанию)          |

```cpp
std::atomic<bool> ready{false};
int data = 0;

// Производитель
data = 42;
ready.store(true, std::memory_order_release);

// Потребитель
while (!ready.load(std::memory_order_acquire));
assert(data == 42);
```

### Спинлок

Спинлок — простейший мьютекс: поток не засыпает, а крутится в цикле (spin), 
пока не захватит замок.

```cpp
#include <atomic>

class Spinlock {
public:
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {}
    }

    void unlock() {
        flag_.clear(std::memory_order_release); // сбросить флаг
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};
```

### Реализация `mutex` через спинлок

Чистый спинлок плох при долгом ожидании — сжигает CPU. Настоящий `mutex` добавляет передачу управления ОС (`yield` или сон):

```cpp
#include <atomic>
#include <thread>

class Mutex {
public:
    void lock() {
        int spin_count = 0;
        while (flag_.test_and_set(std::memory_order_acquire)) {
            ++spin_count;
            if (spin_count < 16) {
                // Фаза 1: активный спин
            } else {
                std::this_thread::yield();
            }
        }
    }

    void unlock() {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};
```

### Сравнение реализаций

|                   | Спинлок                  | Mutex (гибрид)           | std::mutex            |
|-------------------|--------------------------|--------------------------|-----------------------|
| Короткое ожидание | ✅ очень быстро           | ✅ быстро                 | ✅ (futex в userspace) |
| Долгое ожидание   | ❌ 100% CPU               | ✅ yield снижает нагрузку | ✅ поток засыпает      |
| Пробуждение       | мгновенное               | мгновенное               | overhead syscall      |
| Применение        | spinlock в ядре ОС | общий случай             | общий случай          |