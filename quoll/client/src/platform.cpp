//   ___              _ _
//  / _ \ _   _  ___ | | |
// | | | | | | |/ _ \| | |
// | |_| | |_| | (_) | | |
//  \__\_\\__,_|\___/|_|_|
//
// BSD 3-Clause License
//
// Copyright (c) 2026, Thomas Gourgues (thomas.gourgues@gmail.com)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <quoll/async/current_executor.hpp>
#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/require_service.hpp>
#include <quoll/client/detail/client_backend.hpp>
#include <quoll/client/detail/url.hpp>
#include <quoll/http/detail/check.hpp>
#include <quoll/http/detail/write_whole_body.hpp>
#include <quoll/http/error.hpp>
#include <quoll/http/platform_ref.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::http::detail {

namespace {

struct platform_state {
  // Scheme and authority, empty for what is relative to the page's origin, `.` for what is relative to the page.
  std::string origin;
  std::string prefix;
  client_options options;
};

// What send() and stream() ask of a request before anything runs: as a connection does, and what only a URL has to
// fear.
void make_ready(platform_state const &state, http::request &request) {
  check_origin_form(request.target);

  using enum http::verb;

  // fetch() would reject them, as a TypeError that reads as a network failure.
  if (((request.method == get || request.method == head) && (request.body_writer || !request.body.empty())) ||
      request.method == connect || request.method == trace) {
    throw std::invalid_argument{"quoll::http::client: what a browser's fetch refuses to send"};
  }

  request.target.insert(0, state.prefix);

  // Alone, a target is the URL: `//host/path` or `/\\host/path` would leave the page's origin. What a URL parser drops,
  // a tab or a line break, is refused by check_target() below.
  if (state.origin.empty() && (request.target.starts_with("//") || request.target.starts_with("/\\"))) {
    throw std::invalid_argument{"quoll::http::client: a target that could leave the page's origin"};
  }

  // What fetch does not send, the fields framing a message among them, it cannot get wrong.
  if (request.body_writer && !request.body.empty()) {
    throw std::invalid_argument{"quoll::http::client: a request with a body_writer and a body"};
  }

  http::detail::check_target(request.target);

  for (auto const &entry : request.fields) {
    http::detail::check_field(entry);
  }
}

// What fetch has no limit for, in what a response's head holds: its status text counts, as for the parser.
void check_head(http::streamed_response const &head, http::parse_limits const &limits) {
  std::size_t count = 0;
  std::size_t size = head.reason.size();

  for (auto const &entry : head.fields) {
    ++count;
    size += entry.name.size() + entry.value.size();
  }

  if (count > limits.max_field_count || size > limits.max_header_size) {
    throw std::system_error{http::error::header_too_large, "quoll::http::client: cannot read the response"};
  }
}

// Through the one with_optional_timeout() that a page has to hold: each kind of result would be one more of its code.
[[nodiscard]] async::task<>
read_piece(http::response_body &body QUOLL_LIFETIMEBOUND, std::optional<std::string_view> &piece QUOLL_LIFETIMEBOUND) {
  piece = co_await body.read();
}

[[nodiscard]] async::task<> open_body(
    http::platform_ref platform, std::string url, http::request request,
    http::streamed_response &answer QUOLL_LIFETIMEBOUND
) {
  answer = co_await platform.stream(std::move(url), std::move(request));
}

// A body the platform reads in pieces, with what the client's options ask of it. A failure ends it, as one ends a
// connection: the fetch goes, and the next reads fail the same way, as a piece missing would otherwise read as nothing.
//
// Thread safety: none, as for the response_body it is read through.
class platform_body {
public:
  platform_body(http::response_body body, client_options const &options) noexcept
      : body_(std::move(body)), timeout_(options.response_timeout), left_(options.limits.max_streamed_body_size) {}

  [[nodiscard]] async::task<std::optional<std::string_view>> read() QUOLL_LIFETIMEBOUND {
    if (failure_) {
      std::rethrow_exception(failure_);
    }

    try {
      std::optional<std::string_view> piece;

      co_await async::detail::with_optional_timeout(timeout_, read_piece(body_, piece));

      if (piece) {
        if (piece->size() > left_) {
          throw std::system_error{http::error::body_too_large, "quoll::http::client: cannot read the response"};
        }

        left_ -= piece->size();
      }

      co_return piece;
    } catch (...) {
      failure_ = std::current_exception();
      body_ = http::response_body{};
      throw;
    }
  }

private:
  http::response_body body_;
  std::chrono::nanoseconds timeout_;
  std::uint64_t left_;
  std::exception_ptr failure_;
};

// The platform of the executor, and a request ready for it. The time a writer takes is its own: this comes before any
// timeout.
[[nodiscard]] async::task<http::platform_ref> platform_for(http::request &request QUOLL_LIFETIMEBOUND) {
  auto const platform = async::detail::require_service<http::platform_ref>(
      co_await async::current_executor(), "quoll::http::client", "fetch"
  );

  if (request.body_writer) {
    co_await http::detail::write_whole_body(request);
  }

  co_return platform;
}

// A whole body, of which the platform holds no more than its limit allows: past it, the fetch goes.
[[nodiscard]] async::task<> gather(
    http::platform_ref platform, std::string url, http::request request, http::parse_limits limits,
    http::response &answer QUOLL_LIFETIMEBOUND
) {
  auto streamed = co_await platform.stream(std::move(url), std::move(request));

  check_head(streamed, limits);

  while (auto const piece = co_await streamed.body.read()) {
    if (piece->size() > limits.max_body_size - answer.body.size()) {
      throw std::system_error{http::error::body_too_large, "quoll::http::client: cannot read the response"};
    }

    answer.body.append(*piece);
  }

  answer.status = streamed.status;
  answer.reason = std::move(streamed.reason);
  answer.version = streamed.version;
  answer.fields = std::move(streamed.fields);
}

[[nodiscard]] async::task<http::response>
send_on_platform(std::shared_ptr<platform_state> state, http::request request) {
  auto const platform = co_await platform_for(request);
  auto url = state->origin + request.target;

  http::response answer;

  co_await async::detail::with_optional_timeout(
      state->options.response_timeout,
      gather(platform, std::move(url), std::move(request), state->options.limits, answer)
  );
  co_return answer;
}

[[nodiscard]] async::task<http::streamed_response>
stream_on_platform(std::shared_ptr<platform_state> state, http::request request) {
  auto const platform = co_await platform_for(request);
  auto url = state->origin + request.target;
  http::streamed_response answer;

  co_await async::detail::with_optional_timeout(
      state->options.response_timeout, open_body(platform, std::move(url), std::move(request), answer)
  );

  check_head(answer, state->options.limits);
  answer.body = http::response_body{std::make_shared<platform_body>(std::move(answer.body), state->options)};
  co_return answer;
}

} // namespace

client_backend make_platform_backend(std::string_view url, client_options const &options) {
  static constexpr client_operations operations{
      [](std::shared_ptr<void> const &state, http::request request) {
        auto const platform = std::static_pointer_cast<platform_state>(state);

        make_ready(*platform, request);
        return send_on_platform(platform, std::move(request));
      },
      [](std::shared_ptr<void> const &state, http::request request) {
        auto const platform = std::static_pointer_cast<platform_state>(state);

        make_ready(*platform, request);
        return stream_on_platform(platform, std::move(request));
      },
  };

  // From the page itself: what follows the dot reads as a path does, and the dot keeps the rest beside the page.
  auto const from_the_page = url == "." || url.starts_with("./");
  auto const path = from_the_page ? url.substr(1) : url;
  auto const relative = path.empty() || path.starts_with('/');
  auto server = relative ? parse_url("http://page.invalid" + std::string{path}) : parse_url(url);

  if (!server.socket_path.empty()) {
    throw std::invalid_argument{"quoll::http::client: a browser reaches no Unix domain socket"};
  }

  // Refuses a host that is none, as a client over a connector does.
  (void)host_field(server.host, server.port, server.secure ? 443 : 80);

  auto origin = from_the_page ? std::string{"."}
                : relative    ? std::string{}
                              : std::string{url.substr(0, url.find('/', url.find("://") + 3))};

  return {std::make_shared<platform_state>(std::move(origin), std::move(server.prefix), options), &operations};
}

} // namespace quoll::http::detail
