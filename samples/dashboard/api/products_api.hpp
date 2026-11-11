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

// The contract of the products API, shared by the server and the page: types and endpoints, no I/O.

#ifndef QUOLL_SAMPLES_DASHBOARD_API_PRODUCTS_API_HPP
#define QUOLL_SAMPLES_DASHBOARD_API_PRODUCTS_API_HPP

#include "product.hpp"

#include <quoll/http/verb.hpp>
#include <quoll/rest.hpp>

#include <vector>

namespace products {

namespace http = quoll::http;
namespace rest = quoll::rest;

using list = rest::endpoint<"products.list", http::verb::get, "/", rest::returns<std::vector<product>>>;
using update = rest::endpoint<"products.update", http::verb::put, "/{id:int64}", rest::body<product_update>,
                              rest::returns<product>>;
using sales = rest::endpoint<"products.sales", http::verb::get, "/{id:int64}/sales", rest::returns<monthly_sales>>;

using api = rest::api<"/api/products", list, update, sales>;

} // namespace products

#endif // QUOLL_SAMPLES_DASHBOARD_API_PRODUCTS_API_HPP
