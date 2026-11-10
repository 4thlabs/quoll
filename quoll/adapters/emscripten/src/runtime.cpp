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

#include <quoll/emscripten/runtime.hpp>

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detail/is_a_stop.hpp>
#include <quoll/async/report.hpp>
#include <quoll/async/task.hpp>

#include <quoll/emscripten/detail/js.hpp>

#include <cstdlib>
#include <exception>
#include <utility>

namespace quoll::emscripten {

runtime::runtime(async::runtime_options options) noexcept { static_cast<void>(options); }

runtime::executor runtime::get_executor() noexcept { return {}; }

std::stop_token runtime::get_stop_token() noexcept { return {}; }

// Nothing to run: the browser does, once main() has returned to it. What makes main() read the same as a native one.
void runtime::run() noexcept {}

namespace {

// What comes of the program's task: under Node, a failure is its exit status once the loop has drained.
struct program_outcome {
  void operator()() const noexcept {}

  void operator()(std::exception_ptr const &failure) const noexcept {
    if (async::detail::is_a_stop(failure)) {
      return;
    }

    async::detail::report_always("the program's main task ended on an exception", failure);
    detail::quoll_emscripten_set_exit_code(EXIT_FAILURE);
  }
};

} // namespace

int runtime::run(async::task<> main) {
  async::co_spawn(get_executor(), std::move(main), program_outcome{}, get_stop_token());
  return EXIT_SUCCESS;
}

} // namespace quoll::emscripten
