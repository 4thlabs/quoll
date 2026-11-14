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

// A command-line program calling the Docker Engine through its contract (docker_api.hpp), over its Unix domain socket:
//
//   quoll_docker version                 the engine's version
//   quoll_docker ps [-a] [-f key=value]  the containers, running or all, those a filter of Docker's keeps
//   quoll_docker inspect <container>     one container
//   quoll_docker run <image> [cmd...]    creates a container, starts it, waits for it, shows what it wrote, removes it:
//                                        its exit code
//   quoll_docker events [since]          what happened on the engine since then (seconds), up to now
//   quoll_docker stop <container>
//   quoll_docker rm <container>
//
// It calls the engine at DOCKER_HOST, as Docker's own client does, `unix:///var/run/docker.sock` by default, or
// `tcp://host:2375`.

#include "docker_api.hpp"

#include <quoll/async.hpp>
#include <quoll/http/http_error.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;
namespace rest = quoll::rest;

using namespace quoll::rest::literals;

// Copied freely: its transport is shared, so that a coroutine takes it by value.
using docker_client = rest::client<docker::api>;

// What Docker said of an error, in its own shape, or what the client saw of it.
[[nodiscard]] std::string message_of(http::http_error const &error) {
  if (auto const said = error.body_as<docker::error>(rest::lenient_json_codec{})) {
    return said->message;
  }

  return error.what();
}

// An error of the engine's, with its status: 1 as the program's status.
int report(http::http_error const &error) {
  std::cerr << "error " << static_cast<int>(error.status()) << ": " << message_of(error) << '\n';
  return 1;
}

async::task<int> show_version(docker_client client) {
  auto const version = co_await client.try_call<docker::system::version>();

  if (!version) {
    co_return report(version.error());
  }

  std::cout << "Docker " << version->version << " (API " << version->api_version << "), " << version->os << '/'
            << version->arch << '\n';
  co_return 0;
}

async::task<int> list(docker_client client, bool all, std::optional<docker::filters> kept) {
  auto const listed = co_await client.try_call<docker::containers::list>("all"_p = all, "filters"_p = std::move(kept));

  if (!listed) {
    co_return report(listed.error());
  }

  for (auto const &container : *listed) {
    auto const name = container.names.empty() ? std::string{} : container.names.front();

    std::cout << container.id.substr(0, 12) << ' ' << container.image << ' ' << container.state << ' ' << name << '\n';
  }

  co_return 0;
}

async::task<int> inspect(docker_client client, std::string container) {
  auto const found = co_await client.try_call<docker::containers::inspect>(std::move(container));

  if (!found) {
    co_return report(found.error());
  }

  std::cout << found->name << ": " << found->config.image << ", " << found->state.status << '\n';
  co_return 0;
}

// Starts the container `id`, waits for it and shows what it wrote: the exit code of its command.
async::task<int> run_created(docker_client client, std::string id) {
  if (auto const started = co_await client.try_call<docker::containers::start>(id); !started) {
    co_return report(started.error());
  }

  auto const waited = co_await client.try_call<docker::containers::wait>(id);

  if (!waited) {
    co_return report(waited.error());
  }

  // What it wrote, as Docker's frames: its stdout to ours, its stderr and the engine's errors to ours.
  try {
    auto logs = co_await client.subscribe<docker::containers::logs>("id"_p = id, "stdout"_p = true, "stderr"_p = true);

    while (auto const frame = co_await logs.next()) {
      (frame->stream == 1 ? std::cout : std::cerr) << frame->text;
    }
  } catch (http::http_error const &failed) {
    co_return report(failed);
  }

  std::cout << id.substr(0, 12) << " exited with " << waited->status_code << '\n';
  co_return static_cast<int>(waited->status_code);
}

// Creates a container, runs it, and removes it whatever happened once it was made, as `docker run --rm` does.
async::task<int> run_container(docker_client client, std::string image, std::vector<std::string> command) {
  auto const made = co_await client.try_call<docker::containers::create>(
      std::nullopt, docker::create_request{.image = std::move(image), .cmd = std::move(command)}
  );

  if (!made) {
    co_return report(made.error());
  }

  auto const status = co_await run_created(client, made->id);

  if (auto const removed = co_await client.try_call<docker::containers::remove>(made->id, true); !removed) {
    co_return report(removed.error());
  }

  co_return status;
}

// What happened on the engine from `since`, up to now: Docker ends the stream at `until`.
async::task<int> show_events(docker_client client, std::int64_t since, std::int64_t until) {
  auto happened = co_await client.subscribe<docker::system::events>("since"_p = since, "until"_p = until);

  while (auto const event = co_await happened.next()) {
    auto const name = event->from.attributes.contains("name") ? event->from.attributes.at("name") : event->from.id;

    std::cout << event->type << ' ' << event->action << ' ' << name << '\n';
  }

  co_return 0;
}

async::task<int> stop(docker_client client, std::string container) {
  if (auto const stopped = co_await client.try_call<docker::containers::stop>("id"_p = std::move(container));
      !stopped) {
    co_return report(stopped.error());
  }

  co_return 0;
}

async::task<int> remove(docker_client client, std::string container) {
  if (auto const removed = co_await client.try_call<docker::containers::remove>(std::move(container), false);
      !removed) {
    co_return report(removed.error());
  }

  co_return 0;
}

// What the program is given: its arguments, and the engine's URL, read from its environment.
struct given {
  std::vector<std::string> arguments;
  std::string host;
};

async::task<int> run(given what) {
  auto const &arguments = what.arguments;

  if (arguments.empty()) {
    std::cerr << "usage: quoll_docker version | ps [-a] | inspect <c> | run <image> [cmd...] | stop <c> | rm <c>\n";
    co_return 2;
  }

  docker_client const client{std::move(what.host)};
  auto const &command = arguments[0];

  if (command == "version") {
    co_return co_await show_version(client);
  }

  if (command == "events") {
    auto const now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    auto const since = arguments.size() > 1 ? std::stoll(arguments[1]) : now.count() - 60;

    co_return co_await show_events(client, since, now.count());
  }

  if (command == "ps") {
    auto all = false;
    std::optional<docker::filters> kept;

    for (std::size_t at = 1; at < arguments.size(); ++at) {
      auto const equal = at + 1 < arguments.size() ? arguments[at + 1].find('=') : std::string::npos;

      if (arguments[at] == "-a") {
        all = true;
      } else if (arguments[at] == "-f" && equal != std::string::npos) {
        auto const &filter = arguments[++at];

        kept.emplace().by[filter.substr(0, equal)].push_back(filter.substr(equal + 1));
      } else {
        std::cerr << "ps: -a or -f key=value, not " << arguments[at] << '\n';
        co_return 2;
      }
    }

    co_return co_await list(client, all, std::move(kept));
  }

  if (arguments.size() < 2) {
    std::cerr << command << ": what container or image?\n";
    co_return 2;
  }

  if (command == "inspect") {
    co_return co_await inspect(client, arguments[1]);
  }

  if (command == "run") {
    co_return co_await run_container(
        client, arguments[1], std::vector<std::string>(arguments.begin() + 2, arguments.end())
    );
  }

  if (command == "stop") {
    co_return co_await stop(client, arguments[1]);
  }

  if (command == "rm") {
    co_return co_await remove(client, arguments[1]);
  }

  std::cerr << "unknown command " << command << '\n';
  co_return 2;
}

int exit_status = 1;

struct record_status {
  void operator()(int code) const { exit_status = code; }

  void operator()(std::exception_ptr const &failure) const {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      std::cerr << "failed: " << error.what() << '\n';
    } catch (...) {
      std::cerr << "failed\n";
    }

    exit_status = 1;
  }

  void operator()(async::stopped_t) const {
    std::cerr << "failed: stopped\n";
    exit_status = 1;
  }
};

} // namespace

int main(int argc, char **argv) {
  quoll::runtime runtime;
  auto const *set = std::getenv("DOCKER_HOST");
  std::string host = set != nullptr && *set != '\0' ? set : "unix:///var/run/docker.sock";

  // Docker writes an engine over TCP tcp://host:2375, which is plain HTTP.
  if (host.starts_with("tcp://")) {
    host = "http://" + host.substr(6);
  }

  given what{.arguments = std::vector<std::string>(argv + 1, argv + argc), .host = std::move(host)};

  co_spawn(runtime.get_executor(), run(std::move(what)), record_status{});
  runtime.run();
  return exit_status;
}
