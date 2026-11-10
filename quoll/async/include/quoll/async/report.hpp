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

#ifndef QUOLL_ASYNC_REPORT_HPP
#define QUOLL_ASYNC_REPORT_HPP

#include <exception>
#include <string_view>

namespace quoll::async {

/**
 * @brief Writes one line saying `what` happened and what `failure` says, for a failure nothing else takes: an exception
 * a task spawned with async::detached let escape, or one a server's callbacks were not given. To stderr, or to the sink
 * set_report_sink() set. At most ten lines a second, as a peer may cause them at will: those past it are counted, and
 * the next line says how many went. Never throws. Thread safety: may be called from any thread.
 */
void report(std::string_view what, std::exception_ptr const &failure) noexcept;

/** @brief Where report() writes its lines, one at a time, each ending with a newline; called from any thread. */
using report_sink = void (*)(std::string_view line) noexcept;

/**
 * @brief Sends report()'s lines to `sink`, null for stderr, and returns the sink it replaces: to a log of the
 * program's own (syslog, journald), or nowhere. The lines a second count afresh from then. Thread safety: may be called
 * from any thread.
 */
report_sink set_report_sink(report_sink sink) noexcept;

} // namespace quoll::async

#endif // QUOLL_ASYNC_REPORT_HPP
