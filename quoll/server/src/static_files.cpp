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

#include <quoll/server/static_files.hpp>

#include <quoll/async/offload.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/number.hpp>
#include <quoll/http/detail/percent.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/next.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace quoll::server {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t piece_size = std::size_t{64} * 1024;

struct served {
  std::string prefix;
  fs::path root;
  static_files_options options;
};

// The file of a response, for as long as it is being written. Thread safety: none needed, as it is read through one
// operation at a time and pread() takes no position of its own.
class open_file {
public:
  explicit open_file(int descriptor) noexcept : descriptor_(descriptor) {}

  open_file(open_file const &) = delete;
  open_file(open_file &&) = delete;
  open_file &operator=(open_file const &) = delete;
  open_file &operator=(open_file &&) = delete;

  ~open_file() { ::close(descriptor_); }

  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

private:
  int descriptor_;
};

struct found_file {
  std::shared_ptr<open_file> file;
  std::uint64_t size = 0;
  std::time_t modified = 0;
  std::string etag;
};

[[nodiscard]] std::uint64_t nanoseconds_of(struct stat const &status) noexcept {
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(status.st_mtimespec.tv_nsec);
#else
  return static_cast<std::uint64_t>(status.st_mtim.tv_nsec);
#endif
}

void append_hex(std::uint64_t value, std::string &out) {
  std::array<char, 16> digits{};
  auto const [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);

  static_cast<void>(error);
  out.append(digits.data(), end);
}

// Whether `error` says that there is nothing to serve, as a 404 does. Any other says that the server is in trouble, out
// of descriptors for instance, and a 404 of that would be taken for the truth, and perhaps kept in a cache.
[[nodiscard]] bool nothing_there(std::error_code error) noexcept {
  return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory ||
         error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
         error == std::errc::too_many_symbolic_link_levels || error == std::errc::filename_too_long;
}

// What `path` opens to, if it is a regular file. `path` has had its links followed: O_NOFOLLOW keeps a link put there
// since from taking it elsewhere, and O_NONBLOCK keeps opening a FIFO from waiting for a writer.
[[nodiscard]] std::optional<found_file> open_regular(fs::path const &path) {
  auto const descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);

  if (descriptor < 0) {
    std::error_code const error{errno, std::generic_category()};

    if (nothing_there(error)) {
      return std::nullopt;
    }

    throw std::system_error{error, "quoll::server::static_files"};
  }

  auto file = std::make_shared<open_file>(descriptor);

  struct stat status {};

  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    return std::nullopt;
  }

  found_file found{std::move(file), static_cast<std::uint64_t>(status.st_size), status.st_mtime, "\""};

  append_hex(found.size, found.etag);
  found.etag += '-';
  append_hex(static_cast<std::uint64_t>(status.st_mtime), found.etag);
  found.etag += '.';
  append_hex(nanoseconds_of(status), found.etag);
  found.etag += '"';
  return found;
}

// `path` once its links are followed, if that is still under `root`, which already has its own followed.
[[nodiscard]] std::optional<fs::path> inside_root(fs::path const &root, fs::path const &path) {
  std::error_code error;
  auto resolved = fs::canonical(path, error);

  if (error) {
    if (nothing_there(error)) {
      return std::nullopt;
    }

    throw std::system_error{error, "quoll::server::static_files"};
  }

  auto const [in_root, in_path] = std::ranges::mismatch(root, resolved);

  if (in_root != root.end()) {
    return std::nullopt;
  }

  return resolved;
}

// What a request finds on the disk.
struct looked_up {
  std::optional<found_file> file{};
  // A directory with an index, asked for without its final slash.
  bool redirect = false;
};

[[nodiscard]] looked_up look_up(served const &state, fs::path const &relative, bool ends_with_slash) {
  auto const path = inside_root(state.root, state.root / relative);

  if (!path) {
    return {};
  }

  std::error_code error;

  if (!fs::is_directory(*path, error)) {
    return error || ends_with_slash ? looked_up{} : looked_up{open_regular(*path)};
  }

  if (state.options.index.empty()) {
    return {};
  }

  auto const index = inside_root(state.root, *path / state.options.index);

  if (!index || !fs::is_regular_file(*index, error)) {
    return {};
  }

  if (!ends_with_slash) {
    return {.redirect = true};
  }

  return {.file = open_regular(*index)};
}

// The path relative to `root` that `path`, as next::path() has it, asks for: nothing for what cannot be a file of it.
// An encoded slash is a character of its segment, which a file name cannot hold, and a percent sign is the only other
// thing encoded there.
[[nodiscard]] std::optional<fs::path> relative_path(std::string_view path, bool dotfiles) {
  fs::path relative;

  while (!path.empty()) {
    auto const slash = path.find('/');
    auto const raw = path.substr(0, slash);

    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);

    // What a final slash leaves is no segment: this is `a//b`, `a//` or a leading `//`.
    if (raw.empty()) {
      return std::nullopt;
    }

    std::string segment;

    for (std::size_t at = 0; at < raw.size(); ++at) {
      if (raw[at] != '%') {
        segment += raw[at];
        continue;
      }

      if (raw.substr(at, 3) != "%25") {
        return std::nullopt;
      }

      segment += '%';
      at += 2;
    }

    if (segment.contains('\\') || segment == "." || segment == ".." || (!dotfiles && segment.starts_with('.'))) {
      return std::nullopt;
    }

    relative /= segment;
  }

  return relative;
}

[[nodiscard]] std::string http_date(std::time_t time) {
  static constexpr std::array<char const *, 7> days{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static constexpr std::array<char const *, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::tm parts{};
  std::array<char, 40> text{};

  ::gmtime_r(&time, &parts);

  auto const size = std::snprintf(text.data(), text.size(), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                                  days.at(static_cast<std::size_t>(parts.tm_wday)), parts.tm_mday,
                                  months.at(static_cast<std::size_t>(parts.tm_mon)), parts.tm_year + 1900,
                                  parts.tm_hour, parts.tm_min, parts.tm_sec);

  return std::string(text.data(), static_cast<std::size_t>(size));
}

// The time of an HTTP date in its only current form, the one http_date() writes: what is not, a peer's invention above
// all, is ignored, as RFC 9110 lets a recipient do with a date it cannot read.
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view text) {
  static constexpr std::string_view months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  static constexpr std::string_view form = "Sun, 06 Nov 1994 08:49:37 GMT";

  if (text.size() != form.size()) {
    return std::nullopt;
  }

  auto const date = http::detail::parse_number<int>(text.substr(5, 2));
  auto const month = months.find(text.substr(8, 3));
  auto const year = http::detail::parse_number<int>(text.substr(12, 4));
  auto const hour = http::detail::parse_number<int>(text.substr(17, 2));
  auto const minute = http::detail::parse_number<int>(text.substr(20, 2));
  auto const second = http::detail::parse_number<int>(text.substr(23, 2));

  if (!date || month == std::string_view::npos || month % 3 != 0 || !year || !hour || !minute || !second) {
    return std::nullopt;
  }

  std::tm parts{};

  parts.tm_year = *year - 1900;
  parts.tm_mon = static_cast<int>(month / 3);
  parts.tm_mday = *date;
  parts.tm_hour = *hour;
  parts.tm_min = *minute;
  parts.tm_sec = *second;

  auto const time = ::timegm(&parts);

  // What it would write itself, or it is not one: 99 January would otherwise move on to April, and any weekday would
  // do.
  return http_date(time) == text ? std::optional{time} : std::nullopt;
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept { return http::detail::trim(text); }

// RFC 9110, 13.1.2: the weak comparison, over a list, or `*`.
[[nodiscard]] bool none_match_fails(std::string_view list, std::string_view etag) {
  for (auto const member : std::views::split(list, ',')) {
    auto tag = trimmed(std::string_view{member.begin(), member.end()});

    if (tag == "*") {
      return true;
    }

    if (tag.starts_with("W/")) {
      tag.remove_prefix(2);
    }

    if (tag == etag) {
      return true;
    }
  }

  return false;
}

// What a request asks for of a file, as read before it is passed on.
struct conditions {
  std::optional<std::string> if_none_match{};
  std::optional<std::string> if_modified_since{};
  std::optional<std::string> range{};
  std::optional<std::string> if_range{};
};

[[nodiscard]] bool not_modified(conditions const &asked, found_file const &found) {
  // It takes over the date, which is less exact, when both are there.
  if (asked.if_none_match) {
    return none_match_fails(*asked.if_none_match, found.etag);
  }

  if (asked.if_modified_since) {
    auto const since = parse_http_date(*asked.if_modified_since);

    return since && found.modified <= *since;
  }

  return false;
}

struct byte_range {
  std::uint64_t first = 0;
  std::uint64_t length = 0;
};

enum class range_outcome { whole, part, out_of_file };

// RFC 9110, 14.2: one interval. What cannot be read, several intervals among it, is as if it were not there.
[[nodiscard]] range_outcome parse_range(std::string_view header, std::uint64_t size, byte_range &range) {
  header = trimmed(header);

  if (!header.starts_with("bytes=")) {
    return range_outcome::whole;
  }

  header.remove_prefix(6);

  auto const dash = header.find('-');

  if (dash == std::string_view::npos) {
    return range_outcome::whole;
  }

  auto const first = trimmed(header.substr(0, dash));
  auto const last = trimmed(header.substr(dash + 1));

  // The last `n` bytes.
  if (first.empty()) {
    auto const suffix = http::detail::parse_number<std::uint64_t>(last);

    if (!suffix) {
      return range_outcome::whole;
    }

    if (*suffix == 0 || size == 0) {
      return range_outcome::out_of_file;
    }

    auto const length = std::min(*suffix, size);

    range = {size - length, length};
    return range_outcome::part;
  }

  auto const start = http::detail::parse_number<std::uint64_t>(first);

  if (!start) {
    return range_outcome::whole;
  }

  auto const end = http::detail::parse_number<std::uint64_t>(last);

  if (!last.empty() && (!end || *end < *start)) {
    return range_outcome::whole;
  }

  if (*start >= size) {
    return range_outcome::out_of_file;
  }

  range = {*start, std::min(end.value_or(size - 1), size - 1) - *start + 1};
  return range_outcome::part;
}

[[nodiscard]] bool range_still_holds(conditions const &asked, found_file const &found) {
  if (!asked.if_range) {
    return true;
  }

  auto const validator = trimmed(*asked.if_range);

  if (validator.starts_with('"')) {
    return validator == found.etag;
  }

  auto const date = parse_http_date(validator);

  return date && found.modified == *date;
}

[[nodiscard]] std::string content_type_of(fs::path const &name, served const &state) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 34> known{{
      {"html", "text/html; charset=utf-8"},
      {"htm", "text/html; charset=utf-8"},
      {"css", "text/css; charset=utf-8"},
      {"js", "text/javascript; charset=utf-8"},
      {"mjs", "text/javascript; charset=utf-8"},
      {"json", "application/json"},
      {"map", "application/json"},
      {"webmanifest", "application/manifest+json"},
      {"wasm", "application/wasm"},
      {"txt", "text/plain; charset=utf-8"},
      {"md", "text/markdown; charset=utf-8"},
      {"csv", "text/csv; charset=utf-8"},
      {"xml", "application/xml"},
      {"pdf", "application/pdf"},
      {"svg", "image/svg+xml"},
      {"png", "image/png"},
      {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},
      {"gif", "image/gif"},
      {"webp", "image/webp"},
      {"avif", "image/avif"},
      {"ico", "image/x-icon"},
      {"woff", "font/woff"},
      {"woff2", "font/woff2"},
      {"ttf", "font/ttf"},
      {"otf", "font/otf"},
      {"mp4", "video/mp4"},
      {"webm", "video/webm"},
      {"mp3", "audio/mpeg"},
      {"ogg", "audio/ogg"},
      {"wav", "audio/wav"},
      {"zip", "application/zip"},
      {"gz", "application/gzip"},
      {"tar", "application/x-tar"},
  }};

  auto extension = name.extension().string();

  if (!extension.empty()) {
    extension.erase(0, 1);
  }

  std::ranges::transform(extension, extension.begin(), [](char letter) {
    return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
  });

  if (auto const given = state.options.content_types.find(extension); given != state.options.content_types.end()) {
    return given->second;
  }

  auto const entry =
      std::ranges::find(known, std::string_view{extension}, &std::pair<std::string_view, std::string_view>::first);

  return std::string{entry != known.end() ? entry->second : std::string_view{"application/octet-stream"}};
}

[[nodiscard]] http::body_writer writing(std::shared_ptr<open_file> file, std::uint64_t first, std::uint64_t length) {
  return [file = std::move(file), first, length](http::body_stream out) -> async::task<> {
    std::string piece(static_cast<std::size_t>(std::min<std::uint64_t>(piece_size, length)), '\0');
    auto offset = first;
    auto left = length;

    while (left != 0) {
      auto const wanted = static_cast<std::size_t>(std::min<std::uint64_t>(piece.size(), left));
      // What failed is told from there: errno belongs to the thread it ran on.
      auto const read = co_await async::offload([&]() -> std::int64_t {
        for (;;) {
          auto const count = ::pread(file->descriptor(), piece.data(), wanted, static_cast<off_t>(offset));

          if (count >= 0) {
            return count;
          }

          if (errno != EINTR) {
            return -errno;
          }
        }
      });

      if (read < 0) {
        throw std::system_error{static_cast<int>(-read), std::generic_category(), "quoll::server::static_files"};
      }

      // Shorter than it was: what it announced cannot be sent, and the connection says so.
      if (read == 0) {
        co_return;
      }

      auto const count = static_cast<std::size_t>(read);

      co_await out.write(std::string_view{piece.data(), count});
      offset += count;
      left -= count;
    }
  };
}

// What the router's 404 was, a page a later middleware made of it included: what those middlewares added otherwise
// stays.
void forget_its_content(http::response &answer) {
  answer.reason.clear();
  answer.body.clear();
  answer.trailers.clear();
  answer.body_writer = nullptr;

  for (auto const name : {"Content-Type", "Content-Length", "Content-Encoding", "Content-Range", "Transfer-Encoding"}) {
    answer.fields.erase(name);
  }
}

// The response for `found`, on what the routes answered.
[[nodiscard]] http::response answer_with(http::response answer, found_file found, fs::path const &name,
                                         conditions const &asked, served const &state) {
  forget_its_content(answer);
  answer.fields.set("ETag", found.etag);
  answer.fields.set("Last-Modified", http_date(found.modified));
  answer.fields.set("Accept-Ranges", "bytes");

  if (not_modified(asked, found)) {
    answer.status = http::status::not_modified;
    return answer;
  }

  answer.fields.set("Content-Type", content_type_of(name, state));

  byte_range range{0, found.size};
  auto outcome = range_outcome::whole;

  if (asked.range && range_still_holds(asked, found)) {
    outcome = parse_range(*asked.range, found.size, range);
  }

  if (outcome == range_outcome::out_of_file) {
    answer.status = http::status::range_not_satisfiable;
    answer.fields.set("Content-Range", "bytes */" + std::to_string(found.size));
    answer.fields.set("Content-Length", "0");
    return answer;
  }

  if (outcome == range_outcome::part) {
    answer.status = http::status::partial_content;
    answer.fields.set("Content-Range", "bytes " + std::to_string(range.first) + "-" +
                                           std::to_string(range.first + range.length - 1) + "/" +
                                           std::to_string(found.size));
  } else {
    answer.status = http::status::ok;
  }

  answer.fields.set("Content-Length", std::to_string(range.length));
  answer.body_writer = writing(std::move(found.file), range.first, range.length);
  return answer;
}

[[nodiscard]] std::optional<std::string> copy_of(http::fields const &fields, std::string_view name) {
  auto const value = fields.find(name);

  return value ? std::optional{std::string{*value}} : std::nullopt;
}

// A path under the prefix: what follows it, with its leading slash gone.
[[nodiscard]] std::optional<std::string_view> under_prefix(std::string_view path, std::string_view prefix) noexcept {
  if (!path.starts_with(prefix)) {
    return std::nullopt;
  }

  path.remove_prefix(prefix.size());

  if (path.empty()) {
    return path;
  }

  return path.starts_with('/') ? std::optional{path.substr(1)} : std::nullopt;
}

async::task<http::response> serve_file(std::shared_ptr<served const> state, http::request message, http::next next) {
  auto const wanted = message.method == http::verb::get || message.method == http::verb::head;
  auto const path = next.path();
  auto const under = wanted ? under_prefix(path, state->prefix) : std::nullopt;

  // Nothing is asked of a request a file cannot answer: this is on the path of every request.
  if (!under) {
    co_return co_await next(std::move(message));
  }

  // Range is only for a GET: RFC 9110, 14.2.
  auto const is_get = message.method == http::verb::get;
  conditions const asked{copy_of(message.fields, "If-None-Match"), copy_of(message.fields, "If-Modified-Since"),
                         is_get ? copy_of(message.fields, "Range") : std::nullopt,
                         is_get ? copy_of(message.fields, "If-Range") : std::nullopt};
  auto const question = message.target.find('?');
  auto const query = question == std::string::npos ? std::string{} : message.target.substr(question);
  auto answer = co_await next(std::move(message));

  if (!next.router_answered() || answer.status != http::status::not_found) {
    co_return answer;
  }

  auto const relative = relative_path(*under, state->options.dotfiles);

  if (!relative) {
    co_return answer;
  }

  auto const ends_with_slash = path.ends_with('/');
  auto found = co_await async::offload([&] { return look_up(*state, *relative, ends_with_slash); });

  if (found.redirect) {
    auto const directory = relative->empty() ? fs::path{state->prefix}.filename() : relative->filename();

    forget_its_content(answer);
    answer.status = http::status::moved_permanently;
    // Relative, and from a name rather than what the peer sent: `./` keeps a colon in it from reading as a scheme.
    answer.fields.set("Location", "./" + http::detail::percent_encode(directory.string()) + "/" + query);
    co_return answer;
  }

  if (!found.file) {
    co_return answer;
  }

  auto const name = ends_with_slash ? fs::path{state->options.index} : *relative;

  co_return answer_with(std::move(answer), std::move(*found.file), name, asked, *state);
}

} // namespace

http::router::middleware static_files(std::string_view prefix, fs::path const &root, static_files_options options) {
  if (!prefix.starts_with('/')) {
    throw std::invalid_argument{"quoll::server::static_files: the prefix does not start with a slash"};
  }

  while (prefix.ends_with('/')) {
    prefix.remove_suffix(1);
  }

  auto state = std::make_shared<served const>(served{std::string{prefix}, fs::canonical(root), std::move(options)});

  return [state = std::move(state)](http::request &&message, http::next const &next) {
    return serve_file(state, std::move(message), next);
  };
}

} // namespace quoll::server
