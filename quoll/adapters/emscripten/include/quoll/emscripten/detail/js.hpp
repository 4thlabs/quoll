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

#ifndef QUOLL_EMSCRIPTEN_DETAIL_JS_HPP
#define QUOLL_EMSCRIPTEN_DETAIL_JS_HPP

#include <cstddef>

// Functions of the JavaScript library js/quoll_emscripten.js (see there), imported by the WebAssembly module.
namespace quoll::emscripten::detail {

extern "C" {

void quoll_emscripten_post(void (*resume)(void *item), void *item);

// `complete` always runs, from the event loop, with `elapsed` false once cancelled.
int quoll_emscripten_timer_start(double milliseconds, void (*complete)(void *operation, int elapsed), void *operation);
void quoll_emscripten_timer_cancel(int id);

// `url` is null-terminated UTF-8; `method` and `headers` are bytes, one per character of the JavaScript string.
int quoll_emscripten_fetch_start(char const *url, char const *method, std::size_t method_size, char const *headers,
                                 std::size_t headers_size, void const *body, std::size_t body_size,
                                 void (*complete)(void *operation, int id, int outcome), void *operation);
void quoll_emscripten_fetch_abort(int id);
int quoll_emscripten_fetch_status(int id);
std::size_t quoll_emscripten_fetch_size(int id, int part);
void quoll_emscripten_fetch_copy(int id, int part, void *out);
void quoll_emscripten_fetch_release(int id);

} // extern "C"

// Values of `outcome` passed to the completion of quoll_emscripten_fetch_start.
enum class fetch_outcome : int {
  response = 0,
  aborted = 1,
  failed = 2,
};

// Parts of a settled fetch, for quoll_emscripten_fetch_size / _copy.
enum class fetch_part : int {
  // The status text (bytes), or the failure message (UTF-8).
  reason = 0,
  // The header fields, "name:value\n" each (bytes).
  fields = 1,
  body = 2,
};

} // namespace quoll::emscripten::detail

#endif // QUOLL_EMSCRIPTEN_DETAIL_JS_HPP
