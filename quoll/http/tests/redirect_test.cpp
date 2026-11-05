#include <quoll/http/redirect.hpp>

#include <catch2/catch_all.hpp>

using namespace quoll;

TEST_CASE("isRedirect knows every redirect status", "[http]") {
  REQUIRE(http::isRedirect(http::status::moved_permanently));
  REQUIRE(http::isRedirect(http::status::found));
}

TEST_CASE("method_after_redirect keeps the method on 307 and 308, and turns POST into GET on 301, 302 and 303",
          "[http]") {
  REQUIRE(http::method_after_redirect(http::status::see_other, http::verb::post) == http::verb::get);
}

TEST_CASE("redirect_target keeps its location", "[http]") {
  http::redirect_target target{"https://example.com/next"};

  REQUIRE(target.location() == "https://example.com/next");
}
