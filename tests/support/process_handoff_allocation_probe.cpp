#include "process_handoff_allocation_probe.hpp"

#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
thread_local unsigned handoff_depth = 0;
std::atomic_size_t handoff_allocations{0};
std::atomic_size_t handoff_deallocations{0};
void Count() noexcept {
  if (handoff_depth != 0) handoff_allocations.fetch_add(1, std::memory_order_relaxed);
}
void CountFree(void* memory) noexcept {
  if (memory != nullptr && handoff_depth != 0)
    handoff_deallocations.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace
void* operator new(std::size_t size) {
  Count();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept {
  CountFree(memory);
  std::free(memory);
}
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  Count();
  const auto boundary = static_cast<std::size_t>(alignment);
  if (size > std::numeric_limits<std::size_t>::max() - boundary + 1) throw std::bad_alloc();
  const auto rounded = ((size == 0 ? 1 : size) + boundary - 1) / boundary * boundary;
#ifdef _WIN32
  void* memory = ::_aligned_malloc(rounded, boundary);
#else
  void* memory = std::aligned_alloc(boundary, rounded);
#endif
  if (memory != nullptr) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept {
  CountFree(memory);
#ifdef _WIN32
  ::_aligned_free(memory);
#else
  std::free(memory);
#endif
}
void operator delete[](void* memory, std::align_val_t alignment) noexcept {
  ::operator delete(memory, alignment);
}
void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
  ::operator delete(memory, alignment);
}
void operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept {
  ::operator delete(memory, alignment);
}
namespace sitometron::test::process {
HandoffAllocationScope::HandoffAllocationScope() noexcept { ++handoff_depth; }
HandoffAllocationScope::~HandoffAllocationScope() { --handoff_depth; }
std::size_t HandoffAllocationCount() noexcept {
  return handoff_allocations.load(std::memory_order_relaxed);
}
std::size_t HandoffDeallocationCount() noexcept {
  return handoff_deallocations.load(std::memory_order_relaxed);
}
}  // namespace sitometron::test::process
