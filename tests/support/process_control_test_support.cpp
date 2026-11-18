#include "process_control_test_support.hpp"

#ifdef _WIN32
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif
#include <exception>
#include <thread>

namespace sitometron::test::process {
// Control objects stay on their constructing fixture thread. On Linux use blocking
// native sockets, not BOOST_ASIO_DISABLE_THREADS: the HTTP executable also contains
// normal threaded Asio definitions, which must not differ across translation units.
struct Control::Impl {
  const std::thread::id owner = std::this_thread::get_id();
#ifdef _WIN32
  using Peer = boost::asio::ip::tcp::socket;
  boost::asio::io_context context;
  boost::asio::ip::tcp::acceptor acceptor{context, {boost::asio::ip::make_address("127.0.0.1"), 0}};
#else
  struct Peer {
    int descriptor = -1;
    ~Peer() {
      if (descriptor != -1) (void)::close(descriptor);
    }
  };
  Peer acceptor;
#endif
  std::vector<std::unique_ptr<Peer>> peers;
  std::vector<std::uint64_t> process_ids;
  Impl() {
#ifndef _WIN32
    acceptor.descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Require(acceptor.descriptor != -1, "cannot create test control listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    Require(::bind(acceptor.descriptor, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) == 0 &&
                ::listen(acceptor.descriptor, 8) == 0,
            "cannot bind ephemeral loopback test control listener");
#endif
    peers.reserve(8);
    process_ids.reserve(8);
  }
  void CheckThread() const noexcept {
    if (owner != std::this_thread::get_id()) std::terminate();
  }
  ~Impl() { CheckThread(); }
  std::string Port() const {
#ifdef _WIN32
    return std::to_string(acceptor.local_endpoint().port());
#else
    sockaddr_in address{};
    socklen_t size = sizeof(address);
    Require(::getsockname(acceptor.descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0,
            "cannot read test control listener port");
    return std::to_string(ntohs(address.sin_port));
#endif
  }
  std::unique_ptr<Peer> Accept() {
#ifdef _WIN32
    auto peer = std::make_unique<Peer>(context);
    acceptor.accept(*peer);
#else
    auto peer = std::make_unique<Peer>();
    do {
      peer->descriptor = ::accept4(acceptor.descriptor, nullptr, nullptr, SOCK_CLOEXEC);
    } while (peer->descriptor == -1 && errno == EINTR);
    Require(peer->descriptor != -1, "cannot accept test child control connection");
#endif
    return peer;
  }
  static void Read(Peer& peer, void* memory, std::size_t size) {
#ifdef _WIN32
    boost::asio::read(peer, boost::asio::buffer(memory, size));
#else
    auto* bytes = static_cast<char*>(memory);
    while (size != 0) {
      const auto count = ::recv(peer.descriptor, bytes, size, 0);
      if (count == -1 && errno == EINTR) continue;
      Require(count > 0, "test child control read failed or closed");
      const auto transferred = static_cast<std::size_t>(count);
      bytes += transferred;
      size -= transferred;
    }
#endif
  }
  static bool Write(Peer& peer, const void* memory, std::size_t size) {
#ifdef _WIN32
    boost::system::error_code error;
    (void)boost::asio::write(peer, boost::asio::buffer(memory, size), error);
    return !error;
#else
    const auto* bytes = static_cast<const char*>(memory);
    while (size != 0) {
      const auto count = ::send(peer.descriptor, bytes, size, MSG_NOSIGNAL);
      if (count == -1 && errno == EINTR) continue;
      if (count <= 0) return false;
      const auto transferred = static_cast<std::size_t>(count);
      bytes += transferred;
      size -= transferred;
    }
    return true;
#endif
  }
};
Control::Control() : impl_(std::make_unique<Impl>()) {}
Control::~Control() = default;
std::string Control::port() const {
  impl_->CheckThread();
  return impl_->Port();
}
std::size_t Control::Ready() {
  impl_->CheckThread();
  auto peer = impl_->Accept();
  char ready = 0;
  Impl::Read(*peer, &ready, 1);
  std::uint64_t process_id = 0;
  Impl::Read(*peer, &process_id, sizeof(process_id));
  Require(ready == 'R' && process_id != 0,
          "child did not acknowledge initialized report/stop handler and process identity");
  impl_->process_ids.push_back(process_id);
  impl_->peers.push_back(std::move(peer));
  return impl_->peers.size() - 1;
}
std::uint64_t Control::ProcessId(std::size_t index) const {
  impl_->CheckThread();
  return impl_->process_ids.at(index);
}
void Control::Ping(std::size_t index) {
  impl_->CheckThread();
  const char command = 'P';
  Require(Impl::Write(*impl_->peers.at(index), &command, 1),
          "test child control write failed or closed");
  char reply = 0;
  Impl::Read(*impl_->peers.at(index), &reply, 1);
  Require(reply == command, "live child did not acknowledge control ping");
}
void Control::Exit(std::size_t index) {
  impl_->CheckThread();
  const char command = 'X';
  Require(Impl::Write(*impl_->peers.at(index), &command, 1),
          "test child control write failed or closed");
}
void Control::TermObserved(std::size_t index) {
  impl_->CheckThread();
  char observed = 0;
  Impl::Read(*impl_->peers.at(index), &observed, 1);
  Require(observed == 'T', "Linux cooperative stop did not reach initialized child");
}
void Control::ExitAll() noexcept {
  impl_->CheckThread();
  for (auto& peer : impl_->peers) {
    const char command = 'X';
    // The runner already forced/reaped its owned process; cleanup is best effort.
    (void)Impl::Write(*peer, &command, 1);
  }
}
}  // namespace sitometron::test::process
