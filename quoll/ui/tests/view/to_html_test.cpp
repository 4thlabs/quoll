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
#include <quoll/ui/view/to_html.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace ui = quoll::ui;

namespace {

struct page {
  ui::state<int> count;
  ui::state<std::vector<std::string>> names;

  ui::element view() {
    using namespace quoll::ui;

    return main_({
        h1("Notes") | tw("text-3xl"),
        p("Count: " + std::to_string(*count)),
        ul({list(names, [](std::string const &name) { return li(name); })}),
        when(*count > 1, [] { return p("Many"); }),
        button("Add") | on_click([this] { count = *count + 1; }),
    });
  }
};

} // namespace

TEST_CASE("to_html: a component's view, from its states, without its handlers", "[ui][html]") {
  page shown{.count = {2}, .names = {{"Ada", "Grace"}}};

  REQUIRE(ui::to_html(shown.view()) == "<main><h1 class=\"text-3xl\">Notes</h1><p>Count: 2</p>"
                                       "<ul><li>Ada</li><li>Grace</li></ul><p>Many</p><button>Add</button></main>");
}

TEST_CASE("to_html: what list() and when() stand for, at the root", "[ui][html]") {
  std::vector<std::string> const names{"a", "b"};

  REQUIRE(ui::to_html(ui::list(names, [](std::string const &name) { return ui::li(name); })) == "<li>a</li><li>b</li>");
  REQUIRE(ui::to_html(ui::when(false, [] { return ui::div(); })).empty());
  REQUIRE(ui::to_html(ui::text("just text")) == "just text");
}

TEST_CASE("to_html: what a user wrote cannot become markup", "[ui][html]") {
  std::string const hostile = R"(<script>alert('x')</script>&"quoted")";
  std::string const escaped = "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;&amp;&quot;quoted&quot;";

  SECTION("in a text") { REQUIRE(ui::to_html(ui::p(hostile)) == "<p>" + escaped + "</p>"); }

  SECTION("in an attribute") {
    REQUIRE(ui::to_html(ui::a("x") | ui::attribute("title", hostile)) == "<a title=\"" + escaped + "\">x</a>");
  }

  SECTION("in the classes") {
    REQUIRE(ui::to_html(ui::div() | ui::tw_if(true, R"(a" onclick="b)")) ==
            "<div class=\"a&quot; onclick=&quot;b\"></div>");
  }

  SECTION("in a value") {
    ui::state<std::string> name{hostile};

    REQUIRE(ui::to_html(ui::input() | ui::bind(name)) == "<input value=\"" + escaped + "\"/>");
  }

  SECTION("in a textarea, whose value is its text") {
    ui::state<std::string> name{hostile};

    REQUIRE(ui::to_html(ui::node("textarea") | ui::bind(name)) == "<textarea>" + escaped + "</textarea>");
  }
}

TEST_CASE("to_html: a name HTML cannot hold is refused", "[ui][html]") {
  for (std::string const tag : {"", "1a", "a b", "a>b", "a<b", "a/b", "script x=y"}) {
    CAPTURE(tag);
    REQUIRE_THROWS_AS(ui::to_html(ui::node(tag)), std::invalid_argument);
  }

  for (std::string const name : {"", "a b", "a=b", "a>b", "a\"b", "a'b", "a/b", "a\nb"}) {
    CAPTURE(name);
    REQUIRE_THROWS_AS(ui::to_html(ui::div() | ui::attribute(name, "x")), std::invalid_argument);
  }

  REQUIRE(ui::to_html(ui::node("my-element") | ui::attribute("data-id", "1") | ui::attribute("aria-label", "x")) ==
          "<my-element aria-label=\"x\" data-id=\"1\"></my-element>");
  REQUIRE(ui::to_html(ui::node("svg:path") | ui::attribute("xlink:href", "#a")) ==
          "<svg:path xlink:href=\"#a\"></svg:path>");
}

TEST_CASE("to_html: an element without an end tag holds nothing", "[ui][html]") {
  REQUIRE(ui::to_html(ui::node("br")) == "<br/>");
  REQUIRE(ui::to_html(ui::node("IMG") | ui::attribute("src", "a.png")) == "<IMG src=\"a.png\"/>");
  REQUIRE_THROWS_AS(ui::to_html(ui::node("br", {ui::text("x")})), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("BR", {ui::div()})), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("param", {ui::text("x")})), std::invalid_argument);
  // Ended by its slash in svg, where it would otherwise stay open and hold what follows.
  REQUIRE(ui::to_html(ui::node("svg", {ui::node("link"), ui::node("circle")})) ==
          "<svg><link/><circle></circle></svg>");
}

TEST_CASE("to_html: script and style keep their text, which cannot end them early", "[ui][html]") {
  REQUIRE(ui::to_html(ui::node("script", {ui::text("if (a < b && c) { d('e'); }")})) ==
          "<script>if (a < b && c) { d('e'); }</script>");
  REQUIRE(ui::to_html(ui::node("STYLE", {ui::text("a > b { color: red; }")})) ==
          "<STYLE>a > b { color: red; }</STYLE>");
  REQUIRE_THROWS_AS(ui::to_html(ui::node("script", {ui::text("</script><img src=x>")})), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("script", {ui::text("</SCRIPT >")})), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("script", {ui::text("<!-- <script>")})), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("style", {ui::text("</style>")})), std::invalid_argument);
  // Elsewhere, the same text is only escaped.
  REQUIRE(ui::to_html(ui::p("</p>")) == "<p>&lt;/p&gt;</p>");
}

TEST_CASE("to_html: what is cut in pieces still cannot end a script early", "[ui][html]") {
  REQUIRE_THROWS_AS(ui::to_html(ui::node("script", {ui::text("<"), ui::text("/script><img src=x onerror=alert(1)>")})),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("script", {ui::text("<!-"), ui::text("-")})), std::invalid_argument);
  REQUIRE(ui::to_html(ui::node("script", {ui::text("a"), ui::text("b")})) == "<script>ab</script>");
}

TEST_CASE("to_html: an element holding only text holds no element", "[ui][html]") {
  // The inner end tag would close the outer element, and the text after it would be markup.
  REQUIRE_THROWS_AS(ui::to_html(ui::node("style", {ui::node("style"), ui::text("<img src=x onerror=alert(1)>")})),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("script", {ui::div()})), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("textarea", {ui::div()})), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::node("title", {ui::div()})), std::invalid_argument);
  REQUIRE(ui::to_html(ui::node("title", {ui::text("a<b")})) == "<title>a&lt;b</title>");
}

TEST_CASE("to_html: no raw text where a parser may read it as markup", "[ui][html]") {
  auto const hostile = ui::text("<img src=x onerror=alert(1)>");

  for (std::string const around : {"svg", "SVG", "math", "select"}) {
    for (std::string const raw : {"style", "script"}) {
      CAPTURE(around, raw);
      REQUIRE_THROWS_AS(ui::to_html(ui::node(around, {ui::node(raw, {hostile})})), std::invalid_argument);
      REQUIRE_THROWS_AS(ui::to_html(ui::node(around, {ui::div({ui::node(raw, {hostile})})})), std::invalid_argument);
    }
  }

  // Empty, it says nothing, and other text there is escaped.
  REQUIRE(ui::to_html(ui::node("svg", {ui::node("style"), hostile})) ==
          "<svg><style></style>&lt;img src=x onerror=alert(1)&gt;</svg>");
}

TEST_CASE("to_html: elements a parser reads by rules of their own are refused", "[ui][html]") {
  for (std::string const tag : {"plaintext", "PLAINTEXT", "xmp", "noembed", "noframes", "frame", "frameset"}) {
    CAPTURE(tag);
    REQUIRE_THROWS_AS(ui::to_html(ui::div({ui::node(tag), ui::p("after")})), std::invalid_argument);
  }

  REQUIRE(ui::to_html(ui::node("iframe") | ui::attribute("src", "a.html")) == "<iframe src=\"a.html\"></iframe>");
  REQUIRE_THROWS_AS(ui::to_html(ui::node("iframe", {ui::text("<img src=x>")})), std::invalid_argument);
}

TEST_CASE("to_html: a first newline a parser would drop is kept", "[ui][html]") {
  ui::state<std::string> name{"\nhello"};

  REQUIRE(ui::to_html(ui::node("textarea") | ui::bind(name)) == "<textarea>\n\nhello</textarea>");
  REQUIRE(ui::to_html(ui::node("pre", {ui::text("\ncode")})) == "<pre>\n\ncode</pre>");
  REQUIRE(ui::to_html(ui::node("listing", {ui::text("\ncode")})) == "<listing>\n\ncode</listing>");
  REQUIRE(ui::to_html(ui::node("pre", {ui::text("code\n")})) == "<pre>code\n</pre>");
  REQUIRE(ui::to_html(ui::node("p", {ui::text("\ntext")})) == "<p>\ntext</p>");
  // Past an empty text.
  REQUIRE(ui::to_html(ui::node("pre", {ui::text(""), ui::text("\ncode")})) == "<pre>\n\ncode</pre>");
}

TEST_CASE("to_html: a textarea shows its value in place of its text", "[ui][html]") {
  ui::state<std::string> name{"typed"};

  REQUIRE(ui::to_html(ui::node("textarea", {ui::text("default")}) | ui::bind(name)) == "<textarea>typed</textarea>");
  REQUIRE(ui::to_html(ui::node("textarea", {ui::text("default")})) == "<textarea>default</textarea>");
  // With a value or without, it holds only text.
  REQUIRE_THROWS_AS(ui::to_html(ui::node("textarea", {ui::div()}) | ui::bind(name)), std::invalid_argument);
}

TEST_CASE("to_html: an attribute given twice is refused, as a parser keeps the other one", "[ui][html]") {
  ui::state<std::string> name{"typed"};

  REQUIRE_THROWS_AS(ui::to_html(ui::div() | ui::tw("a") | ui::attribute("class", "b")), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::input() | ui::attribute("value", "a") | ui::bind(name)), std::invalid_argument);
  REQUIRE_THROWS_AS(ui::to_html(ui::div() | ui::attribute("TITLE", "a") | ui::attribute("title", "b")),
                    std::invalid_argument);
  REQUIRE(ui::to_html(ui::div() | ui::attribute("class", "b")) == "<div class=\"b\"></div>");
}

TEST_CASE("to_html: names are read as ASCII, whatever the bytes", "[ui][html]") {
  REQUIRE_THROWS_AS(ui::to_html(ui::node("caf\xC3\xA9")), std::invalid_argument);
  REQUIRE(ui::to_html(ui::node("SCRIPT", {ui::text("a && b")})) == "<SCRIPT>a && b</SCRIPT>");
}

TEST_CASE("to_html: a carriage return stays one, which a parser would read as a newline", "[ui][html]") {
  REQUIRE(ui::to_html(ui::p("a\r\nb")) == "<p>a&#13;\nb</p>");
  REQUIRE(ui::to_html(ui::div() | ui::attribute("title", "a\rb")) == "<div title=\"a&#13;b\"></div>");
  REQUIRE(ui::to_html(ui::node("pre", {ui::text("\r\ncode")})) == "<pre>&#13;\ncode</pre>");
}

TEST_CASE("to_html: the value of a password stays out of the page", "[ui][html]") {
  ui::state<std::string> secret{"hunter2"};

  REQUIRE(ui::to_html(ui::input() | ui::attribute("type", "password") | ui::bind(secret)) ==
          "<input type=\"password\"/>");
  REQUIRE(ui::to_html(ui::input() | ui::attribute("TYPE", "PassWord") | ui::bind(secret)) ==
          "<input TYPE=\"PassWord\"/>");
  REQUIRE(ui::to_html(ui::input() | ui::attribute("type", "text") | ui::bind(secret)) ==
          "<input value=\"hunter2\" type=\"text\"/>");
}

TEST_CASE("to_html: image is read as img, and holds nothing", "[ui][html]") {
  REQUIRE(ui::to_html(ui::node("svg", {ui::node("image") | ui::attribute("href", "a.png")})) ==
          "<svg><image href=\"a.png\"/></svg>");
  REQUIRE_THROWS_AS(ui::to_html(ui::node("image", {ui::text("caption")})), std::invalid_argument);
}

TEST_CASE("to_html: a fragment stands for its children wherever it is", "[ui][html]") {
  std::vector<std::string> const names{"a", "b"};
  auto holder = ui::ul();

  holder.children.push_back(ui::list(names, [](std::string const &name) { return ui::li(name); }));

  auto const copy = holder;

  REQUIRE(ui::to_html(copy) == "<ul><li>a</li><li>b</li></ul>");
  // The element is read, not taken.
  REQUIRE(copy.children.size() == 1);
}
