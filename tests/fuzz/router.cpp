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

// A request target, past whatever a proxy lets through: no handler sees a dot segment or a NUL, whatever the encoding.

#include <quoll/async/task.hpp>
#include <quoll/fuzz/support.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/test/run.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace http = quoll::http;

namespace {

void check_param(std::string_view param) {
  quoll::fuzz::require(!param.contains('\0'));

  while (true) {
    auto const separator = param.find_first_of("/\\");
    auto const part = param.substr(0, separator);

    quoll::fuzz::require(part != "." && part != "..");

    if (separator == std::string_view::npos) {
      return;
    }

    param.remove_prefix(separator + 1);
  }
}

quoll::async::task<http::response> checked(http::request, http::route_match match) {
  for (auto const &param : match.params) {
    check_param(param);
  }

  co_return http::response{};
}

http::router const &routes() {
  static http::router const shared = [] {
    http::router router;

    router.add(http::verb::get, "/users/{id}", "param", checked);
    router.add(http::verb::post, "/users/{id}/cards/{card}", "params", checked);
    router.add(http::verb::get, "/files/{name:path}", "rest", checked);
    router.add(http::verb::get, "/caf%C3%A9", "encoded", checked);
    router.add(http::verb::delete_, "/", "root", checked);
    router.get("/typed/{id}/{ratio}/{flag}/{name}", [](std::int64_t, double, bool, std::string_view name) {
      check_param(name);
      return http::response{};
    });
    return router;
  }();

  return shared;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const *data, std::size_t size) {
  constexpr std::array methods{http::verb::get, http::verb::post, http::verb::head, http::verb::delete_};

  if (size == 0) {
    return 0;
  }

  http::request request;

  request.method = methods[data[0] % methods.size()];
  request.target = std::string{quoll::fuzz::as_text(data + 1, size - 1)};

  auto const response = quoll::test::run(routes().dispatch(std::move(request)));

  quoll::fuzz::require(response.has_value());
  return 0;
}
