#ifndef SITOMETRON_TEST_PROCESS_HANDOFF_ALLOCATION_PROBE_HPP_
#define SITOMETRON_TEST_PROCESS_HANDOFF_ALLOCATION_PROBE_HPP_
#include <cstddef>
namespace sitometron::test::process {
class HandoffAllocationScope final {
 public:
  HandoffAllocationScope() noexcept;
  ~HandoffAllocationScope();
  HandoffAllocationScope(const HandoffAllocationScope&) = delete;
  HandoffAllocationScope& operator=(const HandoffAllocationScope&) = delete;
};
std::size_t HandoffAllocationCount() noexcept;
}  // namespace sitometron::test::process
#endif
