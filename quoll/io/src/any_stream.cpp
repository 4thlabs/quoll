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

#include <quoll/io/any_stream.hpp>

#include <exception>
#include <stdexcept>
#include <utility>

namespace quoll::io {

void any_stream::throw_refused() {
  throw std::logic_error{"quoll::io::any_stream: moved from, or an operation is still pending"};
}

void any_stream::throw_finished() {
  throw std::logic_error{"quoll::io::any_stream: this operation has already completed"};
}

void any_stream::take(any_stream &other) noexcept {
  // The awaitable would go on using what moves.
  if (other.reading_ || other.writing_ || other.shutting_down_) {
    std::terminate();
  }

  vtable_ = std::exchange(other.vtable_, nullptr);

  if (vtable_ == nullptr) {
    return;
  }

  if (vtable_->relocate != nullptr) {
    block_ = vtable_->relocate(other.block_, storage_);
  } else {
    block_ = other.block_;
  }

  other.block_ = nullptr;
}

void any_stream::reset() noexcept {
  if (vtable_ != nullptr) {
    // The awaitable would go on using what goes.
    if (reading_ || writing_ || shutting_down_) {
      std::terminate();
    }

    vtable_->destroy(block_);
    vtable_ = nullptr;
    block_ = nullptr;
  }
}

} // namespace quoll::io
