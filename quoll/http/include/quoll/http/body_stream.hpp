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

#ifndef QUOLL_HTTP_BODY_STREAM_HPP
#define QUOLL_HTTP_BODY_STREAM_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/status.hpp>

#include <concepts>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::http {

/**
 * @brief Where a body_writer writes the body of a message, a piece at a time.
 *
 * write() completes once the piece has left, so that a writer goes no faster than its peer reads, and memory holds one
 * piece, but for a transport that gathers a request before sending it (a browser's http::client, local_transport). What
 * prevents it throws std::system_error, a peer gone for instance, and goes on throwing: left to run through the writer,
 * it ends the connection, and is no failure of the writer's.
 *
 * Copied freely, it refers to what writes the body, valid until the writer's task ends. One write() at a time. Thread
 * safety: none. What writes through it does so on the executor of the writer's task: moved to another, without a timer,
 * it has no write timeout.
 */
class body_stream {
public:
  /** @brief Writes through `sink`, whose `write(std::string_view)` is what body_stream::write() says. `sink` outlives
   * this. */
  template <class Sink>
    requires(!std::same_as<Sink, body_stream> && !std::is_const_v<Sink>) &&
                requires(Sink &sink, std::string_view piece) {
                  { sink.write(piece) } -> std::same_as<async::task<>>;
                }
  explicit body_stream(Sink &sink QUOLL_LIFETIMEBOUND) noexcept
      : sink_(&sink),
        write_([](void *erased, std::string_view piece) { return static_cast<Sink *>(erased)->write(piece); }),
        head_([](void *erased, http::status status, http::fields fields) {
          if constexpr (requires(Sink &target) { target.head(status, std::move(fields)); }) {
            static_cast<Sink *>(erased)->head(status, std::move(fields));
          }
        }),
        type_(async::detail::type_id_of<Sink>()) {}

  /** @brief Writes `piece`, which stays valid until the task ends. An empty one sends the head, if it has not left. */
  [[nodiscard]] async::task<> write(std::string_view piece QUOLL_LIFETIMEBOUND) const { return write_(sink_, piece); }

  /**
   * @brief The status and fields of the response this writes, for a head that leaves with the first write: see
   * http::response_writer. Nothing where a body has no such head.
   */
  void head(http::status status, http::fields fields) const { head_(sink_, status, std::move(fields)); }

  /** @brief What it writes through if it is exactly a `Sink`, otherwise `nullptr`. */
  template <class Sink>
  [[nodiscard]] Sink *target() const noexcept {
    return type_ == async::detail::type_id_of<Sink>() ? static_cast<Sink *>(sink_) : nullptr;
  }

private:
  void *sink_;
  async::task<> (*write_)(void *, std::string_view);
  void (*head_)(void *, http::status, http::fields);
  async::detail::type_id type_;
};

/**
 * @brief Writes the body of a message in pieces, for what cannot or need not hold it whole in memory.
 *
 * @code
 * router.get("/export", [](http::request) {
 *   http::response response;
 *
 *   response.fields.set("Content-Type", "text/csv");
 *   response.body_writer = [](http::body_stream out) -> task<> {
 *     for (auto const &row : rows) {
 *       co_await out.write(row);
 *     }
 *   };
 *   return response;
 * });
 * @endcode
 *
 * With a `Content-Length` in the fields, exactly that many bytes are expected: more throws, less cuts the connection,
 * so that its peer sees a body cut short. The head leaves with the first piece; an empty piece sends it at once.
 *
 * On a response, the body goes without a `Content-Length` in chunks, or to HTTP/1.0 up to the end of the connection,
 * which its peer cannot tell from a cut. What the writer throws after the start of a body cuts the connection the same
 * way, as a peer must not take what came for all of it, and before it, answers 500. Not called for a response to HEAD,
 * but for a handler writing through an http::response_writer. An empty piece is for a stream to announce itself before
 * its first data.
 *
 * On a response, it runs after its handler has ended: what it captures by value lives until its task ends, and a
 * `std::string_view` of a path parameter, or a reference to the request, does not. An http::body_reader does, on a
 * connection or through local_transport. With `Expect: 100-continue`, it writes and reads in turn, as `100 Continue`
 * and a piece must not leave at once.
 *
 * On a request, it runs beside the reading of the response: http::client::send() says what comes of that, and how
 * a transport that gathers it first differs. What it captures has to live until the task of the request ends.
 */
using body_writer = std::function<async::task<>(body_stream)>;

} // namespace quoll::http

#endif // QUOLL_HTTP_BODY_STREAM_HPP
