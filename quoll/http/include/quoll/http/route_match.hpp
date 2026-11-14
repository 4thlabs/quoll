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

#ifndef QUOLL_HTTP_ROUTE_MATCH_HPP
#define QUOLL_HTTP_ROUTE_MATCH_HPP

#include <quoll/http/body_reader.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::http {

/** @brief What a route matched in a request. */
struct route_match {
  /** @brief The name the route was added with. Valid as long as the router. */
  std::string_view name;
  /** @brief The path parameters, percent-decoded, in the order of the template. */
  std::vector<std::string> params;
  /** @brief The query string, after `?` and still encoded; empty without one. */
  std::string query;
  /**
   * @brief The values of the query parameters the route's template names, `?q={q}`, in its order: each the values
   * the request gives it, decoded, in order, none when it gives none.
   */
  std::vector<std::vector<std::string>> query_params{};
  /**
   * @brief What reads the body, on the route of a handler taking an http::body_reader: nothing on a route of
   * router::add(), whose request comes whole.
   */
  body_reader reader{};
  /** @brief What `reader` reads from, when nothing outside the router keeps it: for what goes on after dispatch(). */
  std::shared_ptr<void> reader_owner{};
};

} // namespace quoll::http

#endif // QUOLL_HTTP_ROUTE_MATCH_HPP
