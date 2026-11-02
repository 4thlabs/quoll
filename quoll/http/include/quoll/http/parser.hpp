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

#ifndef QUOLL_HTTP_PARSER_HPP
#define QUOLL_HTTP_PARSER_HPP

#include <quoll/http/error.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/version.hpp>

#include <quoll/third_party/llhttp.h>

#include <cassert>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace quoll::http {

/** @brief Size limits of a parser, against peers sending unbounded messages. */
struct parse_limits {
  /** @brief Start line, fields and trailers together. */
  std::size_t max_header_size = 64 * 1024;
  std::size_t max_body_size = 8 * 1024 * 1024;
};

/** @brief Outcome of parser::feed(). */
struct parse_result {
  /** @brief Bytes used from the input; the rest belongs to the next message (or to the upgraded protocol). */
  std::size_t consumed = 0;
  std::error_code error;
};

/**
 * @brief Incremental, sans-IO HTTP/1.x parser (llhttp) for a request or a response.
 *
 * Feed it bytes as they arrive: feed() stops at the end of a message and reports what it consumed, so the bytes of
 * a pipelined next message stay with the caller. Once done(), release() hands the message over and the parser is
 * ready for the next one on the same connection. When the peer closes the connection, call finish(): it completes
 * a body delimited by the end of the stream, or reports a truncated message.
 *
 * Responses: interim 1xx responses are messages of their own (release and keep feeding for the final one), and a
 * response to HEAD needs set_request_method(verb::head) first, as its Content-Length does not announce a body.
 *
 * Thread safety: none.
 */
template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
class parser {
  static constexpr bool is_request = std::same_as<Message, request>;

public:
  using message_type = Message;

  explicit parser(parse_limits limits = {}) noexcept : limits_(limits) {
    llhttp_init(&state_, is_request ? HTTP_REQUEST : HTTP_RESPONSE, &settings());
    state_.data = this;
  }

  parser(parser const &) = delete;
  parser &operator=(parser const &) = delete;

  // llhttp keeps no pointer into the parser between two calls, except `data`, re-pointed here.
  parser(parser &&other) noexcept
      : state_(other.state_), limits_(other.limits_), message_(std::move(other.message_)),
        field_name_(std::move(other.field_name_)), field_value_(std::move(other.field_value_)),
        header_size_(other.header_size_), error_(other.error_), request_method_(other.request_method_),
        done_(other.done_), keep_alive_(other.keep_alive_), upgrade_(other.upgrade_), in_trailers_(other.in_trailers_) {
    state_.data = this;
  }

  parser &operator=(parser &&other) noexcept {
    if (this != &other) {
      state_ = other.state_;
      state_.data = this;
      limits_ = other.limits_;
      message_ = std::move(other.message_);
      field_name_ = std::move(other.field_name_);
      field_value_ = std::move(other.field_value_);
      header_size_ = other.header_size_;
      error_ = other.error_;
      request_method_ = other.request_method_;
      done_ = other.done_;
      keep_alive_ = other.keep_alive_;
      upgrade_ = other.upgrade_;
      in_trailers_ = other.in_trailers_;
    }

    return *this;
  }

  ~parser() = default;

  /** @brief Parses `data` up to the end of the current message. Does nothing once done() or after an error. */
  [[nodiscard]] parse_result feed(std::span<char const> data) {
    if (done_ || error_) {
      return {0, error_};
    }

    auto const code = llhttp_execute(&state_, data.data(), data.size());

    switch (code) {
    case HPE_OK:
      return {data.size(), {}};
    case HPE_PAUSED: {
      // Paused by on_message_complete: the message ends here.
      auto const consumed = offset(data);

      llhttp_resume(&state_);
      return {consumed, {}};
    }
    case HPE_PAUSED_UPGRADE:
      // What follows belongs to the upgraded protocol: leave it unconsumed.
      return {offset(data), {}};
    default:
      if (!error_) {
        error_ = error::bad_message;
      }

      return {offset(data), error_};
    }
  }

  /** @brief The peer closed the stream: ends a body delimited by it, or reports a truncated message. */
  [[nodiscard]] std::error_code finish() {
    if (done_ || error_) {
      return error_;
    }

    auto const code = llhttp_finish(&state_);

    if (code == HPE_OK || code == HPE_PAUSED) {
      return {};
    }

    error_ = code == HPE_INVALID_EOF_STATE ? error::partial_message : error::bad_message;
    return error_;
  }

  /** @brief Responses only: the method of the request answered, so that a response to HEAD has no body. */
  void set_request_method(verb method) noexcept
    requires(!is_request)
  {
    request_method_ = method;
  }

  /** @brief A complete message is waiting for release(). */
  [[nodiscard]] bool done() const noexcept { return done_; }

  /** @brief When done(): whether the connection may carry another message. */
  [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }

  /** @brief When done(): the connection switches protocol (101, CONNECT); its next bytes are not HTTP/1.x. */
  [[nodiscard]] bool upgrade() const noexcept { return upgrade_; }

  /** @brief Why the input was rejected, for diagnostics. */
  [[nodiscard]] std::string_view error_reason() const noexcept {
    auto const *const reason = llhttp_get_error_reason(&state_);

    return reason != nullptr ? std::string_view{reason} : std::string_view{};
  }

  /**
   * @brief Takes the parsed message and gets ready for the next one. Precondition: done().
   *
   * Responses: after a final (non-1xx) response, the request method set by set_request_method() is forgotten.
   */
  [[nodiscard]] Message release() {
    assert(done_ && "parser::release() without a complete message");

    auto message = std::exchange(message_, Message{});

    done_ = false;

    // The request method holds until its final response: an interim 1xx answers the same request.
    if constexpr (is_request) {
      request_method_ = verb::get;
    } else if (!is_informational(message.status)) {
      request_method_ = verb::get;
    }

    return message;
  }

private:
  [[nodiscard]] std::size_t offset(std::span<char const> data) const noexcept {
    auto const *const position = llhttp_get_error_pos(&state_);

    if (data.empty() || position == nullptr) {
      return 0;
    }

    return static_cast<std::size_t>(position - data.data());
  }

  [[nodiscard]] static parser &self(llhttp_t *state) noexcept { return *static_cast<parser *>(state->data); }

  // Records why parsing stops; returns the code that stops llhttp.
  [[nodiscard]] int fail(llhttp_t *state, error reason, char const *text) noexcept {
    error_ = reason;
    llhttp_set_error_reason(state, text);
    return HPE_USER;
  }

  // Returns 0, or the code that stops llhttp.
  [[nodiscard]] int count_header(llhttp_t *state, std::size_t length) noexcept {
    header_size_ += length;

    if (header_size_ > limits_.max_header_size) {
      return fail(state, error::header_too_large, "header too large");
    }

    return 0;
  }

  // The callbacks are noexcept: an exception cannot cross llhttp's C frames (running out of memory terminates).

  static int on_message_begin(llhttp_t *state) noexcept {
    auto &self_ = self(state);

    self_.message_ = Message{};

    if constexpr (is_request) {
      // llhttp appends the target in pieces: start from empty, not from the default "/".
      self_.message_.target.clear();
    }

    self_.field_name_.clear();
    self_.field_value_.clear();
    self_.header_size_ = 0;
    self_.in_trailers_ = false;
    return 0;
  }

  static int on_url(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    if (auto const code = self_.count_header(state, length); code != 0) {
      return code;
    }

    self_.message_.target.append(at, length);
    return 0;
  }

  static int on_status(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    if (auto const code = self_.count_header(state, length); code != 0) {
      return code;
    }

    self_.message_.reason.append(at, length);
    return 0;
  }

  static int on_header_field(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    if (auto const code = self_.count_header(state, length); code != 0) {
      return code;
    }

    self_.field_name_.append(at, length);
    return 0;
  }

  static int on_header_value(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    if (auto const code = self_.count_header(state, length); code != 0) {
      return code;
    }

    self_.field_value_.append(at, length);
    return 0;
  }

  // Called once per field, even when the value is empty.
  static int on_header_value_complete(llhttp_t *state) noexcept {
    auto &self_ = self(state);
    auto &target = self_.in_trailers_ ? self_.message_.trailers : self_.message_.fields;

    target.add(self_.field_name_, self_.field_value_);
    self_.field_name_.clear();
    self_.field_value_.clear();
    return 0;
  }

  // 1 tells llhttp the message has no body.
  static int on_headers_complete(llhttp_t *state) noexcept {
    auto &self_ = self(state);

    self_.in_trailers_ = true;

    if (llhttp_get_http_major(state) != 1) {
      return self_.fail(state, error::bad_message, "unsupported HTTP version");
    }

    self_.message_.version = llhttp_get_http_minor(state) == 0 ? version::http_1_0 : version::http_1_1;

    if constexpr (is_request) {
      auto const method = parse_verb(llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(state))));

      if (!method) {
        // on_headers_complete cannot return HPE_USER: -1 stops parsing, error_ tells why.
        static_cast<void>(self_.fail(state, error::unsupported_method, "unsupported method"));
        return -1;
      }

      self_.message_.method = *method;
      return 0;
    } else {
      self_.message_.status = static_cast<status>(llhttp_get_status_code(state));
      return self_.request_method_ == verb::head ? 1 : 0;
    }
  }

  static int on_body(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    if (self_.message_.body.size() + length > self_.limits_.max_body_size) {
      return self_.fail(state, error::body_too_large, "body too large");
    }

    self_.message_.body.append(at, length);
    return 0;
  }

  // Pausing makes llhttp_execute() return at the end of the message.
  static int on_message_complete(llhttp_t *state) noexcept {
    auto &self_ = self(state);

    self_.done_ = true;
    self_.keep_alive_ = llhttp_should_keep_alive(state) != 0;
    self_.upgrade_ = llhttp_get_upgrade(state) != 0;
    return HPE_PAUSED;
  }

  [[nodiscard]] static constexpr llhttp_settings_t make_settings() noexcept {
    llhttp_settings_t settings{};

    settings.on_message_begin = &on_message_begin;
    settings.on_header_field = &on_header_field;
    settings.on_header_value = &on_header_value;
    settings.on_header_value_complete = &on_header_value_complete;
    settings.on_headers_complete = &on_headers_complete;
    settings.on_body = &on_body;
    settings.on_message_complete = &on_message_complete;

    if constexpr (is_request) {
      settings.on_url = &on_url;
    } else {
      settings.on_status = &on_status;
    }

    return settings;
  }

  [[nodiscard]] static llhttp_settings_t const &settings() noexcept {
    static constexpr llhttp_settings_t value = make_settings();
    return value;
  }

  llhttp_t state_{};
  parse_limits limits_;
  Message message_;
  std::string field_name_;
  std::string field_value_;
  std::size_t header_size_ = 0;
  std::error_code error_;
  verb request_method_ = verb::get;
  bool done_ = false;
  bool keep_alive_ = false;
  bool upgrade_ = false;
  bool in_trailers_ = false;
};

using request_parser = parser<request>;
using response_parser = parser<response>;

} // namespace quoll::http

#endif // QUOLL_HTTP_PARSER_HPP
