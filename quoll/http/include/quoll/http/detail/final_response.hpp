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

#ifndef QUOLL_HTTP_DETAIL_FINAL_RESPONSE_HPP
#define QUOLL_HTTP_DETAIL_FINAL_RESPONSE_HPP

#include <quoll/http/response.hpp>
#include <quoll/http/verb.hpp>

namespace quoll::http::detail {

// A response to HEAD has no body: it announces the length of the one it does not carry, with Content-Length rather than
// chunked coding, whose terminating chunk would be read as the next response. In pieces, its length is only known if it
// says so: no framing is announced that nothing follows.
void keep_what_head_carries(response &answer, bool in_pieces);

// A handler's answer to `method`, unless its peer would read it as something else, shared by what serves a router:
//
// - a 2xx to CONNECT is a tunnel, which is not offered, and answers 501;
// - a 1xx is no final response: its peer goes on waiting for one, or reads a 101 as a switch of protocol, and it
// answers
//   500;
// - a body beside what writes one: neither would stand, and it answers 500, to HEAD too;
// - a length that is invalid, or other than its body, which could not be written, answers 500, and invalid to HEAD too;
// - a length with no body, but to HEAD or as a 304: its peer waits for that body, or reads the next response as it, and
//   it answers 500.
//
// Returns what was wrong, for the application to be told, or nullptr.
[[nodiscard]] char const *replace_what_would_mislead(response &answer, verb method);

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_FINAL_RESPONSE_HPP
