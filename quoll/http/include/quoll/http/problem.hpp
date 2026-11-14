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

#ifndef QUOLL_HTTP_PROBLEM_HPP
#define QUOLL_HTTP_PROBLEM_HPP

#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace quoll::http {

/** @brief The media type of a problem body, RFC 9457. */
inline constexpr std::string_view problem_content_type = "application/problem+json";

/**
 * @brief An RFC 9457 problem, the body of an error a server answers: an http_error its handler raised, a request it
 * refused. Every member is optional, as in the RFC: an absent `type` means `about:blank`.
 */
struct problem {
  /** @brief A URI identifying the problem type. */
  std::optional<std::string> type{};
  /** @brief A short summary of the problem type: the reason phrase of the status for a server's own errors. */
  std::optional<std::string> title{};
  std::optional<int> status{};
  /** @brief What went wrong with this request. */
  std::optional<std::string> detail{};
  /** @brief A URI identifying this occurrence. */
  std::optional<std::string> instance{};
};

/**
 * @brief A response of `code` with a problem, its title the status's reason phrase and its detail `detail`, written
 * as JSON by Quoll: control characters escaped; a detail that is not UTF-8 shown in printable ASCII, '?' for each byte
 * outside it, the valid characters too.
 */
[[nodiscard]] response problem_response(status code, std::optional<std::string> detail);

} // namespace quoll::http

#endif // QUOLL_HTTP_PROBLEM_HPP
