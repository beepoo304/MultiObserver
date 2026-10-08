#pragma once
#include <deque>
#include <vector>
#include <cstring>
#include "FreeRTOS.h"
struct TestQueue {
  size_t capacity, itemSize;
  std::deque<std::vector<char>> entries;
};
using QueueHandle_t = TestQueue*;
inline QueueHandle_t xQueueCreate(size_t count, size_t size) {
  return new TestQueue{count, size, {}};
}
inline int xQueueSend(QueueHandle_t q, const void* item, int) {
  if (q->entries.size() == q->capacity) return pdFALSE;
  q->entries.emplace_back(static_cast<const char*>(item), static_cast<const char*>(item) + q->itemSize);
  return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q, void* item, int) {
  if (q->entries.empty()) return pdFALSE;
  std::memcpy(item, q->entries.front().data(), q->itemSize);
  q->entries.pop_front();
  return pdTRUE;
}
inline void vQueueDelete(QueueHandle_t q) { delete q; }
