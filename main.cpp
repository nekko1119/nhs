#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
    socket() = default;
    explicit socket(int port) : port_{port} {
    }

    ~socket() {
      close();
    }

    void close() noexcept {
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
      int val = 1;
      ::ioctl(sock_, FIONBIO, &val);
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
      while ((accepted_sock_ = ::accept(sock_, nullptr, nullptr)) <= 0) {
        if (errno == EAGAIN) {
          continue;
        }
        throw std::system_error{errno, std::generic_category(), "accept"};
      }
    }

    auto recv(char* buffer, std::size_t size) {
      if (accepted_sock_ == 0) {
        throw std::logic_error{"socket is not accepted"};
      }
      auto const recv_size = ::recv(accepted_sock_, buffer, size, 0);
      if (recv_size < 0) {
        if (errno == EAGAIN) {
          return recv_size;
        }
        throw std::system_error{errno, std::generic_category(), "recv"};
      }
      return recv_size;
    }

    void send(std::string_view buf) {
      if (::send(accepted_sock_, buf.data(), buf.size(), 0) < 0) {
        throw std::system_error{errno, std::generic_category()};
      }
    }
  };

  enum class parse_state {
    method,
    path,
    query,
    protocol,
    http_version,
    header_key,
    header_value,
    body,
    cr,
    crlf,
    crlfcr,
    done,
    invalid,
  };

  class request {
    friend class server;
    std::unordered_map<std::string, std::string> headers_;
    std::string method_;
    std::string original_url_;
    std::string path_;
    std::string protocol_;
    std::string hostname_;
    std::string body_;
    std::string http_version_;
    parse_state state_ = parse_state::method;

    void parse_and_build(char const* buffer, ::ssize_t recv_size) {
      std::pair<std::string, std::string> header_buffer;
      for (auto i = 0; i < recv_size; ++i) {
        auto const it = buffer[i];

        switch (state_) {
          case parse_state::method: {
            if (it == ' ') {
              state_ = parse_state::path;
              break;
            }
            method_.push_back(it);
            break;
          }
          case parse_state::path: {
            if (it == ' ') {
              state_ = parse_state::protocol;
              original_url_ = path_ /* + query */;
              break;
            }
            if (it == '?') {
              state_ = parse_state::query;
              break;
            }
            path_.push_back(it);
            break;
          }
          case parse_state::query: {
            if (it == ' ') {
              state_ = parse_state::protocol;
              original_url_ = path_ /* + query */;
              break;
            }
            // TODO: build query
            break;
          }
          case parse_state::protocol: {
            if (it == '/') {
              state_ = parse_state::http_version;
              break;
            }
            protocol_.push_back(it);
            break;
          }
          case parse_state::http_version: {
            if (it == '\r') {
              state_ = parse_state::cr;
              break;
            }
            http_version_.push_back(it);
            break;
          }
          case parse_state::header_key: {
            if (it == ':') {
              state_ = parse_state::header_value;
              break;
            }
            header_buffer.first.push_back(std::tolower(it));
            break;
          }
          case parse_state::header_value: {
            // skip when it is first space
            if (it == ' ' && header_buffer.second.empty()) {
              break;
            }
            if (it == '\r') {
              headers_.insert(header_buffer);
              header_buffer.first.clear();
              header_buffer.second.clear();
              state_ = parse_state::cr;
              break;
            }
            header_buffer.second.push_back(it);
            break;
          }
          case parse_state::body: {
            body_.push_back(it);
            // TODO: if body size is equal to content-length, state
            break;
          }
          case parse_state::cr: {
            if (it == '\n') {
              state_ = parse_state::crlf;
              break;
            }
            state_ = parse_state::invalid;
            break;
          }
          case parse_state::crlf: {
            if (it == '\r') {
              state_ = parse_state::crlfcr;
              break;
            }
            header_buffer.first.push_back(std::tolower(it));
            state_ = parse_state::header_key;
            break;
          }
          case parse_state::crlfcr: {
            if (it == '\n') {
              state_ = parse_state::done;
              // TODO: if content-length exists, state transits body. otherwise, state transits
              // done.
              break;
            }
            state_ = parse_state::invalid;
            break;
          }
          case parse_state::done: {
            state_ = parse_state::done;
            break;
          }
          default:
            break;
        }
      }
    }

  public:
    request() = default;

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
      std::string lower_header;
      lower_header.reserve(header.size());
      std::transform(header.begin(), header.end(), lower_header.begin(),
                     [](char c) { return std::tolower(c); });
      headers_.emplace(lower_header, value);
    }

    std::string const& get_header(std::string const& header) const {
      std::string lower_header;
      lower_header.reserve(header.size());
      std::transform(header.begin(), header.end(), lower_header.begin(),
                     [](char c) { return std::tolower(c); });
      return headers_.at(lower_header);
    }

    response& status(int status) {
      status_ = status;
      return *this;
    }

    response& status_message(std::string_view message) {
      status_message_ = message;
      return *this;
    }

    void send(std::string_view body) {
      auto const message =
          !status_message_.empty() ? status_message_ : default_status_messages[status_];
      std::ostringstream oss;
      oss << request_->protocol() << "/" << request_->http_version() << " " << status_ << " "
          << message << "\r\n";
      for (auto const& [header, value] : headers_) {
        oss << header << ": " << value << "\r\n";
      }
      if (!body.empty()) {
        oss << "Content-Length: " << body.size() << "\r\n";
        // TODO: deduce content type from body
        oss << "Content-Type: text/html\r\n";
      }
      oss << "Connection: Keep-Alive\r\n";
      oss << "\r\n";
      if (!body.empty()) {
        oss << body;
      }
      sock_->send(oss.str());
    }
  };

  // TODO
  std::unordered_map<int, std::string const> response::default_status_messages = {
      {200, "OK"},
      {400, "Bad Request"},
      {404, "Not Found"}};

  class server {
    std::thread thread_;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::function<void(request const&, response&)>>>
        callbacks_;

  public:
    ~server() noexcept {
      try {
        if (thread_.joinable()) {
          thread_.join();
        }
      } catch (...) {
      }
    }

    template <typename Callback>
    server& get(std::string const& path, Callback&& callback) {
      callbacks_["GET"][path] = std::forward<Callback>(callback);
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
            request req;
            while (true) {
              char buffer[256] = {0};
              auto const recv_size = sock.recv(buffer, sizeof(buffer) - 1);
              if (recv_size == 0) {
                break;
              }
              if (recv_size < 0) {
                continue;
              }
              req.parse_and_build(buffer, recv_size);
              if (req.state_ == parse_state::done) {
                // TODO: get hostname from Host header
                req.hostname_ = "localhost";
                break;
              }
            }
            auto const& target_method_callbacks = callbacks_[req.method()];
            response res{req, sock};
            for (auto const& [path, cb] : target_method_callbacks) {
              if (std::regex_match(req.path(), std::regex{path})) {
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
    char const* placeholder = "{}";
    static int count = 0;
    auto copy = html_str;
    auto const it = std::search(copy.begin(), copy.end(), placeholder, placeholder + 2);
    if (it != html_str.end()) {
      copy.replace(it, it + 2, std::to_string(count));
      ++count;
    }
    res.send(copy);
  });
  serve.listen(3000);
  std::cout << "start server...\n";
}
