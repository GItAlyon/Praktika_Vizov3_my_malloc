#include "my_malloc.h"
#include <string.h>     // для memcpy, memset
#include <stdint.h>     // для uintptr_t
#include <stdio.h>      // для printf, fprintf
#include <stdlib.h>     // для size_t

// Для Windows используем VirtualAlloc
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN  // Ускоряем компиляцию Windows-заголовков
#include <windows.h>         // для VirtualAlloc, VirtualFree
#else
#include <unistd.h>          // для sbrk (Linux/Unix)
#include <sys/mman.h>        // для mmap (альтернатива)
#endif

// КОНСТАНТЫ

// Выравнивание до 16 байт для SSE/AVX
// 16 байт оптимально для 64-битных процессоров
#define ALIGNMENT 16
// Макрос для выравнивания размера: округляет вверх до ALIGNMENT
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// Минимальный размер блока: заголовок + минимум 32 байта данных
// Нужен, чтобы не создавать слишком маленькие блоки (это снижает фрагментацию)
#define MIN_BLOCK_SIZE (ALIGN(sizeof(BlockHeader) + 32))

// Магическое число для проверки целостности
// Используется как "отпечаток" валидного блока
#define MAGIC_NUMBER 0xDEADBEEF

// Запрашиваем память кусками по 16KB
// Это уменьшает количество системных вызовов
#define CHUNK_SIZE 16384

// СТРУКТУРЫ

// Заголовок блока памяти - метаданные, которые хранятся перед данными
typedef struct BlockHeader {
    size_t size;                 // Общий размер блока (включая заголовок)
    size_t magic;                // Магическое число (0xDEADBEEF) - проверка валидности
    struct BlockHeader* next;    // Следующий в списке занятых блоков
    struct BlockHeader* prev;    // Предыдущий в списке занятых блоков
    struct BlockHeader* next_free; // Следующий в списке свободных блоков
    struct BlockHeader* prev_free; // Предыдущий в списке свободных блоков
    int free;                    // 1 - блок свободен, 0 - блок занят
    int canary;                  // Сторожевое значение для обнаружения переполнения буфера
    char padding[4];             // Выравнивание структуры до 16 байт
} BlockHeader;

// Структура для сбора статистики работы аллокатора
typedef struct {
    size_t total_allocated;      // Сколько всего памяти запросили у ОС
    size_t total_freed;          // Сколько всего памяти освободили
    size_t current_allocated;    // Сколько памяти сейчас используется (выделено - освобождено)
    size_t peak_allocated;       // Максимальное использование памяти (пик)
    size_t malloc_calls;         // Счётчик вызовов malloc
    size_t free_calls;           // Счётчик вызовов free
    size_t realloc_calls;        // Счётчик вызовов realloc
    size_t calloc_calls;         // Счётчик вызовов calloc
    size_t failed_allocations;   // Сколько раз не удалось выделить память
    size_t coalesce_operations;  // Сколько раз выполнялось слияние блоков
    size_t split_operations;     // Сколько раз выполнялось разделение блоков
} AllocatorStats;

// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ

static BlockHeader* free_list = NULL;      // Голова списка свободных блоков
static BlockHeader* allocated_list = NULL; // Голова списка занятых блоков
static void* heap_start = NULL;            // Начало кучи (самый первый блок)
static void* heap_end = NULL;              // Конец кучи (для проверки границ)
static AllocatorStats stats = { 0 };       // Статистика (все нули при старте)
static int debug_mode = 0;                 // Режим отладки (0 - выкл, 1 - вкл)
static int search_strategy = 0;            // 0 - Best-Fit, 1 - First-Fit

// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ

// Получение указателя на заголовок блока по указателю на данные
// Для этого отступаем назад на размер заголовка
static inline BlockHeader* get_block_header(void* ptr) {
    return (BlockHeader*)((char*)ptr - sizeof(BlockHeader));
}

// Получение указателя на данные пользователя по заголовку
// Для этого отступаем вперёд на размер заголовка
static inline void* get_user_data(BlockHeader* block) {
    return (void*)((char*)block + sizeof(BlockHeader));
}

// Проверка целостности блока
// Проверяем все поля, чтобы убедиться, что блок валидный
static inline int validate_block(BlockHeader* block) {
    if (block == NULL) return 0;                    // Нулевой указатель
    if (block->magic != MAGIC_NUMBER) return 0;     // Неправильное магическое число
    if (block->canary != 0x5A5A5A5A) return 0;      // Испорчен canary (переполнение)
    if ((uintptr_t)block < (uintptr_t)heap_start) return 0; // Блок до начала кучи
    if ((uintptr_t)block + block->size > (uintptr_t)heap_end) return 0; // Блок за концом кучи
    return 1;  // Все проверки пройдены
}

// Установка canary-значения (сторожевой байт) для нового блока
static inline void set_canary(BlockHeader* block) {
    block->canary = 0x5A5A5A5A;  // Случайное значение, которое легко проверить
}

// Проверка canary (обнаружение переполнения буфера)
// Если canary изменился - значит, кто-то вышел за границы блока
static inline int check_canary(BlockHeader* block) {
    return block->canary == 0x5A5A5A5A;  // Возвращает 1, если canary не повреждён
}

// РАБОТА СО СПИСКАМИ

// Добавление блока в список свободных (с сортировкой по адресам)
// Сортировка нужна для эффективного слияния соседних блоков
static void add_to_free_list(BlockHeader* block) {
    // Помечаем блок как свободный и обнуляем указатели
    block->free = 1;
    block->next_free = NULL;
    block->prev_free = NULL;
    set_canary(block);              // Устанавливаем сторожевой байт
    block->magic = MAGIC_NUMBER;    // Устанавливаем магическое число

    // Если список пуст - делаем блок первым
    if (free_list == NULL) {
        free_list = block;
        return;
    }

    // Сортировка по адресам для эффективного слияния
    // Ищем место для вставки (блоки упорядочены по возрастанию адресов)
    BlockHeader* curr = free_list;
    BlockHeader* prev = NULL;

    // Идём по списку, пока не найдём блок с большим адресом
    while (curr != NULL && (uintptr_t)curr < (uintptr_t)block) {
        prev = curr;
        curr = curr->next_free;
    }

    if (prev == NULL) {
        // Вставляем в начало списка
        block->next_free = free_list;
        free_list->prev_free = block;
        free_list = block;
    }
    else {
        // Вставляем после prev
        block->prev_free = prev;
        block->next_free = curr;
        prev->next_free = block;
        if (curr != NULL) {
            curr->prev_free = block;
        }
    }
}

// Удаление блока из списка свободных
static void remove_from_free_list(BlockHeader* block) {
    // Связываем предыдущий и следующий блоки между собой (обходим удаляемый)
    if (block->prev_free != NULL) {
        block->prev_free->next_free = block->next_free;
    }
    else {
        // Если удаляем первый блок - обновляем голову списка
        free_list = block->next_free;
    }

    if (block->next_free != NULL) {
        block->next_free->prev_free = block->prev_free;
    }

    // Обнуляем указатели удалённого блока (безопасность)
    block->next_free = NULL;
    block->prev_free = NULL;
}

// Добавление блока в список занятых (в начало списка)
static void add_to_allocated_list(BlockHeader* block) {
    block->next = allocated_list;  // Следующим становится бывший первый
    if (allocated_list != NULL) {
        allocated_list->prev = block;  // Бывший первый теперь указывает на новый
    }
    block->prev = NULL;
    allocated_list = block;  // Новый блок становится первым
}

// Удаление блока из списка занятых
static void remove_from_allocated_list(BlockHeader* block) {
    // Связываем предыдущий и следующий блоки между собой
    if (block->prev != NULL) {
        block->prev->next = block->next;
    }
    else {
        // Если удаляем первый блок - обновляем голову списка
        allocated_list = block->next;
    }

    if (block->next != NULL) {
        block->next->prev = block->prev;
    }
    // Обнуляем указатели (безопасность)
    block->next = NULL;
    block->prev = NULL;
}

// СЛИЯНИЕ И РАЗДЕЛЕНИЕ

// Слияние соседних свободных блоков для уменьшения фрагментации
// Если два свободных блока находятся рядом - объединяем их в один большой
static void coalesce_blocks(BlockHeader* block) {
    BlockHeader* curr = free_list;

    // Проходим по всем свободным блокам в поисках соседей
    while (curr != NULL) {
        BlockHeader* next = curr->next_free;

        // Проверка соседства справа: curr находится сразу после block
        // (char*)curr == (char*)block + block->size - блоки соприкасаются
        if ((char*)curr == (char*)block + block->size) {
            // Объединяем: увеличиваем размер block на размер curr
            block->size += curr->size;
            remove_from_free_list(curr);  // Удаляем curr из свободных
            stats.coalesce_operations++;   // Считаем операцию слияния
            curr = free_list;              // Начинаем поиск заново
            continue;
        }

        // Проверка соседства слева: block находится сразу после curr
        if ((char*)block == (char*)curr + curr->size) {
            // Объединяем: увеличиваем размер curr на размер block
            curr->size += block->size;
            block = curr;  // Обновляем block для дальнейшего слияния
            remove_from_free_list(curr);   // Удаляем curr из свободных
            stats.coalesce_operations++;
            curr = free_list;  // Начинаем поиск заново
            continue;
        }

        curr = next;  // Переходим к следующему блоку
    }
}

// Разделение блока на две части: занятую и свободную
// Используется, когда блок слишком большой для запроса
static void split_block(BlockHeader* block, size_t needed_size) {
    size_t remaining = block->size - needed_size;

    // Если остаток достаточно большой - создаём из него новый свободный блок
    if (remaining >= MIN_BLOCK_SIZE) {
        // Создаём новый блок из остатка (сразу после нужного размера)
        BlockHeader* new_block = (BlockHeader*)((char*)block + needed_size);
        new_block->size = remaining;
        new_block->free = 1;
        new_block->magic = MAGIC_NUMBER;
        set_canary(new_block);
        new_block->next = NULL;
        new_block->prev = NULL;
        new_block->next_free = NULL;
        new_block->prev_free = NULL;

        // Уменьшаем размер исходного блока
        block->size = needed_size;

        // Добавляем новый свободный блок в список
        add_to_free_list(new_block);
        stats.split_operations++;  // Считаем операцию разделения
    }
}

// ЗАПРОС ПАМЯТИ У ОС

// Запрос памяти у операционной системы
// В Windows используем VirtualAlloc, в Linux - sbrk
static BlockHeader* request_from_os(size_t size) {
    // Округляем размер до CHUNK_SIZE для уменьшения системных вызовов
    // Запрашиваем память кратную 16KB
    size_t request_size = (size + CHUNK_SIZE - 1) & ~(CHUNK_SIZE - 1);

    void* ptr = NULL;

#ifdef _WIN32
    // Для Windows используем VirtualAlloc
    // MEM_COMMIT | MEM_RESERVE - выделяем физическую и виртуальную память
    // PAGE_READWRITE - разрешаем чтение и запись
    ptr = VirtualAlloc(NULL, request_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (ptr == NULL) {
        stats.failed_allocations++;
        return NULL;
    }
#else
    // Для Linux/Unix используем sbrk (расширяем кучу)
    ptr = sbrk(request_size);
    if (ptr == (void*)-1) {
        stats.failed_allocations++;
        return NULL;
    }
#endif

    // Запоминаем границы кучи (для проверок валидности)
    if (heap_start == NULL) {
        heap_start = ptr;
    }
    heap_end = (char*)ptr + request_size;

    // Инициализируем заголовок нового блока
    BlockHeader* block = (BlockHeader*)ptr;
    block->size = request_size;
    block->free = 0;               // Новый блок занят
    block->magic = MAGIC_NUMBER;
    set_canary(block);
    block->next = NULL;
    block->prev = NULL;
    block->next_free = NULL;
    block->prev_free = NULL;

    // Обновляем статистику
    stats.total_allocated += request_size;
    stats.current_allocated += request_size;
    if (stats.current_allocated > stats.peak_allocated) {
        stats.peak_allocated = stats.current_allocated;
    }

    return block;
}

// ПОИСКОВЫЕ СТРАТЕГИИ

// Best-Fit стратегия - ищет наименьший блок, который подходит по размеру
// Преимущество: меньше фрагментации
// Недостаток: медленнее (нужно просмотреть все блоки)
static BlockHeader* find_best_fit(size_t size) {
    BlockHeader* best = NULL;
    BlockHeader* curr = free_list;
    size_t best_size = (size_t)-1;  // Максимально возможное число

    while (curr != NULL) {
        // Если блок подходит и меньше текущего лучшего
        if (curr->size >= size && curr->size < best_size) {
            best = curr;
            best_size = curr->size;
            if (best_size == size) break;  // Идеальное совпадение - выходим
        }
        curr = curr->next_free;
    }

    return best;
}

// First-Fit стратегия - ищет первый подходящий блок
// Преимущество: быстрее (может найти блок сразу)
// Недостаток: больше фрагментация
static BlockHeader* find_first_fit(size_t size) {
    BlockHeader* curr = free_list;
    while (curr != NULL) {
        if (curr->size >= size) {
            return curr;  // Возвращаем первый подходящий
        }
        curr = curr->next_free;
    }
    return NULL;
}

// ОСНОВНЫЕ ФУНКЦИИ

// Выделение памяти - аналог стандартного malloc
// Алгоритм:
// 1. Выравниваем размер
// 2. Ищем подходящий блок в свободном списке
// 3. Если нет - запрашиваем у ОС
// 4. Разделяем блок, если он слишком большой
// 5. Добавляем в список занятых
// 6. Возвращаем указатель на данные
void* my_malloc(size_t size) {
    stats.malloc_calls++;  // Считаем вызов

    // По стандарту malloc(0) может вернуть NULL или валидный указатель
    // Мы возвращаем NULL (стандартное поведение)
    if (size == 0) {
        return NULL;
    }

    // Выравниваем размер: добавляем место под заголовок и округляем
    size_t total_size = ALIGN(size + sizeof(BlockHeader));
    if (total_size < MIN_BLOCK_SIZE) {
        total_size = MIN_BLOCK_SIZE;  // Не создаём слишком маленькие блоки
    }

    // Поиск подходящего блока
    BlockHeader* block = NULL;
    if (search_strategy == 0) {
        block = find_best_fit(total_size);   // Best-Fit
    }
    else {
        block = find_first_fit(total_size);  // First-Fit
    }

    if (block == NULL) {
        // Нет подходящего блока - запрашиваем память у ОС
        block = request_from_os(total_size);
        if (block == NULL) {
            return NULL;  // Не удалось выделить память
        }
    }
    else {
        // Нашли подходящий блок - удаляем его из свободных
        remove_from_free_list(block);
        stats.current_allocated += block->size;
        if (stats.current_allocated > stats.peak_allocated) {
            stats.peak_allocated = stats.current_allocated;
        }

        // Разделяем блок, если он слишком большой
        split_block(block, total_size);
    }

    // Помечаем блок как занятый и добавляем в список занятых
    block->free = 0;
    add_to_allocated_list(block);

    // Отладочный вывод
    if (debug_mode) {
        printf("[ОТЛАДКА] malloc: размер=%zu, блок=%p, данные=%p\n",
            size, block, get_user_data(block));
    }

    // Возвращаем указатель на данные (после заголовка)
    return get_user_data(block);
}

// Освобождение памяти - аналог стандартного free
// Алгоритм:
// 1. Проверяем валидность указателя
// 2. Проверяем canary (переполнение буфера)
// 3. Проверяем двойное освобождение
// 4. Удаляем из списка занятых
// 5. Добавляем в список свободных
// 6. Выполняем слияние соседних свободных блоков
void my_free(void* ptr) {
    if (ptr == NULL) return;  // free(NULL) - ничего не делаем

    stats.free_calls++;

    BlockHeader* block = get_block_header(ptr);

    // Проверка валидности указателя
    if (!validate_block(block)) {
        fprintf(stderr, "ПРЕДУПРЕЖДЕНИЕ: Неверный указатель передан в free: %p\n", ptr);
        return;
    }

    // Проверка canary - если испорчен, значит было переполнение буфера
    if (!check_canary(block)) {
        fprintf(stderr, "ОШИБКА: Обнаружено переполнение буфера в блоке %p\n", block);
        return;
    }

    // Проверка на двойное освобождение
    if (block->free) {
        fprintf(stderr, "ПРЕДУПРЕЖДЕНИЕ: Двойное освобождение: %p\n", ptr);
        return;
    }

    // Удаляем из списка занятых
    remove_from_allocated_list(block);
    stats.current_allocated -= block->size;
    stats.total_freed += block->size;

    // Добавляем в список свободных
    add_to_free_list(block);

    // Слияние соседних свободных блоков (уменьшает фрагментацию)
    coalesce_blocks(block);

    if (debug_mode) {
        printf("[ОТЛАДКА] free: блок=%p, размер=%zu\n", block, block->size);
    }
}

// Изменение размера памяти - аналог стандартного realloc
// Алгоритм:
// 1. Если ptr == NULL - работаем как malloc
// 2. Если new_size == 0 - работаем как free и возвращаем NULL
// 3. Если текущий блок достаточно большой - уменьшаем его
// 4. Пытаемся расширить блок за счёт соседнего свободного
// 5. Если не получается - выделяем новый блок и копируем данные
void* my_realloc(void* ptr, size_t new_size) {
    // Если ptr == NULL, то realloc работает как malloc
    if (ptr == NULL) {
        return my_malloc(new_size);
    }

    // Если new_size == 0, то realloc работает как free и возвращает NULL
    if (new_size == 0) {
        my_free(ptr);
        return NULL;
    }

    stats.realloc_calls++;

    BlockHeader* block = get_block_header(ptr);

    // Проверяем валидность указателя
    if (!validate_block(block)) {
        fprintf(stderr, "ОШИБКА: Неверный указатель в realloc: %p\n", ptr);
        return NULL;
    }

    // Выравниваем новый размер
    size_t total_new_size = ALIGN(new_size + sizeof(BlockHeader));
    if (total_new_size < MIN_BLOCK_SIZE) {
        total_new_size = MIN_BLOCK_SIZE;
    }

    // СЛУЧАЙ 1: Уменьшаем блок (если он больше нового размера)
    if (block->size >= total_new_size) {
        size_t remaining = block->size - total_new_size;
        // Если остаток достаточно большой - создаём из него свободный блок
        if (remaining >= MIN_BLOCK_SIZE) {
            // Удаляем блок из занятых (временно)
            remove_from_allocated_list(block);

            // Создаём новый свободный блок из остатка
            BlockHeader* new_block = (BlockHeader*)((char*)block + total_new_size);
            new_block->size = remaining;
            new_block->free = 1;
            new_block->magic = MAGIC_NUMBER;
            set_canary(new_block);

            // Уменьшаем размер текущего блока
            block->size = total_new_size;
            // Возвращаем блок в список занятых
            add_to_allocated_list(block);
            // Добавляем новый свободный блок
            add_to_free_list(new_block);
            // Сливаем с соседями (может объединиться с другими свободными)
            coalesce_blocks(new_block);

            stats.current_allocated -= remaining;
            stats.split_operations++;
        }
        // Если остаток маленький - просто оставляем как есть
        return ptr;
    }

    // СЛУЧАЙ 2: Пытаемся расширить блок за счёт соседнего свободного справа
    size_t needed = total_new_size - block->size;
    BlockHeader* curr = free_list;

    // Ищем соседний свободный блок, который находится сразу после нашего
    while (curr != NULL) {
        // Проверяем, что curr находится сразу после block
        if ((char*)curr == (char*)block + block->size) {
            // Если curr достаточно большой - расширяемся за его счёт
            if (curr->size >= needed) {
                // Удаляем curr из свободных и block из занятых
                remove_from_free_list(curr);
                remove_from_allocated_list(block);

                // Расширяем block
                block->size += curr->size;
                stats.current_allocated += curr->size;

                // Проверяем остаток после расширения
                size_t remaining = block->size - total_new_size;
                if (remaining >= MIN_BLOCK_SIZE) {
                    // Создаём новый свободный блок из остатка
                    BlockHeader* new_block = (BlockHeader*)((char*)block + total_new_size);
                    new_block->size = remaining;
                    new_block->free = 1;
                    new_block->magic = MAGIC_NUMBER;
                    set_canary(new_block);

                    block->size = total_new_size;
                    stats.current_allocated -= remaining;
                    stats.split_operations++;
                    add_to_free_list(new_block);
                }

                // Возвращаем block в список занятых
                add_to_allocated_list(block);
                return ptr;
            }
            // Если curr недостаточно большой - выходим (дальше блоки не подойдут)
            break;
        }
        curr = curr->next_free;
    }

    // СЛУЧАЙ 3: Не удалось расширить - выделяем новый блок
    void* new_ptr = my_malloc(new_size);
    if (new_ptr == NULL) {
        return NULL;  // Не удалось выделить память
    }

    // Копируем данные из старого блока в новый
    size_t copy_size = block->size - sizeof(BlockHeader);
    if (copy_size > new_size) {
        copy_size = new_size;  // Копируем только новое количество байт
    }
    memcpy(new_ptr, ptr, copy_size);

    // Освобождаем старый блок
    my_free(ptr);
    return new_ptr;
}

// Выделение памяти с обнулением - аналог стандартного calloc
// Отличается от malloc тем, что заполняет память нулями
void* my_calloc(size_t num, size_t size) {
    size_t total = num * size;
    void* ptr = my_malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);  // Заполняем нулями
        stats.calloc_calls++;
    }
    return ptr;
}

// ОТЛАДОЧНЫЕ ФУНКЦИИ

// Включение/отключение режима отладки
// При включении будут выводиться сообщения о каждом malloc/free
void my_malloc_debug(int enable) {
    debug_mode = enable;
}

// Вывод статистики работы аллокатора
// Показывает всю собранную информацию о работе с памятью
void my_malloc_stats(void) {
    printf("\n=== СТАТИСТИКА РАБОТЫ АЛЛОКАТОРА ===\n");
    printf("Всего выделено:         %10zu байт\n", stats.total_allocated);
    printf("Всего освобождено:      %10zu байт\n", stats.total_freed);
    printf("Текущая выделенная:     %10zu байт\n", stats.current_allocated);
    printf("Пик выделения:          %10zu байт\n", stats.peak_allocated);
    printf("Вызовов malloc:         %10zu\n", stats.malloc_calls);
    printf("Вызовов free:           %10zu\n", stats.free_calls);
    printf("Вызовов realloc:        %10zu\n", stats.realloc_calls);
    printf("Вызовов calloc:         %10zu\n", stats.calloc_calls);
    printf("Неудачных выделений:    %10zu\n", stats.failed_allocations);
    printf("Операций слияния:       %10zu\n", stats.coalesce_operations);
    printf("Операций разделения:    %10zu\n", stats.split_operations);
    printf("Стратегия поиска:       %s\n",
        search_strategy ? "First-Fit (первый подходящий)" : "Best-Fit (наилучший подходящий)");
    printf("======================================\n");
}

// Проверка целостности всей кучи
// Проходит по всем блокам и проверяет каждый на валидность
// Возвращает 1 если всё хорошо, 0 если есть ошибки
int my_malloc_check(void) {
    int errors = 0;

    // Проверяем все занятые блоки
    BlockHeader* curr = allocated_list;
    while (curr != NULL) {
        if (!validate_block(curr)) {
            fprintf(stderr, "ОШИБКА: Неверный блок по адресу %p\n", curr);
            errors++;
        }
        if (!check_canary(curr)) {
            fprintf(stderr, "ОШИБКА: Нарушена canary по адресу %p (переполнение буфера)\n", curr);
            errors++;
        }
        curr = curr->next;
    }

    // Проверяем все свободные блоки
    curr = free_list;
    while (curr != NULL) {
        if (!validate_block(curr)) {
            fprintf(stderr, "ОШИБКА: Неверный свободный блок по адресу %p\n", curr);
            errors++;
        }
        curr = curr->next_free;
    }

    return errors == 0;  // Если ошибок нет - возвращаем 1
}

// Вывод списков блоков (для отладки)
// Показывает все свободные и все занятые блоки с их размерами
void my_malloc_dump(void) {
    printf("\n   СПИСОК СВОБОДНЫХ БЛОКОВ \n");
    BlockHeader* curr = free_list;
    int count = 0;
    while (curr != NULL) {
        printf("[%d] %p: размер=%zu, свободен=%d\n",
            count++, curr, curr->size, curr->free);
        curr = curr->next_free;
    }

    printf("\n    СПИСОК ЗАНЯТЫХ БЛОКОВ \n");
    curr = allocated_list;
    count = 0;
    while (curr != NULL) {
        printf("[%d] %p: размер=%zu, свободен=%d, canary=%x\n",
            count++, curr, curr->size, curr->free, curr->canary);
        curr = curr->next;
    }
}

// Установка стратегии поиска
// 0 - Best-Fit (меньше фрагментации, но медленнее)
// 1 - First-Fit (быстрее, но больше фрагментации)
void my_malloc_set_strategy(int strategy) {
    search_strategy = strategy;
}

// Получение размера блока (без учёта заголовка)
// Полезно для отладки - сколько байт реально выделено
size_t my_malloc_usable_size(void* ptr) {
    if (ptr == NULL) return 0;
    BlockHeader* block = get_block_header(ptr);
    if (!validate_block(block)) return 0;
    return block->size - sizeof(BlockHeader);  // Размер данных без заголовка
}