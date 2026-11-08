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

#ifndef QUOLL_HTTP_DETAIL_ONE_AT_A_TIME_HPP
#define QUOLL_HTTP_DETAIL_ONE_AT_A_TIME_HPP

#include <atomic>
#include <memory>
#include <string_view>

namespace quoll::http::detail {

// The turn of one operation of an object that allows one at a time: taken when the operation is asked, and passed to
// its task as a parameter, which moves it into a local, so that it is given back when the operation is over, and with
// the frame of a task dropped unrun. Asked while another holds it: std::logic_error with `message`, so that operations
// run in the order they were asked.
//
// Thread safety: the flag is atomic, taken by an exchange, so that an operation asked from another thread is refused
// rather than raced; the flag keeps what holds it alive, an aliasing pointer into its state.
class one_at_a_time {
public:
  one_at_a_time(std::shared_ptr<std::atomic<bool>> flag, std::string_view message);

  one_at_a_time(one_at_a_time const &) = delete;
  one_at_a_time &operator=(one_at_a_time const &) = delete;
  one_at_a_time(one_at_a_time &&other) noexcept;
  one_at_a_time &operator=(one_at_a_time &&) = delete;

  ~one_at_a_time();

private:
  // Null once moved from.
  std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_ONE_AT_A_TIME_HPP
