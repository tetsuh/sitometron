#ifndef SITOMETRON_SPIKE_HTTP_SERVER_HPP_
#define SITOMETRON_SPIKE_HTTP_SERVER_HPP_

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sitometron::spike {

struct HttpRequest {
  std::string method;
  std::string target;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string body;                                          // JSON text
  std::vector<std::pair<std::string, std::string>> headers;  // extra headers, e.g. Location
};

// The External REST v1 error envelope (ADR-0008 Section 5) as JSON text. `message` must not carry
// a path, a raw error text, a Journal record, or a secret.
[[nodiscard]] std::string ErrorEnvelope(std::string_view domain, std::string_view code,
                                        std::string_view message);

// Blocking loopback HTTP/1.1 listener over POSIX sockets: one connection at a time, no
// keep-alive, bounded request size. Enough for curl; not a contract and not the Phase 1 surface.
class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  HttpServer(std::string host, unsigned short port, Handler handler);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  [[nodiscard]] bool Start(std::string& error);
  void Stop();
  [[nodiscard]] unsigned short port() const noexcept { return port_; }

 private:
  void Serve();
  void HandleConnection(int client);

  std::string host_;
  unsigned short port_;
  Handler handler_;
  int listen_fd_ = -1;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

}  // namespace sitometron::spike

#endif  // SITOMETRON_SPIKE_HTTP_SERVER_HPP_
