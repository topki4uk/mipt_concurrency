## Файберы. Идея реализации context switch. Scheduler, функция yield. Fault injection.

### Файберы (Fibers)

**Файбер** — это «легковесный поток», который управляется в userspace, без участия ядра ОС. 
Ключевое отличие от обычных потоков:

```text
Потоки (threads):              Файберы (fibers):

  Переключение — syscall         Переключение — обычный вызов функции
  Стек: 1-8 MB                   Стек: 4-64 KB (настраиваемый)
  Планировщик: ядро ОС           Планировщик: ваш код в userspace
  Параллельность: настоящая      Параллельность: кооперативная
  Создание: ~10мкс               Создание: ~1мкс
```

Файберы — кооперативные: сами решают когда отдать управление через `yield()`. 
Ядро видит один поток, внутри которого файберы переключаются сами.

### Идея context switch

Чтобы переключиться с файбера A на файбер B, нужно:
* Сохранить состояние A (регистры, указатель стека, указатель инструкций)

* Восстановить состояние B

* Прыгнуть на инструкцию, где B был прерван

Состояние файбера — это его контекст (context):

```text
Контекст файбера (x86-64):

  ┌─────────────┐
  │     rsp     │  ← указатель стека (самое важное)
  │     rbp     │  ← base pointer
  │     rbx     │  ← callee-saved регистры
  │     r12     │
  │     r13     │
  │     r14     │
  │     r15     │
  │     rip     │  ← куда вернуться (адрес следующей инструкции)
  └─────────────┘
```

Регистры `rax`, `rcx`, `rdx`, `rsi`, `rdi` — caller-saved, их сохраняет вызывающий код по соглашению C. 
Поэтому в контексте нужны только callee-saved регистры.

Реализация context switch на ассемблере

```text
; context_switch(Context* from, Context* to)
; rdi = from, rsi = to  (System V AMD64 ABI)

context_switch:
    ; Сохраняем контекст текущего файбера (from)
    mov  [rdi + 0],  rsp
    mov  [rdi + 8],  rbp
    mov  [rdi + 16], rbx
    mov  [rdi + 24], r12
    mov  [rdi + 32], r13
    mov  [rdi + 40], r14
    mov  [rdi + 48], r15

    ; Сохраняем rip через стек: call кладёт адрес возврата на стек,
    ; pop снимает его
    lea  rax, [rip + .return_here]
    mov  [rdi + 56], rax        ; сохраняем адрес возврата

    ; Восстанавливаем контекст нового файбера (to)
    mov  rsp, [rsi + 0]
    mov  rbp, [rsi + 8]
    mov  rbx, [rsi + 16]
    mov  r12, [rsi + 24]
    mov  r13, [rsi + 32]
    mov  r14, [rsi + 40]
    mov  r15, [rsi + 48]

    ; Прыгаем на rip нового файбера
    jmp  [rsi + 56]

.return_here:
    ret
```

### На C++ через `ucontext` (POSIX)

```cpp
#include <ucontext.h>

struct Fiber {
    ucontext_t  ctx_;
    std::vector<char> stack_;

    Fiber(size_t stack_size, void (*func)())
        : stack_(stack_size)
    {
        getcontext(&ctx_);                    // инициализация
        ctx_.uc_stack.ss_sp   = stack_.data();
        ctx_.uc_stack.ss_size = stack_.size();
        ctx_.uc_link          = nullptr;      // куда вернуться по завершении
        makecontext(&ctx_, func, 0);          // связать с функцией
    }
};

// Переключение: сохранить текущий → восстановить следующий
void switch_to(Fiber* from, Fiber* to) {
    swapcontext(&from->ctx_, &to->ctx_);
    // ← сюда вернёмся когда кто-то переключится обратно на from
}
```

### Scheduler и yield

**Планировщик (Scheduler)** в userspace — это просто очередь готовых файберов и логика выбора следующего:

```cpp
#include <queue>
#include <functional>

class Scheduler {
    std::queue<Fiber*> ready_queue_;  // очередь готовых файберов
    Fiber*             current_ = nullptr;
    Fiber              main_fiber_;   // контекст «основного» потока

public:
    void spawn(std::function<void()> func) {
        auto* fiber = new Fiber(64 * 1024, func, this);
        ready_queue_.push(fiber);
    }

    // Запустить следующий файбер из очереди
    void run() {
        while (!ready_queue_.empty()) {
            Fiber* next = ready_queue_.front();
            ready_queue_.pop();

            current_ = next;
            switch_to(&main_fiber_, next); // передаём управление
            // ← вернулись сюда когда next вызвал yield или завершился
        }
    }

    // Вызывается из файбера: отдать управление планировщику
    void yield() {
        Fiber* me = current_;
        ready_queue_.push(me);        // ставим себя обратно в очередь
        current_ = nullptr;
        switch_to(me, &main_fiber_);  // возвращаемся в run()
        // ← сюда вернёмся когда планировщик снова выберет нас
    }

    static Scheduler* current_scheduler; // thread_local для доступа из файбера
};

// Глобальная функция yield (вызывается из кода файбера)
void yield() {
    Scheduler::current_scheduler->yield();
}
```

### Асинхронный I/O + yield

Настоящая польза файберов — скрыть асинхронность за синхронным интерфейсом:

```cpp
// Выглядит как синхронный код:
std::string data = async_read(fd);   // внутри: регистрирует в epoll + yield()
process(data);                       // выполняется когда данные готовы

// Внутри async_read:
std::string async_read(int fd) {
    // Регистрируем fd в epoll
    scheduler->register_io(fd, current_fiber);
    yield();  // отдаём управление — другие файберы работают пока ждём I/O

    // Планировщик разбудит нас когда epoll сообщит о готовности fd
    char buf[4096];
    read(fd, buf, sizeof(buf));
    return std::string(buf);
}
```

### Fault Injection

**Fault injection** — намеренное внесение ошибок в код для тестирования устойчивости. 
В контексте файберов и планировщика это особенно полезно: можно проверить поведение при сбоях которые в реальности редки.

```cpp
// В production: просто вызов функции
// В тестах: может бросить исключение или вернуть ошибку

class FaultInjector {
    struct Fault {
        int   countdown;  // через сколько вызовов сработать
        bool  active;
    };

    std::unordered_map<std::string, Fault> faults_;

public:
    // Зарегистрировать ошибку: через N вызовов точки "name" — сбой
    void inject(const std::string& name, int after_n_calls) {
        faults_[name] = {after_n_calls, true};
    }

    // Вызывается в "точке внедрения"
    void check(const std::string& name) {
        auto it = faults_.find(name);
        if (it == faults_.end()) return;

        auto& fault = it->second;
        if (--fault.countdown <= 0) {
            faults_.erase(it);
            throw std::runtime_error("injected fault: " + name);
        }
    }
};

FaultInjector g_faults; // глобальный или thread_local
```

Внедрение точек в код

```cpp
std::string async_read(int fd) {
    g_faults.check("read");   // ← точка внедрения

    scheduler->register_io(fd, current_fiber);
    yield();

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));

    g_faults.check("read_result"); // ← ещё одна точка

    return std::string(buf, n);
}

void handle_client(int fd) {
    g_faults.check("handle_client"); // ← точка внедрения

    auto data = async_read(fd);
    process(data);
}
```

Применение в тестах

```cpp
// Тест: что происходит если read упадёт на 3-й вызов?
void test_read_failure() {
    g_faults.inject("read", 3); // сбой на 3-м вызове async_read

    Scheduler sched;
    bool error_handled = false;

    sched.spawn([&] {
        try {
            for (int i = 0; i < 5; i++) {
                async_read(some_fd);
            }
        } catch (const std::runtime_error& e) {
            error_handled = true; // убеждаемся что ошибка поймана
        }
    });

    sched.run();
    assert(error_handled);
}
```

### Fault injection + yield: тестирование гонок

В файберном планировщике можно контролировать точно в каком месте произошёл context switch — это позволяет воспроизводить гонки детерминированно:

```cpp
// Обычный yield случаен. Deterministic scheduler:
class DeterministicScheduler : public Scheduler {
    std::vector<int> schedule_; // заданная последовательность: [0,1,0,0,1,...]
    int step_ = 0;

public:
    void yield() override {
        // Переключаемся на файбер номер schedule_[step_++]
        int next_fiber = schedule_[step_++];
        switch_to_fiber(next_fiber);
    }
};

// Тест: воспроизводим конкретную гонку
DeterministicScheduler sched;
sched.set_schedule({0, 1, 0, 1}); // точный порядок переключений

// Теперь тест детерминирован — гонка воспроизводится каждый раз
```