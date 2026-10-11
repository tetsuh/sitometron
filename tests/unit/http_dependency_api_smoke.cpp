#include <array>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>

namespace {

int Check(bool condition, std::string_view description) {
  if (condition) {
    return 0;
  }
  std::cerr << "http_dependency_api_smoke: " << description << '\n';
  return 1;
}

int CheckBeastHeaderParsing() {
  namespace http = boost::beast::http;
  http::request_parser<http::string_body> parser;
  parser.eager(false);
  constexpr std::string_view request =
      "GET / HTTP/1.1\r\nHost: loopback\r\nContent-Length: 4\r\n\r\nbody";
  boost::system::error_code error;
  const auto consumed = parser.put(boost::asio::buffer(request.data(), request.size()), error);
  int result = 0;
  result |= Check(!error, "Beast request_parser accepted the header section");
  result |= Check(parser.is_header_done(), "Beast request_parser stopped after headers");
  result |= Check(!parser.is_done(), "Beast request_parser retained the body");
  result |= Check(consumed < request.size(), "Beast request_parser did not consume the body");
  return result;
}

int CheckAsioTotalCancellation() {
  namespace asio = boost::asio;
  using asio::ip::tcp;

  asio::io_context context;
  tcp::acceptor acceptor(context, tcp::endpoint(tcp::v4(), 0));
  tcp::socket client(context);
  client.connect(acceptor.local_endpoint());
  tcp::socket server(context);
  acceptor.accept(server);

  std::array<char, 1> input{};
  bool canceled = false;
  boost::system::error_code read_error;
  server.async_read_some(
      asio::buffer(input),
      asio::cancel_after(std::chrono::milliseconds(50), asio::cancellation_type::total,
                         [&](boost::system::error_code error, std::size_t) {
                           read_error = error;
                           canceled = error == asio::error::operation_aborted;
                         }));
  context.run();

  const char response = 'x';
  boost::system::error_code write_error;
  const auto written = server.write_some(asio::buffer(&response, 1), write_error);
  int result = 0;
  result |= Check(canceled, "Asio cancel_after used total cancellation for the read");
  result |= Check(read_error == asio::error::operation_aborted,
                  "Asio canceled read returned operation_aborted");
  result |= Check(!write_error && written == 1,
                  "Asio socket remained writable after total cancellation");
  std::cout << "http_dependency_api_smoke: cancel_after(total) read="
            << (canceled ? "PASS" : "FAIL")
            << " socket_write=" << ((!write_error && written == 1) ? "PASS" : "FAIL") << '\n';
  return result;
}

int CheckJsonKeyCallback() {
  constexpr std::array<char, 14> bytes{
      '{', '"', 'n', 'a', 'm', 'e', '"', ':', '"', 'j', 'o', 'b', '"', '}'};
  std::size_t key_count = 0;
  std::string key;
  const auto value = nlohmann::json::parse(
      bytes.data(), bytes.data() + bytes.size(),
      [&key_count, &key](int, nlohmann::json::parse_event_t event, nlohmann::json& item) {
        if (event == nlohmann::json::parse_event_t::key && item.is_string()) {
          ++key_count;
          key = item.get<std::string>();
        }
        return true;
      });
  int result = 0;
  result |= Check(value.is_object() && value.at("name") == "job",
                  "nlohmann-json parsed the explicit byte range");
  result |= Check(key_count == 1 && key == "name",
                  "nlohmann-json invoked the key callback");
  return result;
}

}  // namespace

int main() {
  int result = 0;
  const int beast_result = CheckBeastHeaderParsing();
  result |= beast_result;
  std::cout << "http_dependency_api_smoke: Beast header parser "
            << (beast_result == 0 ? "PASS" : "FAIL") << '\n';
  result |= CheckAsioTotalCancellation();
  const int json_result = CheckJsonKeyCallback();
  result |= json_result;
  std::cout << "http_dependency_api_smoke: JSON key callback "
            << (json_result == 0 ? "PASS" : "FAIL") << '\n';
  return result;
}
