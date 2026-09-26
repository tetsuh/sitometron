#include "http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <utility>

namespace sitometron::spike {
namespace {

constexpr std::size_t k_max_request_bytes = 64 * 1024;
// Per-recv socket timeout and a total deadline per request: a client that trickles one byte at a
// time must not hold the single server thread (and therefore Stop()) open indefinitely.
constexpr int k_receive_timeout_seconds = 2;
constexpr auto k_request_deadline = std::chrono::seconds(6);

std::string_view ReasonPhrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 202:
      return "Accepted";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 408:
      return "Request Timeout";
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Payload Too Large";
    case 503:
      return "Service Unavailable";
    default:
      return "Internal Server Error";
  }
}

bool SendAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto count = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

void Respond(int fd, const HttpResponse& response) {
  std::ostringstream head;
  head << "HTTP/1.1 " << response.status << ' ' << ReasonPhrase(response.status) << "\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << response.body.size() << "\r\n"
       << "Connection: close\r\n\r\n";
  (void)SendAll(fd, head.str() + response.body);
}

// Returns false when the request is malformed or too large; `status` carries the reason.
bool ReadRequest(int fd, HttpRequest& request, int& status) {
  const auto deadline = std::chrono::steady_clock::now() + k_request_deadline;
  auto expired = [&] {
    if (std::chrono::steady_clock::now() < deadline) return false;
    status = 408;
    return true;
  };
  std::string buffer;
  std::size_t header_end = std::string::npos;
  char chunk[4096];
  while (header_end == std::string::npos) {
    if (expired()) return false;
    const auto count = ::recv(fd, chunk, sizeof chunk, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    buffer.append(chunk, static_cast<std::size_t>(count));
    if (buffer.size() > k_max_request_bytes) {
      status = 413;
      return false;
    }
    header_end = buffer.find("\r\n\r\n");
  }
  std::istringstream lines(buffer.substr(0, header_end));
  std::string line;
  if (!std::getline(lines, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream request_line(line);
  std::string version;
  if (!(request_line >> request.method >> request.target >> version) ||
      version.rfind("HTTP/1.", 0) != 0) {
    status = 400;
    return false;
  }
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto value = line.substr(colon + 1);
    value.erase(0, value.find_first_not_of(' '));
    request.headers[name] = value;
  }
  std::size_t content_length = 0;
  if (const auto found = request.headers.find("content-length"); found != request.headers.end()) {
    // Strict decimal token: "25junk" must be rejected, not read as 25.
    const auto& text = found->second;
    const bool decimal =
        !text.empty() && text.size() <= 10 &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (!decimal) {
      status = 400;
      return false;
    }
    content_length = std::stoul(text);
  }
  if (content_length > k_max_request_bytes) {
    status = 413;
    return false;
  }
  request.body = buffer.substr(header_end + 4);
  while (request.body.size() < content_length) {
    if (expired()) return false;
    const auto count = ::recv(fd, chunk, sizeof chunk, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    request.body.append(chunk, static_cast<std::size_t>(count));
  }
  request.body.resize(content_length);
  return true;
}

}  // namespace

HttpServer::HttpServer(std::string host, unsigned short port, Handler handler)
    : host_(std::move(host)), port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(std::string& error) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    error = std::strerror(errno);
    return false;
  }
  const int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    error = "listen address must be an IPv4 literal";
    return false;
  }
  // The surface launches arbitrary processes without authentication: loopback only, by design.
  if ((ntohl(address.sin_addr.s_addr) >> 24) != 127U) {
    error = "listen address must be a loopback address (127.0.0.0/8)";
    return false;
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    error = std::strerror(errno);
    return false;
  }
  socklen_t length = sizeof address;
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0)
    port_ = ntohs(address.sin_port);
  thread_ = std::thread(&HttpServer::Serve, this);
  return true;
}

void HttpServer::Stop() {
  stopping_.store(true);
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void HttpServer::Serve() {
  while (!stopping_.load()) {
    pollfd waiter{listen_fd_, POLLIN, 0};
    const int ready = ::poll(&waiter, 1, 200);
    if (ready <= 0) continue;
    const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) continue;
    // A client that stops sending must not pin the single server thread (and shutdown) forever.
    const timeval receive_timeout{k_receive_timeout_seconds, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof receive_timeout);
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &receive_timeout, sizeof receive_timeout);
    HandleConnection(client);
    ::close(client);
  }
}

void HttpServer::HandleConnection(int client) {
  HttpRequest request;
  int status = 400;
  if (!ReadRequest(client, request, status)) {
    Respond(client, {status, R"({"error":"malformed request"})"});
    return;
  }
  HttpResponse response;
  try {
    response = handler_(request);
  } catch (const std::exception& e) {
    response = {500, std::string(R"({"error":")") + e.what() + "\"}"};
  }
  Respond(client, response);
}

}  // namespace sitometron::spike
