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

#include <quoll/http/parser.hpp>

#include <quoll/http/status.hpp>
#include <quoll/http/version.hpp>

#include <quoll/third_party/llhttp.h>

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace quoll::http {

// llhttp keeps a pointer to this in `state_.data`: it stays in place, whatever happens to the parser holding it.
template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
struct parser<Message>::impl {
  static constexpr bool is_request = std::same_as<Message, request>;

  explicit impl(parse_limits limits) noexcept : limits_(limits) {
    llhttp_init(&state_, is_request ? HTTP_REQUEST : HTTP_RESPONSE, &settings());
    state_.data = this;
  }

  impl(impl const &) = delete;
  impl(impl &&) = delete;
  impl &operator=(impl const &) = delete;
  impl &operator=(impl &&) = delete;
  ~impl() = default;

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
      // Only reached after the upgrade message was released (on_message_complete pauses first): what follows
      // belongs to the upgraded protocol. Leave it unconsumed.
      error_ = error::upgraded;
      return {offset(data), error_};
    default:
      if (!error_) {
        error_ = error::bad_message;
      }

      return {offset(data), error_};
    }
  }

  [[nodiscard]] std::error_code finish() {
    if (done_ || error_) {
      return error_;
    }

    // The stream may end between two messages. llhttp would disagree right after one: paused in
    // on_message_complete, it has not yet recorded that ending there is safe.
    if (!in_message_) {
      llhttp_reset(&state_);
      return {};
    }

    auto const code = llhttp_finish(&state_);

    // HPE_PAUSED: on_message_complete ended the message with the stream. Either way the stream is over and llhttp
    // would ignore anything fed next: start afresh, ready for a new connection.
    if (code == HPE_OK || code == HPE_PAUSED) {
      llhttp_reset(&state_);
      return {};
    }

    error_ = code == HPE_INVALID_EOF_STATE ? error::partial_message : error::bad_message;
    return error_;
  }

  [[nodiscard]] std::string_view error_reason() const noexcept {
    // llhttp replaces the reason given by some callbacks with a generic one: ours wins.
    auto const *const reason = reason_ != nullptr ? reason_ : llhttp_get_error_reason(&state_);

    return reason != nullptr ? std::string_view{reason} : std::string_view{};
  }

  [[nodiscard]] Message release() {
    assert(done_ && "parser::release() without a complete message");

    auto message = std::exchange(message_, Message{});

    done_ = false;
    head_complete_ = false;
    body_streamed_ = false;
    keep_alive_ = false;
    upgrade_ = false;

    // The request method holds until its final response: an interim 1xx answers the same request.
    if constexpr (!is_request) {
      if (!is_informational(message.status)) {
        request_method_ = verb::get;
      }
    }

    return message;
  }

  [[nodiscard]] std::size_t offset(std::span<char const> data) const noexcept {
    auto const *const position = llhttp_get_error_pos(&state_);

    if (data.empty() || position == nullptr) {
      return 0;
    }

    return static_cast<std::size_t>(position - data.data());
  }

  [[nodiscard]] static impl &self(llhttp_t *state) noexcept { return *static_cast<impl *>(state->data); }

  void decide_streaming() noexcept { body_streamed_ = decide_ && decide_(std::as_const(message_)); }

  [[nodiscard]] std::uint64_t body_limit() const noexcept {
    return body_streamed_ ? limits_.max_streamed_body_size : limits_.max_body_size;
  }

  [[nodiscard]] bool announces_too_much(llhttp_t const *state) const noexcept {
    return (state->flags & F_CONTENT_LENGTH) != 0 && state->content_length > body_limit();
  }

  // Records why parsing stops; returns the code that stops llhttp.
  [[nodiscard]] int fail(llhttp_t *state, error reason, char const *text) noexcept {
    error_ = reason;
    reason_ = text;
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

    // release() has already reset the message, but for the target.
    if constexpr (is_request) {
      // llhttp appends the target in pieces: start from empty, not from the default "/".
      self_.message_.target.clear();
    }

    self_.field_name_.clear();
    self_.field_value_.clear();
    self_.header_size_ = 0;
    self_.field_count_ = 0;
    self_.body_size_ = 0;
    self_.in_message_ = true;
    self_.in_trailers_ = false;
    return 0;
  }

  static int on_url(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    if (auto const code = self_.count_header(state, length); code != 0) {
      return code;
    }

    if constexpr (is_request) {
      self_.message_.target.append(at, length);
    }

    return 0;
  }

  static int on_status(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    if (auto const code = self_.count_header(state, length); code != 0) {
      return code;
    }

    if constexpr (!is_request) {
      self_.message_.reason.append(at, length);
    }

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

  // Called once per field, even when the value is empty. Counts the separators the data callbacks do not see
  // (": " and CRLF), and the field itself.
  static int on_header_value_complete(llhttp_t *state) noexcept {
    auto &self_ = self(state);

    if (auto const code = self_.count_header(state, 4); code != 0) {
      return code;
    }

    if (++self_.field_count_ > self_.limits_.max_field_count) {
      return self_.fail(state, error::header_too_large, "too many fields");
    }

    auto &target = self_.in_trailers_ ? self_.message_.trailers : self_.message_.fields;

    target.add(field{std::exchange(self_.field_name_, {}), std::exchange(self_.field_value_, {})});
    return 0;
  }

  // 1 tells llhttp the message has no body, 2 that it has none and the connection switches protocol.
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
      self_.decide_streaming();

      // Refused now, rather than once that much has arrived: a client waiting for `100 Continue` sends nothing more.
      if (self_.announces_too_much(state)) {
        static_cast<void>(self_.fail(state, error::body_too_large, "body too large"));
        return -1;
      }

      self_.head_complete_ = true;
      return 0;
    } else {
      auto const code = llhttp_get_status_code(state);

      self_.message_.status = static_cast<status>(code);
      self_.decide_streaming();
      self_.head_complete_ = true;

      // A 2xx answer to CONNECT turns the connection into a tunnel.
      if (self_.request_method_ == verb::connect && code >= 200 && code < 300) {
        return 2;
      }

      if (self_.request_method_ == verb::head) {
        return 1;
      }

      // Refused now, rather than past a gigabyte handed over in pieces. As llhttp reads them, these statuses announce
      // what they do not carry.
      auto const carries_body = code > 103 && code != 204 && code != 304;

      if (carries_body && self_.announces_too_much(state)) {
        static_cast<void>(self_.fail(state, error::body_too_large, "body too large"));
        return -1;
      }

      return 0;
    }
  }

  static int on_body(llhttp_t *state, char const *at, std::size_t length) noexcept {
    auto &self_ = self(state);

    // Counted apart: a body in pieces leaves the message as it comes. In 64 bits under WASM too, as memory does not
    // bound it.
    self_.body_size_ += length;

    if (self_.body_size_ > self_.body_limit()) {
      return self_.fail(state, error::body_too_large, "body too large");
    }

    self_.message_.body.append(at, length);
    return 0;
  }

  // Pausing makes llhttp_execute() return at the end of the message.
  static int on_message_complete(llhttp_t *state) noexcept {
    auto &self_ = self(state);

    self_.done_ = true;
    self_.in_message_ = false;
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
  std::function<bool(Message const &)> decide_;
  Message message_;
  std::string field_name_;
  std::string field_value_;
  std::size_t header_size_ = 0;
  std::size_t field_count_ = 0;
  std::uint64_t body_size_ = 0;
  std::error_code error_;
  char const *reason_ = nullptr;
  verb request_method_ = verb::get;
  bool done_ = false;
  bool keep_alive_ = false;
  bool upgrade_ = false;
  bool head_complete_ = false;
  bool body_streamed_ = false;
  bool in_message_ = false;
  bool in_trailers_ = false;
};

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
parser<Message>::parser(parse_limits limits) : impl_(std::make_unique<impl>(limits)) {}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
parser<Message>::parser(parser &&) noexcept = default;

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
parser<Message> &parser<Message>::operator=(parser &&) noexcept = default;

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
parser<Message>::~parser() = default;

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
parse_result parser<Message>::feed(std::span<char const> data) {
  return impl_->feed(data);
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
std::error_code parser<Message>::finish() {
  return impl_->finish();
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
                                           void parser<Message>::set_request_method(verb method) noexcept
             requires(!is_request)
{
  impl_->request_method_ = method;
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
void parser<Message>::set_body_streaming(std::function<bool(Message const &)> decide) {
  impl_->decide_ = std::move(decide);
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
bool parser<Message>::body_streamed() const noexcept {
  return impl_->body_streamed_;
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
void parser<Message>::take_body(std::string &piece) noexcept {
  piece.clear();

  // A body read whole stays whole. Each keeps the memory of the other: nothing allocates once they have grown.
  if (impl_->body_streamed_) {
    piece.swap(impl_->message_.body);
  }
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
bool parser<Message>::done() const noexcept {
  return impl_->done_;
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
bool parser<Message>::keep_alive() const noexcept {
  return impl_->keep_alive_;
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
bool parser<Message>::head_complete() const noexcept {
  return impl_->head_complete_;
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
Message const &parser<Message>::head() const noexcept {
  return impl_->message_;
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
bool parser<Message>::upgrade() const noexcept {
  return impl_->upgrade_;
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
std::string_view parser<Message>::error_reason() const noexcept {
  return impl_->error_reason();
}

template <class Message>
  requires std::same_as<Message, request> || std::same_as<Message, response>
Message parser<Message>::release() {
  return impl_->release();
}

template class parser<request>;
template class parser<response>;

} // namespace quoll::http
