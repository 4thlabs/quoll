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

// The products service of samples/dashboard, called through a REST client of its contract, over a router of the same
// program: what the page's calls get.

#include "products_api.hpp"
#include "service.hpp"

#include <quoll/http/local_transport.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace {

namespace http = quoll::http;
namespace rest = quoll::rest;

using products_client = rest::client<products::api, http::local_transport>;

// The status a call throws with, or nothing when it does not throw.
template <class Task>
http::status status_of(Task work) {
  try {
    static_cast<void>(quoll::test::run(std::move(work)));
  } catch (rest::call_error const &failure) {
    return failure.status();
  }

  return http::status::ok;
}

} // namespace

TEST_CASE("dashboard service: lists the products, saves a change, and gives a product's sales",
          "[samples][dashboard]") {
  http::router router;

  rest::mount<products::api>(router, products::service{});

  products_client const client{http::local_transport{router}};
  auto const all = quoll::test::run(client.call<products::list>()).value();

  REQUIRE(all.size() == 6);
  CHECK(all.front().name == "Espresso beans");

  auto const saved =
      quoll::test::run(client.call<products::update>(1, {.name = "House beans", .price = 11, .stock = 3})).value();

  CHECK(saved == products::product{.id = 1, .name = "House beans", .category = "Coffee", .price = 11, .stock = 3});
  CHECK(quoll::test::run(client.call<products::list>()).value().front() == saved);

  auto const sales = quoll::test::run(client.call<products::sales>(1)).value();

  REQUIRE(sales.months.size() == 12);
  REQUIRE(sales.units.size() == 12);
  CHECK(sales.months.front() == "Jan");
  CHECK(sales.units.front() == 40);
}

TEST_CASE("dashboard service: refuses an empty name or a negative figure, and an unknown product",
          "[samples][dashboard]") {
  http::router router;

  rest::mount<products::api>(router, products::service{});

  products_client const client{http::local_transport{router}};

  CHECK(status_of(client.call<products::update>(1, {.name = "", .price = 1, .stock = 1})) ==
        http::status::unprocessable_content);
  CHECK(status_of(client.call<products::update>(1, {.name = "Beans", .price = -1, .stock = 1})) ==
        http::status::unprocessable_content);
  CHECK(status_of(client.call<products::update>(99, {.name = "Beans", .price = 1, .stock = 1})) ==
        http::status::not_found);
  CHECK(status_of(client.call<products::sales>(99)) == http::status::not_found);
  CHECK(quoll::test::run(client.call<products::list>()).value().front().name == "Espresso beans");
}
