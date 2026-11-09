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

#ifndef QUOLL_HTTP_STATUS_HPP
#define QUOLL_HTTP_STATUS_HPP

#include <cstdint>
#include <string_view>

namespace quoll::http {

/**
 * @brief Response status code.
 *
 * Names the common codes of RFC 9110; any other three-digit code is a valid value too
 * (`static_cast<status>(418)`).
 */
enum class status : std::uint16_t {
  continue_ = 100,
  switching_protocols = 101,
  ok = 200,
  created = 201,
  accepted = 202,
  no_content = 204,
  reset_content = 205,
  partial_content = 206,
  moved_permanently = 301,
  found = 302,
  see_other = 303,
  not_modified = 304,
  temporary_redirect = 307,
  permanent_redirect = 308,
  bad_request = 400,
  unauthorized = 401,
  forbidden = 403,
  not_found = 404,
  method_not_allowed = 405,
  request_timeout = 408,
  conflict = 409,
  precondition_failed = 412,
  content_too_large = 413,
  unsupported_media_type = 415,
  range_not_satisfiable = 416,
  expectation_failed = 417,
  unprocessable_content = 422,
  upgrade_required = 426,
  too_many_requests = 429,
  request_header_fields_too_large = 431,
  internal_server_error = 500,
  not_implemented = 501,
  bad_gateway = 502,
  service_unavailable = 503,
  gateway_timeout = 504,
};

/** @brief The reason phrase of RFC 9110 for a named code, empty for the others. */
[[nodiscard]] constexpr std::string_view reason_phrase(status code) noexcept {
  switch (code) {
  case status::continue_:
    return "Continue";
  case status::switching_protocols:
    return "Switching Protocols";
  case status::ok:
    return "OK";
  case status::created:
    return "Created";
  case status::accepted:
    return "Accepted";
  case status::no_content:
    return "No Content";
  case status::reset_content:
    return "Reset Content";
  case status::partial_content:
    return "Partial Content";
  case status::moved_permanently:
    return "Moved Permanently";
  case status::found:
    return "Found";
  case status::see_other:
    return "See Other";
  case status::not_modified:
    return "Not Modified";
  case status::temporary_redirect:
    return "Temporary Redirect";
  case status::permanent_redirect:
    return "Permanent Redirect";
  case status::bad_request:
    return "Bad Request";
  case status::unauthorized:
    return "Unauthorized";
  case status::forbidden:
    return "Forbidden";
  case status::not_found:
    return "Not Found";
  case status::method_not_allowed:
    return "Method Not Allowed";
  case status::request_timeout:
    return "Request Timeout";
  case status::conflict:
    return "Conflict";
  case status::precondition_failed:
    return "Precondition Failed";
  case status::content_too_large:
    return "Content Too Large";
  case status::unsupported_media_type:
    return "Unsupported Media Type";
  case status::range_not_satisfiable:
    return "Range Not Satisfiable";
  case status::expectation_failed:
    return "Expectation Failed";
  case status::unprocessable_content:
    return "Unprocessable Content";
  case status::upgrade_required:
    return "Upgrade Required";
  case status::too_many_requests:
    return "Too Many Requests";
  case status::request_header_fields_too_large:
    return "Request Header Fields Too Large";
  case status::internal_server_error:
    return "Internal Server Error";
  case status::not_implemented:
    return "Not Implemented";
  case status::bad_gateway:
    return "Bad Gateway";
  case status::service_unavailable:
    return "Service Unavailable";
  case status::gateway_timeout:
    return "Gateway Timeout";
  }

  return {};
}

/** @brief True for 1xx codes: interim responses, followed by the final one. */
[[nodiscard]] constexpr bool is_informational(status code) noexcept {
  auto const value = static_cast<std::uint16_t>(code);

  return value >= 100 && value < 200;
}

} // namespace quoll::http

#endif // QUOLL_HTTP_STATUS_HPP
