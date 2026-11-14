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

// The contract of a few routes of Docker's Engine API (https://docs.docker.com/reference/api/engine/), as Docker has
// them: its paths under the version of the API, its names in PascalCase, its fields as they come, more than the
// contract reads. Shared by the program and the fake Docker its test runs against. No I/O.

#ifndef QUOLL_SAMPLES_DOCKER_API_HPP
#define QUOLL_SAMPLES_DOCKER_API_HPP

#include <quoll/rest.hpp>

#include <glaze/core/common.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace docker {

// A container as GET /containers/json lists it.
struct container_summary {
  std::string id;
  std::vector<std::string> names;
  std::string image;
  std::string state;
  std::string status;
};

struct container_state {
  std::string status;
  bool running = false;
  std::int64_t exit_code = 0;
};

struct container_config {
  std::string image;
  std::vector<std::string> cmd;
};

// A container as GET /containers/{id}/json shows it.
struct container_details {
  std::string id;
  std::string name;
  container_state state;
  container_config config;
};

// What POST /containers/create takes.
struct create_request {
  std::string image;
  std::vector<std::string> cmd;
};

struct created_container {
  std::string id;
  std::vector<std::string> warnings;
};

// What POST /containers/{id}/wait answers once the container has stopped.
struct wait_result {
  std::int64_t status_code = 0;
};

struct version_info {
  std::string version;
  std::string api_version;
  std::string os;
  std::string arch;
};

// What Docker answers on an error, whatever its status.
struct error {
  std::string message;
};

// What GET /containers/json lists, `{"status":["running"]}`: a query parameter in JSON, as Docker reads it.
struct filters {
  std::map<std::string, std::vector<std::string>> by;
};

} // namespace docker

// A filters parameter goes as Docker reads it: JSON in the query, which rest::param_traits says, as for any type of a
// parameter of one's own.
template <>
struct quoll::rest::param_traits<docker::filters> {
  [[nodiscard]] static std::string to_text(docker::filters const &value) { return json_codec{}.encode(value.by); }

  [[nodiscard]] static std::optional<docker::filters> from_text(std::string_view text) {
    auto by = json_codec{}.decode<std::map<std::string, std::vector<std::string>>>(text);

    return by ? std::optional{docker::filters{.by = std::move(*by)}} : std::nullopt;
  }
};

// Docker's names, in PascalCase.
template <>
struct glz::meta<docker::container_summary> {
  using T = docker::container_summary;
  static constexpr auto value =
      glz::object("Id", &T::id, "Names", &T::names, "Image", &T::image, "State", &T::state, "Status", &T::status);
};

template <>
struct glz::meta<docker::container_state> {
  using T = docker::container_state;
  static constexpr auto value = glz::object("Status", &T::status, "Running", &T::running, "ExitCode", &T::exit_code);
};

template <>
struct glz::meta<docker::container_config> {
  using T = docker::container_config;
  static constexpr auto value = glz::object("Image", &T::image, "Cmd", &T::cmd);
};

template <>
struct glz::meta<docker::container_details> {
  using T = docker::container_details;
  static constexpr auto value = glz::object("Id", &T::id, "Name", &T::name, "State", &T::state, "Config", &T::config);
};

template <>
struct glz::meta<docker::create_request> {
  using T = docker::create_request;
  static constexpr auto value = glz::object("Image", &T::image, "Cmd", &T::cmd);
};

template <>
struct glz::meta<docker::created_container> {
  using T = docker::created_container;
  static constexpr auto value = glz::object("Id", &T::id, "Warnings", &T::warnings);
};

template <>
struct glz::meta<docker::wait_result> {
  using T = docker::wait_result;
  static constexpr auto value = glz::object("StatusCode", &T::status_code);
};

template <>
struct glz::meta<docker::version_info> {
  using T = docker::version_info;
  static constexpr auto value =
      glz::object("Version", &T::version, "ApiVersion", &T::api_version, "Os", &T::os, "Arch", &T::arch);
};

namespace docker {

namespace rest = quoll::rest;

// Docker adds fields over time: every result is read leniently.
namespace containers {

using list = rest::get<
    "/containers/json?all={all}&filters={filters}", rest::lenient_json<std::vector<container_summary>>,
    std::optional<bool>, std::optional<filters>>;
using inspect = rest::get<"/containers/{id}/json", rest::lenient_json<container_details>, std::string>;
using create = rest::post<
    "/containers/create?name={name}", rest::created<rest::lenient_json<created_container>>, std::optional<std::string>,
    create_request>;
using start = rest::post<"/containers/{id}/start", void, std::string>;
using stop = rest::post<"/containers/{id}/stop?t={t}", void, std::string, std::optional<int>>;
using wait = rest::post<"/containers/{id}/wait", rest::lenient_json<wait_result>, std::string>;
using remove = rest::del<"/containers/{id}?force={force}", void, std::string, std::optional<bool>>;

using routes = rest::group<list, inspect, create, start, stop, wait, remove>;

} // namespace containers

namespace system {

using version = rest::get<"/version", rest::lenient_json<version_info>>;

} // namespace system

// The version of the API the program speaks, which every path starts with.
using api = rest::api<"/v1.43", containers::routes, system::version>;

} // namespace docker

#endif // QUOLL_SAMPLES_DOCKER_API_HPP
