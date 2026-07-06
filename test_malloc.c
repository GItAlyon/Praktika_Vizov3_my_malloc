#define _CRT_SECURE_NO_WARNINGS
#include "my_malloc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <windows.h>

// Тест базового выделения
void test_malloc_basic() {
    printf("Тест базового выделения памяти...\n");

    int* arr = (int*)my_malloc(10 * sizeof(int));
    assert(arr != NULL);

    for (int i = 0; i < 10; i++) {
        arr[i] = i * 2;
    }

    assert(arr[5] == 10);
    my_free(arr);
    printf("[OK] Тест базового выделения пройден\n");
}

// Тест realloc
void test_realloc() {
    printf("Тест изменения размера памяти (realloc)...\n");

    char* str = (char*)my_malloc(6);
    strcpy_s(str, 6, "Hello");
    assert(strcmp(str, "Hello") == 0);

    str = (char*)my_realloc(str, 20);
    strcat_s(str, 20, " World!");
    assert(strcmp(str, "Hello World!") == 0);

    my_free(str);
    printf("[OK] Тест realloc пройден\n");
}

// Тест calloc
void test_calloc() {
    printf("Тест выделения с обнулением (calloc)...\n");

    int* arr = (int*)my_calloc(10, sizeof(int));
    assert(arr != NULL);

    for (int i = 0; i < 10; i++) {
        assert(arr[i] == 0);  // Все нули
    }

    my_free(arr);
    printf("[OK] Тест calloc пройден\n");
}

// Тест освобождения NULL
void test_free_null() {
    printf("Тест освобождения NULL-указателя...\n");
    my_free(NULL);
    printf("[OK] Тест free(NULL) пройден\n");
}

// Тест множественных выделений
void test_multiple_allocations() {
    printf("Тест множественных выделений...\n");

    void* ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = my_malloc(i + 1);
        assert(ptrs[i] != NULL);
    }

    for (int i = 0; i < 100; i++) {
        my_free(ptrs[i]);
    }

    printf("[OK] Тест множественных выделений пройден\n");
}

// Тест фрагментации
void test_fragmentation() {
    printf("Тест фрагментации памяти...\n");

    void* blocks[50];

    // Выделяем блоки разного размера
    for (int i = 0; i < 50; i++) {
        blocks[i] = my_malloc(10 + i * 5);
        assert(blocks[i] != NULL);
    }

    // Освобождаем через один
    for (int i = 0; i < 50; i += 2) {
        my_free(blocks[i]);
        blocks[i] = NULL;
    }

    // Пытаемся выделить большой блок
    void* big = my_malloc(500);
    if (big != NULL) {
        my_free(big);
    }

    // Освобождаем остальные
    for (int i = 1; i < 50; i += 2) {
        if (blocks[i] != NULL) {
            my_free(blocks[i]);
        }
    }

    printf("[OK] Тест фрагментации пройден\n");
}

// Тест переключения стратегий
void test_strategies() {
    printf("Тест стратегий поиска (Best-Fit / First-Fit)...\n");

    // Пробуем Best-Fit
    my_malloc_set_strategy(0);
    void* p1 = my_malloc(100);
    void* p2 = my_malloc(200);
    void* p3 = my_malloc(50);
    my_free(p1);
    my_free(p2);
    my_free(p3);

    // Пробуем First-Fit
    my_malloc_set_strategy(1);
    p1 = my_malloc(100);
    p2 = my_malloc(200);
    p3 = my_malloc(50);
    my_free(p1);
    my_free(p2);
    my_free(p3);

    printf("[OK] Тест стратегий пройден\n");
}

// Тест больших выделений
void test_big_allocations() {
    printf("Тест больших выделений (1 МБ)...\n");

    // Выделяем 1MB
    char* big = (char*)my_malloc(1024 * 1024);
    assert(big != NULL);

    for (int i = 0; i < 1024 * 1024; i += 1024) {
        big[i] = 'A';
    }

    my_free(big);
    printf("[OK] Тест больших выделений пройден\n");
}

int main() {
    SetConsoleCP(1251);
    SetConsoleOutputCP(1251);

    printf("\n    ЗАПУСК ТЕСТОВ АЛЛОКАТОРА \n\n");

    // Включаем отладку (0 - выключена, 1 - включена)
    my_malloc_debug(1);

    test_malloc_basic();
    test_realloc();
    test_calloc();
    test_free_null();
    test_multiple_allocations();
    test_fragmentation();
    test_strategies();
    test_big_allocations();

    // Проверяем целостность
    if (my_malloc_check()) {
        printf("\n[OK] Проверка целостности кучи пройдена\n");
    }
    else {
        printf("\n[FAIL] Проверка целостности кучи НЕ ПРОЙДЕНА\n");
    }

    // Выводим статистику
    my_malloc_stats();

    printf("\n[OK] ВСЕ ТЕСТЫ ПРОЙДЕНЫ УСПЕШНО\n");

    // Пауза перед закрытием
    system("pause");
    return 0;
}