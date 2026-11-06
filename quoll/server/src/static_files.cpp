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
#include <quoll/server/detail/open_beneath.hpp>

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
#include <iterator>
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

namespace detail {

namespace {

// To go through a directory, which only asks for the right to search it: a web root in mode 0711 hides its listing.
#if defined(O_PATH) && !defined(__EMSCRIPTEN__)
constexpr int search_only = O_PATH;
#elif defined(O_SEARCH) && !defined(__EMSCRIPTEN__)
constexpr int search_only = O_SEARCH;
#else
constexpr int search_only = O_RDONLY;
#endif

} // namespace

int open_beneath(std::filesystem::path const &root, std::filesystem::path const &relative) {
  auto current = ::open(root.c_str(), search_only | O_CLOEXEC | O_DIRECTORY);

  if (current < 0) {
    return -errno;
  }

  for (auto component = relative.begin(); component != relative.end(); ++component) {
    // Only the last may be anything but a directory.
    auto const last = std::next(component) == relative.end();
    auto const next =
        ::openat(current, component->c_str(),
                 O_CLOEXEC | O_NOFOLLOW | (last ? O_RDONLY | O_NONBLOCK | O_NOCTTY : search_only | O_DIRECTORY));
    auto const failure = errno;

    ::close(current);

    if (next < 0) {
      return -failure;
    }

    current = next;
  }

  return current;
}

} // namespace detail

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
         error == std::errc::too_many_symbolic_link_levels || error == std::errc::filename_too_long ||
         // A Unix socket, which macOS says with the last of these, or a device with no driver: no files.
         error == std::errc::no_such_device_or_address || error == std::errc::no_such_device ||
         error == std::errc::operation_not_supported;
}

// What a path opened to, with what it is.
struct opened_path {
  std::shared_ptr<open_file> file;

  struct stat status {};
};

// `resolved`, under `root`, opened: nothing if there is nothing there to serve. What cannot say what it opened is
// trouble: the descriptor is good, so that no answer is a failure of the system.
[[nodiscard]] std::optional<opened_path> open_resolved(fs::path const &root, fs::path const &resolved) {
  auto const descriptor = detail::open_beneath(root, resolved.lexically_relative(root));

  if (descriptor < 0) {
    std::error_code const error{-descriptor, std::generic_category()};

    if (nothing_there(error)) {
      return std::nullopt;
    }

    throw std::system_error{error, "quoll::server::static_files"};
  }

  opened_path opened{std::make_shared<open_file>(descriptor)};

  if (::fstat(descriptor, &opened.status) != 0) {
    throw std::system_error{errno, std::generic_category(), "quoll::server::static_files"};
  }

  return opened;
}

[[nodiscard]] found_file found_from(opened_path opened) {
  auto const &status = opened.status;
  found_file found{std::move(opened.file), static_cast<std::uint64_t>(status.st_size), status.st_mtime, "\""};

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

// Whether a path whose links have been followed is one to serve: a link must not lead to the `.env` its name does not
// say.
[[nodiscard]] bool may_be_served(served const &state, fs::path const &root, fs::path const &resolved) {
  if (state.options.dotfiles) {
    return true;
  }

  auto const relative = resolved.lexically_relative(root);

  return std::ranges::none_of(relative, [](fs::path const &component) {
    auto const name = component.native();

    return name != "." && name.starts_with('.');
  });
}

// `path`, once its links are followed, if it is under `root` and to be served.
[[nodiscard]] std::optional<fs::path> to_serve(served const &state, fs::path const &root, fs::path const &path) {
  auto resolved = inside_root(root, path);

  return resolved && may_be_served(state, root, *resolved) ? std::move(resolved) : std::nullopt;
}

[[nodiscard]] std::optional<found_file> regular_file(fs::path const &root, fs::path const &resolved) {
  auto opened = open_resolved(root, resolved);

  if (!opened || !S_ISREG(opened->status.st_mode)) {
    return std::nullopt;
  }

  return found_from(std::move(*opened));
}

[[nodiscard]] looked_up look_up(served const &state, fs::path const &relative, bool ends_with_slash) {
  std::error_code error;

  // As it is now: a root that is a link, flipped to deploy a new release, is to be followed.
  auto const root = fs::canonical(state.root, error);

  if (error) {
    if (nothing_there(error)) {
      return {};
    }

    throw std::system_error{error, "quoll::server::static_files"};
  }

  auto const path = to_serve(state, root, root / relative);

  if (!path) {
    return {};
  }

  auto const kind = fs::status(*path, error).type();

  if (error) {
    if (nothing_there(error)) {
      return {};
    }

    throw std::system_error{error, "quoll::server::static_files"};
  }

  // What is neither is not even opened: opening a device may do something.
  if (kind == fs::file_type::regular) {
    return ends_with_slash ? looked_up{} : looked_up{regular_file(root, *path)};
  }

  if (kind != fs::file_type::directory || state.options.index.empty()) {
    return {};
  }

  auto const index = to_serve(state, root, *path / state.options.index);
  auto file = index ? regular_file(root, *index) : std::nullopt;

  if (!file) {
    return {};
  }

  // What it opened goes with this: the redirect comes back for it.
  return ends_with_slash ? looked_up{std::move(file)} : looked_up{.redirect = true};
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

// The parts of an HTTP date, as text, whichever form they came in.
struct date_parts {
  std::string_view weekday;
  std::string_view day;
  std::string_view month;
  std::string_view year;
  std::string_view time;
};

// The time of what they say, if that is a moment that exists, on that weekday: what timegm() would otherwise move on to
// another day, 31 February to March, is no date. Read here rather than by strptime(), whose checks differ from one C
// library to another, as this comes from the network.
[[nodiscard]] std::optional<std::time_t> time_of(date_parts const &parts, int year) {
  static constexpr std::array<std::string_view, 7> days{"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                        "Thursday", "Friday", "Saturday"};
  static constexpr std::string_view months = "JanFebMarAprMayJunJulAugSepOctNovDec";

  auto const day = http::detail::parse_number<int>(http::detail::trim(parts.day));
  auto const month = months.find(parts.month);
  auto const hour = http::detail::parse_number<int>(parts.time.substr(0, 2));
  auto const minute = http::detail::parse_number<int>(parts.time.substr(3, 2));
  auto const second = http::detail::parse_number<int>(parts.time.substr(6, 2));

  if (parts.time.size() != 8 || parts.time[2] != ':' || parts.time[5] != ':' || parts.month.size() != 3 || !day ||
      month == std::string_view::npos || month % 3 != 0 || !hour || !minute || !second) {
    return std::nullopt;
  }

  std::tm asked{};

  asked.tm_year = year - 1900;
  asked.tm_mon = static_cast<int>(month / 3);
  asked.tm_mday = *day;
  asked.tm_hour = *hour;
  asked.tm_min = *minute;
  asked.tm_sec = *second;

  // Read back rather than from what timegm() was given, which not all of them normalize.
  auto given = asked;
  auto const time = ::timegm(&given);
  std::tm normalized{};

  ::gmtime_r(&time, &normalized);

  auto const weekday = days.at(static_cast<std::size_t>(normalized.tm_wday));

  if (normalized.tm_year != asked.tm_year || normalized.tm_mon != asked.tm_mon || normalized.tm_mday != asked.tm_mday ||
      normalized.tm_hour != asked.tm_hour || normalized.tm_min != asked.tm_min || normalized.tm_sec != asked.tm_sec ||
      (parts.weekday != weekday && parts.weekday != weekday.substr(0, 3))) {
    return std::nullopt;
  }

  return time;
}

// A year of four digits: no more, as what a peer sends here must stay far from what an int holds.
[[nodiscard]] std::optional<int> four_digits(std::string_view text) {
  return text.size() == 4 ? http::detail::parse_number<int>(text) : std::nullopt;
}

// The time of an HTTP date, in the three forms RFC 9110, 5.6.7 asks a recipient to accept. What is none of them, a
// peer's invention above all, is ignored, as it lets a recipient do with a date that cannot be read.
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view text) {
  // Sun, 06 Nov 1994 08:49:37 GMT
  if (text.size() == 29 && text.substr(3, 2) == ", " && text[7] == ' ' && text[11] == ' ' && text[16] == ' ' &&
      text.substr(25) == " GMT") {
    auto const year = four_digits(text.substr(12, 4));

    return year ? time_of(
                      {text.substr(0, 3), text.substr(5, 2), text.substr(8, 3), text.substr(12, 4), text.substr(17, 8)},
                      *year)
                : std::nullopt;
  }

  // Sun Nov  6 08:49:37 1994
  if (text.size() == 24 && text[3] == ' ' && text[7] == ' ' && text[10] == ' ' && text[19] == ' ') {
    auto const year = four_digits(text.substr(20, 4));

    return year ? time_of(
                      {text.substr(0, 3), text.substr(8, 2), text.substr(4, 3), text.substr(20, 4), text.substr(11, 8)},
                      *year)
                : std::nullopt;
  }

  // Sunday, 06-Nov-94 08:49:37 GMT
  auto const comma = text.find(", ");

  if (comma == std::string_view::npos || text.size() != comma + 2 + 22) {
    return std::nullopt;
  }

  auto const rest = text.substr(comma + 2);

  if (rest[2] != '-' || rest[6] != '-' || rest[9] != ' ' || rest.substr(18) != " GMT") {
    return std::nullopt;
  }

  auto const short_year = http::detail::parse_number<int>(rest.substr(7, 2));

  if (!short_year || *short_year < 0) {
    return std::nullopt;
  }

  // RFC 9110, 5.6.7: what would be more than 50 years ahead is the same year in the past.
  auto const now = std::time(nullptr);
  std::tm today{};

  ::gmtime_r(&now, &today);

  auto const this_year = today.tm_year + 1900;
  auto year = this_year - this_year % 100 + *short_year;

  if (year > this_year + 50) {
    year -= 100;
  }

  return time_of({text.substr(0, comma), rest.substr(0, 2), rest.substr(3, 3), rest.substr(7, 2), rest.substr(10, 8)},
                 year);
}

// RFC 9110, 13.1.2: the weak comparison, over a list, or `*`.
[[nodiscard]] bool none_match_fails(std::string_view list, std::string_view etag) {
  for (auto const member : std::views::split(list, ',')) {
    auto tag = http::detail::trim(std::string_view{member.begin(), member.end()});

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
  std::optional<std::string> if_match{};
  std::optional<std::string> if_unmodified_since{};
  std::optional<std::string> if_none_match{};
  std::optional<std::string> if_modified_since{};
  std::optional<std::string> range{};
  std::optional<std::string> if_range{};
};

// RFC 9110, 13.1.1: the strong comparison, over a list, or `*`.
[[nodiscard]] bool match_holds(std::string_view list, std::string_view etag) {
  for (auto const member : std::views::split(list, ',')) {
    auto const tag = http::detail::trim(std::string_view{member.begin(), member.end()});

    if (tag == "*" || tag == etag) {
      return true;
    }
  }

  return false;
}

// RFC 9110, 13.2.2: what the request holds the file to, the first of them taking over the second. A client resuming a
// download through these must not get the bytes of another file.
[[nodiscard]] bool precondition_fails(conditions const &asked, found_file const &found) {
  if (asked.if_match) {
    return !match_holds(*asked.if_match, found.etag);
  }

  if (asked.if_unmodified_since) {
    auto const since = parse_http_date(*asked.if_unmodified_since);

    return since && found.modified > *since;
  }

  return false;
}

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
  header = http::detail::trim(header);

  if (!header.starts_with("bytes=")) {
    return range_outcome::whole;
  }

  header.remove_prefix(6);

  auto const dash = header.find('-');

  if (dash == std::string_view::npos) {
    return range_outcome::whole;
  }

  auto const first = http::detail::trim(header.substr(0, dash));
  auto const last = http::detail::trim(header.substr(dash + 1));

  // The last `n` bytes.
  if (first.empty()) {
    auto const suffix = http::detail::parse_number<std::uint64_t>(last);

    if (!suffix) {
      return range_outcome::whole;
    }

    if (*suffix == 0) {
      return range_outcome::out_of_file;
    }

    // Satisfiable, RFC 9110, 14.1.2, and no 206 can say so.
    if (size == 0) {
      return range_outcome::whole;
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

  auto const validator = http::detail::trim(*asked.if_range);

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

  std::ranges::transform(extension, extension.begin(), http::detail::to_lower);

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

  for (auto const name : {"Content-Type", "Content-Length", "Content-Encoding", "Content-Range", "Content-Language",
                          "Content-Disposition", "Content-Location", "Transfer-Encoding", "Trailer"}) {
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

  if (precondition_fails(asked, found)) {
    answer.status = http::status::precondition_failed;
    answer.fields.set("Content-Length", "0");
    return answer;
  }

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

// A field, on as many lines as it came: a list may, and what makes no sense so, two ranges, reads as what it is.
[[nodiscard]] std::optional<std::string> copy_of(http::fields const &fields, std::string_view name) {
  std::optional<std::string> value;

  for (auto const &entry : fields) {
    if (!http::detail::iequals(entry.name, name)) {
      continue;
    }

    if (value) {
      value->append(", ");
    }

    value = value.value_or("") + entry.value;
  }

  return value;
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

  // Nothing is asked of a request a file cannot answer, one a route answers among them: this is on the path of every
  // request.
  if (!under || !next.route().empty()) {
    co_return co_await next(std::move(message));
  }

  // Range is only for a GET: RFC 9110, 14.2.
  auto const is_get = message.method == http::verb::get;
  conditions const asked{copy_of(message.fields, "If-Match"),
                         copy_of(message.fields, "If-Unmodified-Since"),
                         copy_of(message.fields, "If-None-Match"),
                         copy_of(message.fields, "If-Modified-Since"),
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
    // The prefix is as next::path() has it, a percent sign encoded, unlike a name of the disk.
    auto const directory = relative->empty()
                               ? http::detail::percent_decode(fs::path{state->prefix}.filename().string()).value_or("")
                               : relative->filename().string();

    forget_its_content(answer);
    answer.status = http::status::moved_permanently;
    // Relative, and from a name rather than what the peer sent: `./` keeps a colon in it from reading as a scheme.
    answer.fields.set("Location", "./" + http::detail::percent_encode(directory) + "/" + query);
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

  if (!fs::is_directory(fs::canonical(root))) {
    throw std::invalid_argument{"quoll::server::static_files: the root is not a directory"};
  }

  // As given, rather than where it leads now: see look_up().
  auto state = std::make_shared<served const>(served{std::string{prefix}, fs::absolute(root), std::move(options)});

  return [state = std::move(state)](http::request &&message, http::next const &next) {
    return serve_file(state, std::move(message), next);
  };
}

} // namespace quoll::server
