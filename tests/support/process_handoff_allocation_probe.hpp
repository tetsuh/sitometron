#ifndef SITOMETRON_TEST_PROCESS_HANDOFF_ALLOCATION_PROBE_HPP_
#define SITOMETRON_TEST_PROCESS_HANDOFF_ALLOCATION_PROBE_HPP_
#include <cstddef>
#include <new>
// Our replacement is defined in the probe unit even when a compiler does not
// enable implicit sized deallocation. Declare it for the explicit coverage call.
void operator delete[](void*, std::size_t, std::align_val_t) noexcept;
namespace sitometron::test::process {
class HandoffAllocationScope final {
 public:
  HandoffAllocationScope() noexcept;
  ~HandoffAllocationScope();
  HandoffAllocationScope(const HandoffAllocationScope&) = delete;
  HandoffAllocationScope& operator=(const HandoffAllocationScope&) = delete;
};
std::size_t HandoffAllocationCount() noexcept;
std::size_t HandoffDeallocationCount() noexcept;
}  // namespace sitometron::test::process
#endif
