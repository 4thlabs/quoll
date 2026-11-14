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

// A fake Docker Engine for the test of quoll_docker: the same contract, served on a Unix domain socket, with containers
// in memory, answering its errors as Docker does, `{"message": ...}`.
//
//   quoll_fake_docker <path>     listens on a socket at <path>, and prints its URL once it does

#include "docker_api.hpp"

#include <quoll/async.hpp>
#include <quoll/http/http_error.hpp>
#include <quoll/http/next.hpp>
#include <quoll/http/problem.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/io/listen_local.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;
namespace rest = quoll::rest;

struct container {
  std::string name;
  std::string image;
  std::vector<std::string> cmd;
  std::string status = "created";
};

// The engine's containers, by id, and what happened to them. The runtime runs one thread.
struct engine_state {
  std::map<std::string, container> containers;
  std::vector<docker::event> happened;
  std::uint64_t made = 0;

  void record(std::string action, std::string const &id, container const &of) {
    auto const now = std::chrono::system_clock::now().time_since_epoch();

    happened.push_back(
        {.type = "container",
         .action = std::move(action),
         .from = {.id = id, .attributes = {{"name", of.name.substr(1)}, {"image", of.image}}},
         .time = std::chrono::duration_cast<std::chrono::seconds>(now).count()}
    );
  }
};

[[noreturn]] void no_such_container(std::string const &id) {
  throw http::http_error{http::status::not_found, "No such container: " + id};
}

struct docker_engine {
  std::shared_ptr<engine_state> state = std::make_shared<engine_state>();

  // A container by its id, the start of it, or its name.
  [[nodiscard]] std::pair<std::string const, container> &find(std::string const &id) const {
    for (auto &entry : state->containers) {
      if (entry.first.starts_with(id) || entry.second.name == "/" + id) {
        return entry;
      }
    }

    no_such_container(id);
  }

  docker::version_info operator()(docker::system::version) const {
    return {.version = "24.0.7", .api_version = "1.43", .os = "linux", .arch = "amd64"};
  }

  std::vector<docker::container_summary>
  operator()(docker::containers::list, std::optional<bool> all, std::optional<docker::filters> kept) const {
    std::vector<docker::container_summary> listed;
    // Of Docker's filters, the status, which the test asks for.
    auto const statuses = kept && kept->by.contains("status") ? kept->by.at("status") : std::vector<std::string>{};

    for (auto const &[id, made] : state->containers) {
      auto const wanted = statuses.empty() ? all.value_or(false) || made.status == "running"
                                           : std::ranges::find(statuses, made.status) != statuses.end();

      if (wanted) {
        listed.push_back(
            {.id = id, .names = {made.name}, .image = made.image, .state = made.status, .status = made.status}
        );
      }
    }

    return listed;
  }

  docker::container_details operator()(docker::containers::inspect, std::string id) const {
    auto const &[full, found] = find(id);

    return {
        .id = full,
        .name = found.name,
        .state = {.status = found.status, .running = found.status == "running", .exit_code = 0},
        .config = {.image = found.image, .cmd = found.cmd}
    };
  }

  docker::created_container
  operator()(docker::containers::create, std::optional<std::string> name, docker::create_request request) const {
    if (request.image == "missing") {
      throw http::http_error{http::status::not_found, "No such image: missing:latest"};
    }

    auto const id = std::string(56, '0') + std::to_string(10000000 + ++state->made);
    auto const named = "/" + name.value_or("quoll_" + std::to_string(state->made));

    auto const &made =
        state->containers.emplace(id, container{.name = named, .image = request.image, .cmd = request.cmd})
            .first->second;

    state->record("create", id, made);
    return {.id = id, .warnings = {}};
  }

  void operator()(docker::containers::start, std::string id) const {
    auto &[full, found] = find(id);

    found.status = "running";
    state->record("start", full, found);
  }

  void operator()(docker::containers::stop, std::string id, std::optional<int>) const {
    find(id).second.status = "exited";
  }

  // Its command has run: false fails, as the shell's does, anything else succeeds.
  docker::wait_result operator()(docker::containers::wait, std::string id) const {
    auto &found = find(id).second;

    found.status = "exited";
    return {.status_code = !found.cmd.empty() && found.cmd.front() == "false" ? 1 : 0};
  }

  void operator()(docker::containers::remove, std::string id, std::optional<bool> force) const {
    auto &entry = find(id);

    if (entry.second.status == "running" && !force.value_or(false)) {
      throw http::http_error{
          http::status::conflict, "You cannot remove a running container " + entry.first.substr(0, 12)
      };
    }

    state->record("destroy", entry.first, entry.second);
    state->containers.erase(entry.first);
  }

  // What its command wrote, as the shell would: echo its arguments, on its stdout.
  async::task<> operator()(
      docker::containers::logs, std::string id, std::optional<bool> stdout_wanted, std::optional<bool>,
      std::optional<bool>, rest::frame_sink<docker::log_frames> &out
  ) const {
    auto const cmd = find(id).second.cmd;

    if (stdout_wanted.value_or(false) && !cmd.empty() && cmd.front() == "echo") {
      std::string line;

      for (std::size_t at = 1; at < cmd.size(); ++at) {
        line += (at > 1 ? " " : "") + cmd[at];
      }

      co_await out.send({.stream = 1, .text = line + "\n"});
    }
  }

  // What happened between two times, a value per line of JSON; then the stream ends, as Docker's does at `until`.
  async::task<> operator()(
      docker::system::events, std::optional<std::int64_t> since, std::optional<std::int64_t> until,
      rest::frame_sink<rest::json_lines_framing<docker::event, rest::lenient_json_codec>> &out
  ) const {
    auto const happened = state->happened;

    co_await out.open();

    for (auto const &event : happened) {
      if (event.time >= since.value_or(0) && event.time <= until.value_or(event.time)) {
        co_await out.send(event);
      }
    }
  }
};

// Docker's own shape of its errors, `{"message": ...}`, from the problem a handler's http_error answers.
async::task<http::response> as_docker_answers(http::request &&request, http::next const &next) {
  auto response = co_await next(std::move(request));

  if (response.fields.find("Content-Type") != http::problem_content_type) {
    co_return response;
  }

  auto const problem = rest::lenient_json_codec{}.decode<http::problem>(response.body);

  response.body = rest::json_codec{}.encode(docker::error{.message = problem ? problem->detail.value_or("") : ""});
  response.fields.set("Content-Type", "application/json");
  response.fields.set("Content-Length", std::to_string(response.body.size()));
  co_return response;
}

async::task<> serve(std::string path) {
  http::router router;

  router.use(as_docker_answers);
  rest::mount<docker::api>(router, docker_engine{});

  // A socket left by an earlier run would refuse the bind.
  std::filesystem::remove(path);

  auto listener = co_await quoll::io::listen_local(path);

  std::cout << "fake Docker on unix://" << path << std::endl;
  co_await http::server{std::move(router)}.serve(std::move(listener));
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: quoll_fake_docker <path of the socket>\n";
    return 2;
  }

  quoll::runtime runtime;

  return runtime.run(serve(argv[1]));
}
