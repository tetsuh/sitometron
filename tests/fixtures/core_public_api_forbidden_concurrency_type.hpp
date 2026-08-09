#ifndef SITOMETRON_TESTS_FIXTURES_CORE_PUBLIC_API_FORBIDDEN_CONCURRENCY_TYPE_HPP_
#define SITOMETRON_TESTS_FIXTURES_CORE_PUBLIC_API_FORBIDDEN_CONCURRENCY_TYPE_HPP_
namespace sitometron::fixture {
struct LeakedConcurrency {
  std::atomic<int> atomic;
  std::thread thread;
  std::mutex mutex;
  std::condition_variable condition;
  std::future<int> future;
  std::unique_ptr<int> pointer;
};
}  // namespace sitometron::fixture
#endif
