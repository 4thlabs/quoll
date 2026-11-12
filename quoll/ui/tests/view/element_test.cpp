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

#include <quoll/ui/data/state.hpp>
#include <quoll/ui/view/decorators.hpp>
#include <quoll/ui/view/element.hpp>
#include <quoll/ui/view/tags.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

using namespace quoll::ui;

} // namespace

TEST_CASE("ui: a tag holds its children, moved rather than copied", "[ui]") {
  auto const tree = div({
      h1("Notes"),
      span({text("a"), text("b")}),
  });

  REQUIRE(tree.tag == "div");
  REQUIRE(tree.children.size() == 2);
  REQUIRE(tree.children[0].tag == "h1");
  REQUIRE(tree.children[0].children.at(0).content == "Notes");
  REQUIRE(tree.children[1].children.size() == 2);
}

TEST_CASE("ui: decorators complete an element, in order", "[ui]") {
  auto const styled = div() | tw("flex") | tw("gap-2") | tw_if(true, "p-4") | tw_if(false, "hidden") | key("first") |
                      attribute("id", "main");

  REQUIRE(styled.classes == "flex gap-2 p-4");
  REQUIRE(styled.key == "first");
  REQUIRE(styled.attributes.at("id") == "main");
  REQUIRE((div() | key(42)).key == "42");
}

TEST_CASE("ui: a decorator can be written by anyone", "[ui]") {
  auto const primary = [] { return decorator([](element &target) { target.classes = "btn btn-primary"; }); };

  REQUIRE((button("Go") | primary()).classes == "btn btn-primary");
}

TEST_CASE("ui: list() and when() stand for their children in the parent", "[ui]") {
  std::vector<int> const numbers{1, 2};

  auto const tree = ul({
      li("first"),
      list(numbers, [](int number) { return li(std::to_string(number)) | key(number); }),
      when(true, [] { return li("shown"); }),
      when(false, [] { return li("hidden"); }),
      li("last"),
  });

  REQUIRE(tree.children.size() == 5);
  REQUIRE(tree.children[1].key == "1");
  REQUIRE(tree.children[3].children.at(0).content == "shown");
  REQUIRE(tree.children[4].children.at(0).content == "last");
}

TEST_CASE("ui: list() takes the value of a state", "[ui]") {
  state<std::vector<int>> numbers{std::vector<int>{1, 2, 3}};

  REQUIRE(list(numbers, [](int number) { return li(std::to_string(number)); }).children.size() == 3);
}

TEST_CASE("ui: a state holds its value, and tells of what changes it", "[ui]") {
  int changes = 0;
  state<int> count{1};

  count.link({&changes, [](void *owner) { ++*static_cast<int *>(owner); }});

  REQUIRE(*count == 1);

  count = 2;
  count.set(3);
  count.update([](int &value) { value += 1; });
  REQUIRE(*count == 4);
  REQUIRE(changes == 3);
}
