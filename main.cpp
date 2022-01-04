#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nek {
  class socket {
    int sock_ = 0;
    int accepted_sock_ = 0;
    int port_ = 80;

  public:
    static constexpr int buffer_size = 256;
    socket() = default;
    explicit socket(int port) : port_{port} {
    }

    ~socket() {
      if (accepted_sock_ != 0) {
        ::close(accepted_sock_);
      }
      if (sock_ != 0) {
        ::close(sock_);
      }
    }

    int port() const noexcept {
      return port_;
    }

    void connect() {
      if ((sock_ = ::socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        throw std::system_error{errno, std::generic_category(), "socket"};
      }
      sockaddr_in addr;
      std::memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
      addr.sin_port = htons(port_);
      if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::system_error{errno, std::generic_category(), "bind"};
      }
    }

    void listen() {
      if (sock_ == 0) {
        throw std::logic_error{"socket is not created"};
      }
      if (::listen(sock_, 5) != 0) {
        throw std::system_error{errno, std::generic_category(), "listen"};
      }
    }

    void accept() {
      if (sock_ == 0) {
        throw std::logic_error{"socket is not created"};
      }
      if ((accepted_sock_ = ::accept(sock_, nullptr, nullptr)) < 0) {
        throw std::system_error{errno, std::generic_category(), "accept"};
      }
    }

    std::vector<char> recv() {
      if (accepted_sock_ == 0) {
        throw std::logic_error{"socket is not accepted"};
      }

      std::vector<char> response;
      response.reserve(buffer_size);
      while (true) {
        char buf[buffer_size] = {0};
        auto const recv_size = ::recv(accepted_sock_, buf, sizeof(buf) - 1, 0);
        if (recv_size == 0) {
          break;
        }
        if (recv_size < 0) {
          throw std::system_error{errno, std::generic_category(), "recv"};
        }
        response.insert(response.end(), buf, buf + recv_size);
        auto const last = response.size();
        if (response[last - 1] == '\n' && response[last - 2] == '\r' &&
            response[last - 3] == '\n' && response[last - 4] == '\r') {
          break;
        }
      };
      response.shrink_to_fit();
      return response;
    }

    void send(std::string const& buf) {
      if (::send(accepted_sock_, buf.c_str(), buf.size(), 0) < 0) {
        throw std::system_error{errno, std::generic_category()};
      }
    }
  };

  class request {
    static constexpr char delimiter[] = "\r\n\r\n";
    static constexpr char newline[] = "\r\n";
    std::vector<char> raw_request_;
    std::unordered_map<std::string, std::string> headers_;
    std::string method_;
    std::string original_url_;
    std::string path_;
    std::string protocol_;
    std::string hostname_;
    std::string body_;
    std::string http_version_;

    void parse() {
      if (raw_request_.empty()) {
        return;
      }
      auto const headers_end = std::search(raw_request_.begin(), raw_request_.end(), delimiter,
                                           delimiter + std::size(delimiter) - 1);
      if (headers_end == raw_request_.end()) {
        throw std::runtime_error{"there is not delimiter in the raw request"};
      }
      // body
      body_ = std::string{headers_end + std::size(delimiter) - 1, raw_request_.end()};

      // parse request line
      auto header_end =
          std::search(raw_request_.begin(), headers_end, newline, newline + std::size(newline) - 1);
      std::string request_line{raw_request_.begin(), header_end};

      // parse method
      auto const method_end = std::find(request_line.begin(), request_line.end(), ' ');
      method_ = std::string{request_line.begin(), method_end};
      std::transform(method_.begin(), method_.end(), method_.begin(), ::tolower);

      // parse original_url
      auto const original_url_end = std::find(method_end + 1, request_line.end(), ' ');
      original_url_ = std::string{method_end + 1, original_url_end};

      // parse path
      auto const path_end = std::find(original_url_.begin(), original_url_.end(), '?');
      path_ = std::string{original_url_.begin(), path_end};

      // parse protocol
      auto const protocol_end = std::find(original_url_end + 1, request_line.end(), '/');
      protocol_ = std::string{original_url_end + 1, protocol_end};
      std::transform(protocol_.begin(), protocol_.end(), protocol_.begin(), ::tolower);

      // parse http version
      http_version_ = std::string{protocol_end + 1, request_line.end()};

      // parse headers
      auto header_begin = header_end + 2;
      while (header_begin < headers_end) {
        header_end =
            std::search(header_begin, headers_end, newline, newline + std::size(newline) - 1);
        auto const key_end = std::find(header_begin, header_end, ':');
        if (key_end == header_end) {
          throw std::runtime_error{"key end not found"};
        }
        auto const value_begin = key_end + 2;
        headers_.emplace(std::piecewise_construct, std::forward_as_tuple(header_begin, key_end),
                         std::forward_as_tuple(value_begin, header_end));
        header_begin = header_end + 2;
      }
      auto const& host = headers_["Host"];
      auto const port_begin = std::find(host.begin(), host.end(), ':');
      hostname_ = std::string{host.begin(), port_begin};
    }

  public:
    explicit request(std::vector<char> raw_request) : raw_request_{std::move(raw_request)} {
      parse();
    }

    std::unordered_map<std::string, std::string> const& headers() const noexcept {
      return headers_;
    }

    std::string const& body() const noexcept {
      return body_;
    }

    std::string const& hostname() const noexcept {
      return hostname_;
    }

    std::string const& method() const noexcept {
      return method_;
    }

    std::string const& original_url() const noexcept {
      return original_url_;
    }

    std::string const& path() const noexcept {
      return path_;
    }

    std::string const& protocol() const noexcept {
      return protocol_;
    }

    std::string const& http_version() const noexcept {
      return http_version_;
    }
  };

  class response {
    request const* request_ = nullptr;
    socket* sock_ = nullptr;
    std::unordered_map<std::string, std::string> headers_;
    int status_ = 200;
    std::string status_message_ = "";

    static std::unordered_map<int, std::string const> default_status_messages;

  public:
    response(request const& request, socket& sock) : request_{&request}, sock_{&sock} {
    }

    std::unordered_map<std::string, std::string>& headers() noexcept {
      return headers_;
    }

    void set_header(std::string const& header, std::string const& value) {
      headers_[header] = value;
    }

    std::string const& get_header(std::string const& header) const {
      return headers_.at(header);
    }

    response& status(int status) {
      status_ = status;
      return *this;
    }

    response& status_message(std::string message) {
      status_message_ = std::move(message);
      return *this;
    }

    void send(std::string const& body) {
      auto const message =
          !status_message_.empty() ? status_message_ : default_status_messages[status_];
      std::ostringstream oss;
      oss << request_->protocol() << "/" << request_->http_version() << " " << status_ << " "
          << message << "\r\n";
      for (auto const& [header, value] : headers_) {
        oss << header << ": " << value << "\r\n";
      }
      oss << "Content-Length: " << body.size() << "\r\n";
      oss << "Content-Type: text/html\r\n";
      oss << "Connection: Keep-Alive\r\n";
      oss << "\r\n";
      oss << body;
      sock_->send(oss.str());
    }
  };

  // TODO
  std::unordered_map<int, std::string const> response::default_status_messages = {
      {200, "OK"},
      {400, "Bad Request"},
      {404, "Not Found"}};

  request parse(std::vector<char> raw_response) {
    request req{std::move(raw_response)};
    return req;
  }

  class server {
    std::thread thread_;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::function<void(request const&, response&)>>>
        callbacks_;

  public:
    ~server() {
      if (thread_.joinable()) {
        thread_.join();
      }
    }

    template <typename Callback>
    server& get(std::filesystem::path const& path, Callback&& callback) {
      callbacks_["get"][path.lexically_normal()] = std::forward<Callback>(callback);
      return *this;
    }

    void listen(int port) {
      thread_ = std::thread([&] {
        try {
          socket sock{port};
          sock.connect();
          sock.listen();
          while (true) {
            sock.accept();
            auto const raw_response = sock.recv();
            auto const req = parse(raw_response);
            auto const& target_method_callbacks = callbacks_[req.method()];
            response res{req, sock};
            for (auto const& [path, cb] : target_method_callbacks) {
              namespace fs = std::filesystem;
              if (fs::path fspath = path; path == req.path()) {
                cb(req, res);
              }
            }
          }
        } catch (std::exception const& ex) {
          std::cerr << ex.what() << std::endl;
        } catch (...) {
          std::cerr << "unknown error" << std::endl;
        }
      });
    }
  };
}

struct parsed_command {
  std::string path;
};

parsed_command parse_command(int argc, char** argv) {
  // location of execution file is is difference when debugging by F5 and executing by cmake.
  // so, this server allows to recieve the relative path of index.html.
  static ::option longopts[] = {{"path", optional_argument, nullptr, 'p'}};
  parsed_command command;
  int opt{};
  int longindex{};
  while ((opt = ::getopt_long(argc, argv, "p", longopts, &longindex)) != -1) {
    switch (opt) {
      case 'p':
        command.path = ::optarg != nullptr ? ::optarg : "";
        break;
      default:
        break;
    }
  };
  return command;
}

int main(int argc, char** argv) {
  auto const command = parse_command(argc, argv);
  std::filesystem::path index_html{"./index.html"};
  std::ifstream ifs{(command.path / index_html).lexically_normal()};
  std::string html_str{std::istreambuf_iterator<char>{ifs}, std::istreambuf_iterator<char>{}};
  nek::server serve;
  serve.get("/", [&html_str](nek::request const& req, nek::response& res) {
    std::cout << req.method() << " " << req.path() << "\n";
    res.send(html_str);
  });
  serve.listen(3000);
  std::cout << "start server...\n";
}
