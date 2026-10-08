#pragma once
#include <cstddef>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
inline size_t fakeHeap = 120 * 1024;
inline size_t heap_caps_get_free_size(int) { return fakeHeap; }
inline size_t heap_caps_get_largest_free_block(int) { return fakeHeap; }
