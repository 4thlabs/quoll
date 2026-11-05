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

#ifndef QUOLL_HTTP_BODY_READER_HPP
#define QUOLL_HTTP_BODY_READER_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>

#include <concepts>
#include <optional>
#include <string_view>
#include <type_traits>

namespace quoll::http {

namespace detail {

[[nodiscard]] async::task<std::optional<std::string_view>> no_body();

// The body of a request already whole, as one piece.
class whole_body {
public:
  explicit whole_body(std::string_view body QUOLL_LIFETIMEBOUND) noexcept : body_(body) {}

  [[nodiscard]] async::task<std::optional<std::string_view>> read();

private:
  std::string_view body_;
};

} // namespace detail

/**
 * @brief The body of a message, a piece at a time, for what cannot or need not hold it whole in memory.
 *
 * A handler gets one by taking it as its last parameter, which makes its route read the request as it goes:
 *
 * @code
 * router.post("/users/{id}/avatar", [](std::int64_t id, http::body_reader body) -> task<http::response> {
 *   while (auto piece = co_await body.read()) {
 *     co_await store(id, *piece);
 *   }
 *
 *   co_return http::response{};
 * });
 * @endcode
 *
 * The next piece is read when it is asked for, so that a peer sends no faster than a handler takes it. What a handler
 * leaves unread closes the connection after its response.
 *
 * read() gives a view, valid up to the next read(), and nothing at the end of the body, as often as it is asked. What
 * prevents reading throws std::system_error, as a body cut short or too large (http::error), or a stream that failed.
 * The request is then answered as one refused as it is read would be, whatever the handler made of it, as it could not
 * know what the connection can still carry. Failing once, read() goes on failing. A cancellation of
 * the read, as a handler's own timeout is, ends the reading too, as a stream does not take it up again, and the
 * handler's response then stands, though over TLS, whose session a cancelled read leaves unusable, it may not arrive.
 * During `100 Continue`, it is a failure, as what did not leave whole leaves a stream nothing more can be written to.
 *
 * Copied freely, it refers to what reads the body, valid until the response has left: a body_writer may read it, as an
 * echo or a proxy does, if it came with http::local_transport or a connection, and not from a dispatch() of your own. A
 * default-constructed one has nothing to read. One read() at a time, and a body is read once: a middleware calling
 * `next` again finds what is left of it. The trailers of a body in pieces are not handed over: `request.trailers` is
 * empty.
 *
 * Thread safety: none. What reads through it does so on the executor of the handler's task: moved to another, without a
 * timer, it has no read timeout.
 */
class body_reader {
public:
  body_reader() noexcept = default;

  /** @brief Reads through `source`, whose `read()` is what body_reader::read() says. `source` outlives this. */
  template <class Source>
    requires(!std::same_as<Source, body_reader> && !std::is_const_v<Source>) && requires(Source &source) {
      { source.read() } -> std::same_as<async::task<std::optional<std::string_view>>>;
    }
  explicit body_reader(Source &source QUOLL_LIFETIMEBOUND) noexcept
      : source_(&source), read_([](void *erased) { return static_cast<Source *>(erased)->read(); }),
        type_(async::detail::type_id_of<Source>()) {}

  /** @brief The next piece of the body, or nothing at its end. */
  [[nodiscard]] async::task<std::optional<std::string_view>> read() const {
    return read_ != nullptr ? read_(source_) : detail::no_body();
  }

  /** @brief What it reads through if it is exactly a `Source`, otherwise `nullptr`. */
  template <class Source>
  [[nodiscard]] Source *target() const noexcept {
    return read_ != nullptr && type_ == async::detail::type_id_of<Source>() ? static_cast<Source *>(source_) : nullptr;
  }

  /** @brief Whether it reads something. */
  [[nodiscard]] explicit operator bool() const noexcept { return read_ != nullptr; }

private:
  void *source_ = nullptr;
  async::task<std::optional<std::string_view>> (*read_)(void *) = nullptr;
  async::detail::type_id type_{};
};

} // namespace quoll::http

#endif // QUOLL_HTTP_BODY_READER_HPP
