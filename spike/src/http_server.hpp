#ifndef SITOMETRON_SPIKE_HTTP_SERVER_HPP_
#define SITOMETRON_SPIKE_HTTP_SERVER_HPP_

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace sitometron::spike {

struct HttpRequest {
  std::string method;
  std::string target;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string body;  // JSON text
};

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
