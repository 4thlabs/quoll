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

// A connection from anywhere: bytes in pieces, into a server with a router and a REST API. Whatever they are, the task
// ends without an exception, as a connection ends on what it cannot read.

#include <quoll/async/task.hpp>
#include <quoll/fuzz/api.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/serve_connection.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

// What the server writes is checked, then dropped. A response leaves in one write, and a body here is JSON, starting
// with a quote: a write starting like this is a 500, which a route answers for whatever it threw. The service never
// throws here, so that is a bug in the decoding.
class checked_stream {
public:
  checked_stream(std::string_view input, std::size_t chunk) noexcept : input_(input, chunk) {}

  [[nodiscard]] auto read_some(std::span<std::byte> buffer) noexcept { return input_.read_some(buffer); }

  [[nodiscard]] auto write_some(std::span<std::byte const> data) const noexcept {
    std::string_view const text{reinterpret_cast<char const *>(data.data()), data.size()};

    quoll::fuzz::require(!text.starts_with("HTTP/1.1 500 "));
    return input_.write_some(data);
  }

private:
  quoll::fuzz::input_stream input_;
};

static_assert(quoll::io::ByteStream<checked_stream>);

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  quoll::server::connection_options options;

  // Low enough for an input to reach them.
  options.limits = quoll::fuzz::low_limits;

  auto const serve = [&]() -> quoll::async::task<bool> {
    co_await quoll::server::serve_connection(
        quoll::io::any_stream{checked_stream{quoll::fuzz::as_text(data + 1, size - 1), data[0]}}, quoll::fuzz::routes(),
        options);
    co_return true;
  };

  // With a timer, as a server has one: the timeouts then wrap each read and write.
  quoll::test::virtual_time time;

  quoll::fuzz::require(quoll::test::run(time, serve()).has_value());
  return 0;
}
