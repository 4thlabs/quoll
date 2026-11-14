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

#include <cstddef>
#include <cstdint>
#include <expected>
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

// What happens to an object of the engine, as GET /events streams it, a value per line of JSON.
struct actor {
  std::string id;
  std::map<std::string, std::string> attributes;
};

struct event {
  std::string type;
  std::string action;
  actor from;
  std::int64_t time = 0;
};

// A piece of what a container wrote, as GET /containers/{id}/logs sends it: on its stdout (1) or its stderr (2), or an
// error of the engine's within the stream (3).
struct log_frame {
  std::uint8_t stream = 1;
  std::string text;
};

// Docker's frames of logs, for a container with no TTY: 8 bytes of head, the stream and the size of the text in big
// endian, then the text. A Framing of the sample's own, as rest::stream<F> takes any.
struct log_frames {
  using value_type = log_frame;

  static constexpr std::string_view content_type = "application/vnd.docker.multiplexed-stream";
  static constexpr std::size_t head_size = 8;

  [[nodiscard]] std::string encode(log_frame const &frame) const {
    auto const size = static_cast<std::uint32_t>(frame.text.size());
    std::string bytes(head_size, '\0');

    bytes[0] = static_cast<char>(frame.stream);
    bytes[4] = static_cast<char>((size >> 24U) & 0xFFU);
    bytes[5] = static_cast<char>((size >> 16U) & 0xFFU);
    bytes[6] = static_cast<char>((size >> 8U) & 0xFFU);
    bytes[7] = static_cast<char>(size & 0xFFU);
    return bytes + frame.text;
  }

  [[nodiscard]] std::expected<std::optional<quoll::rest::framed<log_frame>>, quoll::rest::decode_error>
  decode(std::string_view pending) const {
    if (pending.size() < head_size) {
      return std::optional<quoll::rest::framed<log_frame>>{};
    }

    auto const stream = static_cast<std::uint8_t>(pending[0]);

    // 1 stdout, 2 stderr, 3 an error of the engine's: anything else is no frame of Docker's.
    if (stream > 3) {
      return std::unexpected{quoll::rest::decode_error{"not a frame of Docker's logs"}};
    }

    std::size_t size = 0;

    for (std::size_t at = 4; at < head_size; ++at) {
      size = (size << 8U) | static_cast<std::uint8_t>(pending[at]);
    }

    if (pending.size() < head_size + size) {
      return std::optional<quoll::rest::framed<log_frame>>{};
    }

    return std::optional{quoll::rest::framed<log_frame>{
        {.stream = stream, .text = std::string{pending.substr(head_size, size)}}, head_size + size
    }};
  }
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
struct glz::meta<docker::actor> {
  using T = docker::actor;
  static constexpr auto value = glz::object("ID", &T::id, "Attributes", &T::attributes);
};

template <>
struct glz::meta<docker::event> {
  using T = docker::event;
  static constexpr auto value =
      glz::object("Type", &T::type, "Action", &T::action, "Actor", &T::from, "time", &T::time);
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
// What it wrote, as Docker's frames: until now, or as it comes with follow.
using logs = rest::get<
    "/containers/{id}/logs?stdout={stdout}&stderr={stderr}&follow={follow}", rest::stream<log_frames>, std::string,
    std::optional<bool>, std::optional<bool>, std::optional<bool>>;

using routes = rest::group<list, inspect, create, start, stop, wait, remove, logs>;

} // namespace containers

namespace system {

using version = rest::get<"/version", rest::lenient_json<version_info>>;
// What happens on the engine, a value per line of JSON, between two times in seconds; with no end, as it happens.
using events = rest::get<
    "/events?since={since}&until={until}", rest::json_lines<event, rest::lenient_json_codec>,
    std::optional<std::int64_t>, std::optional<std::int64_t>>;

using routes = rest::group<version, events>;

} // namespace system

// The version of the API the program speaks, which every path starts with.
using api = rest::api<"/v1.43", containers::routes, system::routes>;

} // namespace docker

#endif // QUOLL_SAMPLES_DOCKER_API_HPP
