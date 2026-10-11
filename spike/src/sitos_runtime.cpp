#include "sitos_runtime.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#ifdef SITOMETRON_SPIKE_SITOS
#include <memory>
#include <optional>
#include <sitos/sitos.hpp>
#endif

namespace sitometron::spike {

#ifdef SITOMETRON_SPIKE_SITOS
namespace {

template <typename T>
std::string Failure(std::string_view operation, const sitos::Result<T>& result) {
  std::string detail(operation);
  detail.append(" failed (status=")
      .append(std::to_string(static_cast<int>(result.StatusCode())))
      .append(")");
  if (!result.Message().empty()) {
    detail.append(": ").append(result.Message());
  }
  return detail;
}

}  // namespace

struct SitosRuntime::Impl {
  std::shared_ptr<sitos::Transport> transport;
  std::shared_ptr<sitos::StorageEngine> engine;
  std::unique_ptr<sitos::StorageNode> node;
  std::optional<sitos::ParamStore> seed_store;
  std::mutex mutex;
  std::set<std::string, std::less<>> active_sessions;
};
#endif

SitosRuntime::SitosRuntime(std::string prefix) : prefix_(std::move(prefix)) {
#ifdef SITOMETRON_SPIKE_SITOS
  enabled_ = true;
  ready_ = false;
  impl_ = std::make_unique<Impl>();

  const std::string node_zenoh_config =
      R"({"mode":"peer","listen":{"endpoints":["tcp/127.0.0.1:27400"]}})";
  config_ =
      R"({"mode":"client","connect":{"endpoints":["tcp/127.0.0.1:27400"]},"scouting":{"multicast":{"enabled":false}}})";
  auto opened_transport = sitos::OpenZenohTransport(node_zenoh_config);
  if (!opened_transport.IsOk()) {
    error_ = Failure("OpenZenohTransport", opened_transport);
    return;
  }
  impl_->transport = std::shared_ptr<sitos::Transport>(std::move(opened_transport).Value());
  impl_->engine = std::make_shared<sitos::InMemoryEngine>();
  impl_->node = std::make_unique<sitos::StorageNode>(*impl_->transport);

  sitos::StorageNodeConfig node_config;
  node_config.prefix = prefix_;
  node_config.log_sink = nullptr;
  node_config.durable_buffer_engine_factory =
      [](std::string_view) -> sitos::Result<std::unique_ptr<sitos::StorageEngine>> {
    return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
        std::make_unique<sitos::InMemoryEngine>());
  };
  auto started = impl_->node->Start(impl_->engine, node_config);
  if (!started.IsOk()) {
    error_ = Failure("StorageNode::Start", started);
    return;
  }

  sitos::ClientConfig client_config;
  client_config.prefix = prefix_;
  client_config.query_timeout = std::chrono::seconds(5);
  client_config.log_sink = nullptr;
  auto opened_store = sitos::ParamStore::Open(impl_->transport, client_config);
  if (!opened_store.IsOk()) {
    error_ = Failure("ParamStore::Open", opened_store);
    return;
  }
  impl_->seed_store.emplace(std::move(opened_store).Value());
  auto seeded = impl_->seed_store->Put("base", "input", std::string("spike-input"));
  if (!seeded.IsOk()) {
    error_ = Failure("ParamStore::Put(base/input)", seeded);
    return;
  }
  auto seeded_read = impl_->seed_store->Get<std::string>("base", "input");
  if (!seeded_read.IsOk() || seeded_read.Value() != "spike-input") {
    error_ = seeded_read.IsOk() ? "seeded base parameter did not round-trip"
                                : Failure("ParamStore::Get(base/input)", seeded_read);
    return;
  }

  ready_ = true;
  std::cout << "SITOS_RUNTIME_READY prefix=" << prefix_ << '\n' << std::flush;
#else
  (void)prefix_;
#endif
}

SitosRuntime::~SitosRuntime() {
  CloseAll();
#ifdef SITOMETRON_SPIKE_SITOS
  // The owner calls CloseAll before this destructor while the node is still running. If
  // construction failed, the partially initialized node and client objects are released by Impl in
  // dependency order here.
  impl_.reset();
#endif
}

bool SitosRuntime::CreateSession(const std::string& session_id, std::string& error) {
  if (!enabled_) return true;
  if (!ready_) {
    error = error_.empty() ? "Sitos runtime is not ready" : error_;
    return false;
  }
#ifdef SITOMETRON_SPIKE_SITOS
  sitos::SessionOptions options;
  options.durable_buffers = true;
  options.ephemeral_buffers = true;
  auto created = impl_->node->CreateSession(session_id, options);
  if (!created.IsOk()) {
    error = Failure("StorageNode::CreateSession", created);
    return false;
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->active_sessions.insert(session_id);
  }
  return true;
#else
  (void)session_id;
  (void)error;
  return true;
#endif
}

bool SitosRuntime::CloseSession(const std::string& session_id, std::string& error) {
  if (!enabled_) return true;
#ifdef SITOMETRON_SPIKE_SITOS
  if (!ready_) return true;
  auto closed = impl_->node->CloseSession(session_id);
  if (!closed.IsOk()) {
    error = Failure("StorageNode::CloseSession", closed);
    return false;
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->active_sessions.erase(session_id);
  }
  std::cout << "SITOS_SESSION_CLOSED " << session_id << '\n' << std::flush;
  return true;
#else
  (void)session_id;
  (void)error;
  return true;
#endif
}

void SitosRuntime::CloseAll() {
#ifdef SITOMETRON_SPIKE_SITOS
  if (!impl_ || !ready_) return;
  std::vector<std::string> sessions;
  {
    std::lock_guard lock(impl_->mutex);
    sessions.assign(impl_->active_sessions.begin(), impl_->active_sessions.end());
  }
  for (const auto& session_id : sessions) {
    std::string error;
    if (!CloseSession(session_id, error))
      std::cerr << "SITOS_SESSION_CLOSE_ERROR " << session_id << ": " << error << '\n';
  }
#endif
}

}  // namespace sitometron::spike
