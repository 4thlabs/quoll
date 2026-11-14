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

// expect: quoll::rest::query_params: the struct has more than 64 fields
//
// query_params<> of a struct past what its fields are read by name for.

#include <quoll/rest.hpp>

namespace rest = quoll::rest;

struct filter {
  int f0 = 0;
  int f1 = 0;
  int f2 = 0;
  int f3 = 0;
  int f4 = 0;
  int f5 = 0;
  int f6 = 0;
  int f7 = 0;
  int f8 = 0;
  int f9 = 0;
  int f10 = 0;
  int f11 = 0;
  int f12 = 0;
  int f13 = 0;
  int f14 = 0;
  int f15 = 0;
  int f16 = 0;
  int f17 = 0;
  int f18 = 0;
  int f19 = 0;
  int f20 = 0;
  int f21 = 0;
  int f22 = 0;
  int f23 = 0;
  int f24 = 0;
  int f25 = 0;
  int f26 = 0;
  int f27 = 0;
  int f28 = 0;
  int f29 = 0;
  int f30 = 0;
  int f31 = 0;
  int f32 = 0;
  int f33 = 0;
  int f34 = 0;
  int f35 = 0;
  int f36 = 0;
  int f37 = 0;
  int f38 = 0;
  int f39 = 0;
  int f40 = 0;
  int f41 = 0;
  int f42 = 0;
  int f43 = 0;
  int f44 = 0;
  int f45 = 0;
  int f46 = 0;
  int f47 = 0;
  int f48 = 0;
  int f49 = 0;
  int f50 = 0;
  int f51 = 0;
  int f52 = 0;
  int f53 = 0;
  int f54 = 0;
  int f55 = 0;
  int f56 = 0;
  int f57 = 0;
  int f58 = 0;
  int f59 = 0;
  int f60 = 0;
  int f61 = 0;
  int f62 = 0;
  int f63 = 0;
  int f64 = 0;
};

using route = rest::get<"/", void, rest::query_params<filter>>;

inline constexpr rest::api<"", rest::group<route>> instance{};
