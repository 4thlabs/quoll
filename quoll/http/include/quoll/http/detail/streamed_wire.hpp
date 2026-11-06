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

#ifndef QUOLL_HTTP_DETAIL_STREAMED_WIRE_HPP
#define QUOLL_HTTP_DETAIL_STREAMED_WIRE_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/http/detail/streamed_head.hpp>
#include <quoll/http/fields.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace quoll::http::detail {

// What goes on the wire for a body written in pieces. The head leaves with the first piece, in one write, so that the
// second write of a message does not wait for what acknowledges the first. Thread safety: none.
class streamed_wire {
public:
  streamed_wire(std::string head, streamed_framing framing) noexcept;

  // `piece` would take the body past its Content-Length.
  [[nodiscard]] bool past_its_length(std::string_view piece) const noexcept;

  // The bytes that send `piece`, valid until the next call and as long as `piece`: an empty piece sends what is held
  // back, and what has nothing to frame and nothing before it is not copied.
  [[nodiscard]] std::string_view frame(std::string_view piece QUOLL_LIFETIMEBOUND) QUOLL_LIFETIMEBOUND;

  // The bytes that end a body that is whole: see short_of_its_length().
  [[nodiscard]] std::string_view end(fields const &trailers) QUOLL_LIFETIMEBOUND;

  // What it returned holds the head: nothing is left to hold back.
  [[nodiscard]] bool started() const noexcept;

  [[nodiscard]] bool short_of_its_length() const noexcept;

private:
  void append_head();

  std::string head_;
  streamed_framing framing_;
  std::uint64_t left_;
  std::string wire_;
  bool started_ = false;
};

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_STREAMED_WIRE_HPP
