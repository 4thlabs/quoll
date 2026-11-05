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

#ifndef QUOLL_HTTP_DETAIL_STREAMED_HEAD_HPP
#define QUOLL_HTTP_DETAIL_STREAMED_HEAD_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::http::detail {

// How the body of a response written in pieces is framed.
struct streamed_framing {
  bool chunked = false;
  // What a Content-Length announces; with neither, the body ends with the connection.
  std::optional<std::uint64_t> length{};
};

// Appends the start line and fields of a response with a body_writer. Throws std::invalid_argument for what serialize()
// rejects, for a body as well as a writer, and for a status that has no body.
[[nodiscard]] streamed_framing serialize_streamed_head(response const &message, std::string &out);

// Appends the start line and fields of a request with a body_writer, in chunks unless a Content-Length says how much
// comes. Throws std::invalid_argument for what serialize() rejects, for a body as well as a writer, for a method that
// has no body, and for HTTP/1.0 with no Content-Length: it has no chunks, and a request cannot end with its connection.
[[nodiscard]] streamed_framing serialize_streamed_head(request const &message, std::string &out);

// Appends a chunk: nothing for an empty piece, which would read as the last one.
void append_chunk(std::string_view piece, std::string &out);

// Appends the last chunk and the trailers, which serialize_streamed_head() has checked.
void append_last_chunk(fields const &trailers, std::string &out);

// Runs the writer of `message` for a transport that has no use for pieces: `body` then holds what it wrote. Throws
// std::invalid_argument for a body as well as a writer.
[[nodiscard]] async::task<> write_whole_body(request &message);

// What goes on the wire for a body written in pieces. The head leaves with the first piece, in one write, so that the
// second write of a message does not wait for what acknowledges the first. Thread safety: none.
class streamed_wire {
public:
  streamed_wire(std::string head, streamed_framing framing) noexcept
      : head_(std::move(head)), framing_(framing), left_(framing.length.value_or(0)) {}

  // `piece` would take the body past its Content-Length.
  [[nodiscard]] bool past_its_length(std::string_view piece) const noexcept {
    return framing_.length && piece.size() > left_;
  }

  // The bytes that send `piece`, valid until the next call and as long as `piece`: an empty piece sends what is held
  // back, and what has nothing to frame and nothing before it is not copied.
  [[nodiscard]] std::string_view frame(std::string_view piece);

  // The bytes that end a body that is whole: see short_of_its_length().
  [[nodiscard]] std::string_view end(fields const &trailers);

  // Nothing has left yet.
  [[nodiscard]] bool started() const noexcept { return started_; }

  [[nodiscard]] bool short_of_its_length() const noexcept { return left_ != 0; }

private:
  void append_head();

  std::string head_;
  streamed_framing framing_;
  std::uint64_t left_;
  std::string wire_;
  bool started_ = false;
};

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_STREAMED_HEAD_HPP
