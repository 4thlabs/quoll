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

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/late_head.hpp>
#include <quoll/http/response_writer.hpp>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace quoll::http {

response_writer::response_writer(response_writer &&other) noexcept
    : status(other.status), fields(std::move(other.fields)), out_(std::exchange(other.out_, std::nullopt)),
      head_given_(other.head_given_) {}

response_writer &response_writer::operator=(response_writer &&other) noexcept {
  if (this != &other) {
    status = other.status;
    fields = std::move(other.fields);
    out_ = std::exchange(other.out_, std::nullopt);
    head_given_ = other.head_given_;
  }

  return *this;
}

response_writer::~response_writer() {
  try {
    give_head();
  } catch (...) {
    // Out of memory while the handler ends: its response goes without what it set.
  }
}

void response_writer::give_head() {
  if (out_ && !head_given_) {
    head_given_ = true;
    out_->head(status, std::move(fields));
  }
}

async::task<> response_writer::write(std::string_view piece) {
  if (!out_) {
    throw std::logic_error{"quoll::http::response_writer: write() on one moved from"};
  }

  give_head();
  return out_->write(piece);
}

namespace detail {

void merge_head(response &answer, status code, fields given) {
  if (code != status::ok) {
    answer.status = code;
  }

  // What the handler sets replaces what a middleware added under that name, every value of it staying, but for what is
  // a list of its own: a session's cookie, or a Vary for CORS, would be lost.
  for (auto const &entry : given) {
    if (!iequals(entry.name, "Set-Cookie") && !iequals(entry.name, "Vary")) {
      answer.fields.erase(entry.name);
    }
  }

  for (auto const &entry : given) {
    answer.fields.add(entry.name, entry.value);
  }
}

} // namespace detail

} // namespace quoll::http
