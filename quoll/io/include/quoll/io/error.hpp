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

#ifndef QUOLL_IO_ERROR_HPP
#define QUOLL_IO_ERROR_HPP

#include <string>
#include <system_error>
#include <type_traits>

namespace quoll::io {

/**
 * @brief Stream outcomes specific to Quoll. Other failures use the standard codes: `std::errc::broken_pipe` when
 * the peer is gone, `std::errc::operation_canceled` when a stop request ended an operation.
 */
enum class error {
  /** @brief The peer closed its side cleanly: no more bytes will come. */
  end_of_stream = 1,
};

namespace detail {

// std::error_category is polymorphic by design: the virtual functions here are the standard's, not Quoll's.
class io_error_category final : public std::error_category {
public:
  [[nodiscard]] char const *name() const noexcept override { return "quoll.io"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<error>(value)) {
    case error::end_of_stream:
      return "end of stream";
    }
    return "unknown quoll.io error";
  }
};

} // namespace detail

/** @brief The category of io::error codes. */
[[nodiscard]] inline std::error_category const &io_category() noexcept {
  static detail::io_error_category const category;
  return category;
}

[[nodiscard]] inline std::error_code make_error_code(error value) noexcept {
  return {static_cast<int>(value), io_category()};
}

} // namespace quoll::io

template <>
struct std::is_error_code_enum<quoll::io::error> : std::true_type {};

#endif // QUOLL_IO_ERROR_HPP
