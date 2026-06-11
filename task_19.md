## Lock-free очередь (очередь Майкла-Скотта) и ее реализация. Проблема ABA, решение этой проблемы.

### Очередь Майкла-Скотта (Michael-Scott Queue, 1996)

Lock-free очередь на односвязном списке с двумя указателями: `head` (откуда читаем) и `tail` (куда пишем). 
Ключевая идея — голова и хвост обновляются независимо, что позволяет `enqueue` и `dequeue` работать параллельно без конфликтов.

#### Структура

![alt text](images/lfqueue.png)

**Sentinel (dummy) узел** — пустой узел всегда стоит перед первым элементом. 
Это упрощает логику: head всегда указывает на dummy, реальные данные начинаются с `head->next`.

### Реализация

```cpp
#include <atomic>
#include <optional>

template<typename T>
class MSQueue {
    struct Node {
        std::optional<T>   value;      // nullopt у dummy-узла
        std::atomic<Node*> next{nullptr};

        Node() = default;                        // dummy
        explicit Node(T val) : value(std::move(val)) {}
    };

    // Выравнивание: head и tail в разных кэш-линиях
    // (иначе — false sharing между enqueue и dequeue)
    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;

public:
    MSQueue() {
        Node* dummy = new Node();  // sentinel
        head_.store(dummy);
        tail_.store(dummy);
    }

    // Enqueue: добавить в хвост
    void enqueue(T value) {
        Node* new_node = new Node(std::move(value));

        while (true) {
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = tail->next.load(std::memory_order_acquire);

            // Проверяем что tail не устарел пока читали
            if (tail != tail_.load(std::memory_order_acquire)) continue;

            if (next == nullptr) {
                // Хвост действительно последний — пробуем вставить
                if (tail->next.compare_exchange_weak(
                        next, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    // Вставили! Пробуем подвинуть tail (может не успеть —
                    // это нормально, следующий enqueue доделает)
                    tail_.compare_exchange_strong(
                        tail, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;
                }
            } else {
                // Другой поток вставил узел, но не успел подвинуть tail
                // Помогаем ему (это суть алгоритма — "helping")
                tail_.compare_exchange_strong(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
        }
    }

    // Dequeue: забрать из головы
    std::optional<T> dequeue() {
        while (true) {
            Node* head = head_.load(std::memory_order_acquire);
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = head->next.load(std::memory_order_acquire);

            if (head != head_.load(std::memory_order_acquire)) continue;

            if (head == tail) {
                // Очередь пуста или tail отстаёт
                if (next == nullptr) return std::nullopt; // пуста

                // tail отстаёт — помогаем подвинуть
                tail_.compare_exchange_strong(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }

            // Читаем значение ДО CAS (после CAS узел может быть удалён)
            T value = std::move(*next->value);

            // Пробуем переставить head на next
            if (head_.compare_exchange_weak(
                    head, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                delete head; // старый dummy удаляется
                // next становится новым dummy
                return value;
            }
            // CAS не удался — кто-то другой забрал элемент, повторяем
        }
    }

    ~MSQueue() {
        while (dequeue().has_value()) {}
        delete head_.load(); // удаляем последний dummy
    }
};
```

### Механизм "Helping"

Ключевая особенность алгоритма — помощь незавершённым операциям. 
Если поток приостановился после вставки узла, но до обновления `tail`:

```text
Состояние:  head→[dummy]→[A]→[B]   tail→[A]  (tail отстаёт!)

Поток 1 (enqueue C): вставил C после B, но не успел подвинуть tail
                     приостановлен планировщиком

Поток 2 (enqueue D): читает tail→[A], next = [B] (не null!)
                     → "tail отстаёт, помогу":
                     CAS(tail, [A], [B])
                     теперь tail→[B]
                     повторяет цикл → CAS(tail, [B], [C])
                     теперь tail→[C]
                     вставляет D: [C]→[D], CAS(tail,[C],[D])

Поток 1 просыпается: пробует CAS(tail, [A], [C]) → неудача (tail уже [D])
                     но это нормально — tail уже правильный
```

Это делает алгоритм lock-free: даже если один поток завис, другие продвигают очередь вперёд.

### Проблема ABA

ABA возникает при `dequeue`. Поток читает `head`, его вытесняют, другой поток удаляет и переиспользует тот же адрес:

#### Для наглядности ABA стоит привести пример на стеке

Пусть стек такой:

```text
head -> A -> B -> C
```

Поток T1 хочет сделать `pop()`:

* читает `old_head = A`;

* читает `next = B`;

* собирается сделать `CAS(head, A, B)`.

Но его вытесняют.

Что делают другие потоки
Пока T1 спит, поток T2 делает:

* `pop(A)` → теперь `head -> B -> C`

* `pop(B)` → теперь `head -> C`

* `push(A)` → теперь снова `head -> A -> C`

**Обратим внимание:** `head` снова указывает на `A`, то есть внешне значение стало таким же, как видел T1 раньше, хотя стек уже изменился.

CAS проходит успешно, потому что в `head` и правда сейчас лежит `A`. Но это уже не тот же самый логический момент состояния: узел `B` мог быть уже удалён, переиспользован или вообще больше не принадлежать стеку.

В итоге T1 ставит `head = B`, хотя корректный стек сейчас был `A -> C`, и структура ломается.

### Решение 1: Tagged Pointer (версионный счётчик)

Упаковать счётчик в указатель. На x86-64 верхние 16 бит виртуального адреса не используются:

```cpp
struct TaggedPtr {
    uintptr_t value;

    static TaggedPtr make(Node* ptr, uint16_t tag) {
        return { (uintptr_t(tag) << 48) | (uintptr_t(ptr) & 0x0000'FFFF'FFFF'FFFF) };
    }

    Node*    ptr() const { return reinterpret_cast<Node*>(value & 0x0000'FFFF'FFFF'FFFF); }
    uint16_t tag() const { return value >> 48; }

    bool operator==(TaggedPtr o) const { return value == o.value; }
};

// Атомарные операции с TaggedPtr через atomic<uintptr_t>
std::atomic<TaggedPtr> head_;
std::atomic<TaggedPtr> tail_;

// При dequeue — инкрементируем тег:
TaggedPtr new_head = TaggedPtr::make(next, old_head.tag() + 1);
head_.compare_exchange_weak(old_head, new_head, ...);

// Теперь ABA невозможна:
// даже если адрес совпал — тег будет другим → CAS провалится
```

### Решение 2: Hazard Pointers

Каждый поток публикует адреса узлов которые он сейчас использует — они не могут быть удалены:

```cpp
// Глобальный массив "опасных" указателей (по одному на поток)
constexpr int MAX_THREADS = 64;
std::atomic<Node*> hazard_ptrs[MAX_THREADS];
thread_local int   thread_id = /* назначается при старте */;

std::optional<T> dequeue() {
    while (true) {
        Node* head = head_.load(std::memory_order_acquire);

        // Публикуем: "я работаю с head, не удаляй его"
        hazard_ptrs[thread_id].store(head, std::memory_order_seq_cst);

        // Проверяем что head не изменился пока мы публиковали
        if (head != head_.load(std::memory_order_acquire)) continue;

        Node* next = head->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            hazard_ptrs[thread_id].store(nullptr);
            return std::nullopt;
        }

        T value = *next->value;

        if (head_.compare_exchange_weak(head, next, ...)) {
            hazard_ptrs[thread_id].store(nullptr); // снимаем защиту
            retire(head); // не delete сразу — откладываем
            return value;
        }
    }
}

// Отложенное удаление: удалить только если никто не держит hazard
void retire(Node* node) {
    retired_list.push_back(node);

    if (retired_list.size() >= THRESHOLD) {
        for (Node* n : retired_list) {
            bool safe = true;
            for (int i = 0; i < MAX_THREADS; i++) {
                if (hazard_ptrs[i].load() == n) {
                    safe = false; break; // кто-то ещё использует
                }
            }
            if (safe) delete n;
        }
    }
}
```

### Сравнение решений ABA

| Решение           | Сложность | Производительность | Ограничения                                 |
| ----------------- | --------- | ------------------ | ------------------------------------------- |
| Tagged Pointer    | Низкая    | Высокая            | Зависит от архитектуры (16 бит тега)        |
| Hazard Pointers   | Средняя   | Средняя            | $\mathcal{O}(N·P)$ памяти, N потоков                    |