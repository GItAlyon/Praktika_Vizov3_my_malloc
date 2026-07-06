#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

	// ОСНОВНЫЕ ФУНКЦИИ

	// Выделяет память размером size байт
	// Возвращает указатель на выделенную память или NULL при ошибке
	void* my_malloc(size_t size);

	// Освобождает ранее выделенную память
	void my_free(void* ptr);

	// Изменяет размер ранее выделенной памяти
	// Возвращает указатель на новую память или NULL при ошибке
	void* my_realloc(void* ptr, size_t new_size);

	// Выделяет память и заполняет её нулями
	// num - количество элементов, size - размер каждого
	// Возвращает указатель на выделенную память или NULL при ошибке
	void* my_calloc(size_t num, size_t size);

	// ОТЛАДОЧНЫЕ ФУНКЦИИ

	// Включает/выключает режим отладки (1 - включить, 0 - выключить)
	void my_malloc_debug(int enable);

	// Выводит статистику работы аллокатора
	void my_malloc_stats(void);

	// Проверяет целостность всей кучи
	// Возвращает 1 если целостность не нарушена, 0 если есть ошибки
	int my_malloc_check(void);

	// Выводит содержимое списков свободных и занятых блоков
	void my_malloc_dump(void);

	// Устанавливает стратегию поиска (0 - Best-Fit, 1 - First-Fit)
	void my_malloc_set_strategy(int strategy);

	// Возвращает размер выделенного блока (для отладки)
	// Возвращает размер блока или 0 при ошибке
	size_t my_malloc_usable_size(void* ptr);

#ifdef __cplusplus
}
#endif

#endif /* MY_MALLOC_H */