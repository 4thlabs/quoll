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

#ifndef QUOLL_SERVER_STATIC_FILES_HPP
#define QUOLL_SERVER_STATIC_FILES_HPP

#include <quoll/http/router.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace quoll::server {

/** @brief How server::static_files() serves a directory. */
struct static_files_options {
  /** @brief What a request for a directory serves, from that directory; empty: such a request stays a 404. */
  std::string index = "index.html";
  /** @brief Whether files and directories whose name starts with a dot are served: `.env` and `.git` are not, by
   * default, nor what a link leads to among them. */
  bool dotfiles = false;
  /**
   * @brief The `Content-Type` of an extension (lower case, with no dot), in addition to those known, and over them.
   * What is neither is `application/octet-stream`.
   */
  std::map<std::string, std::string, std::less<>> content_types{};
};

/**
 * @brief A middleware serving the files under `root` for the GET and HEAD requests under `prefix`:
 *
 * @code
 * router.use(quoll::server::static_files("/assets", "./public"));
 * router.use(quoll::server::static_files("/", "./site"));
 * @endcode
 *
 * It only ever answers what the router itself answers 404 to, once the middlewares added after it have run, so that:
 * - a route always wins, and a request a route answers costs no look at the disk;
 * - where it stands among the middlewares protects nothing by accident: one added after it that refuses a request
 * refuses its file too, and what they add to the 404 stays on the file's response;
 * - a path the router refuses (`..`, `%00`) has had its 400. Of several on one path, the last added looks first, and a
 * file that is not there leaves the 404 to the one before;
 * - a later middleware answering a 404 of its own rather than awaiting `next`, to hide something, has answered: only
 * the router's own counts, which one that awaits `next` and then changes has not replaced.
 *
 * A file is sent in pieces of 64 KiB, so that what it costs in memory does not depend on its size, read through
 * async::offload() so that a slow disk does not hold up the loop. A response has `Content-Type`, `Content-Length`,
 * `Last-Modified` and `ETag`: `If-Match` and `If-Unmodified-Since` answer 412 when the file is no longer the one asked
 * for, `If-None-Match` and `If-Modified-Since` 304, and a `Range` of one interval 206, or 416 out of the file (several
 * intervals, or what cannot be read, send it whole; `If-Range` as RFC 9110 says). A request for a directory without its
 * final slash is redirected to it, if it has an index.
 *
 * Nothing outside `root` is served, a symbolic link leading out of it included, and neither is what is not a regular
 * file: all of that is the same 404 as what does not exist. What says that the server is in trouble, out of file
 * descriptors for instance, is thrown rather than taken for a file that is not there: a 500, told to
 * `on_handler_error`.
 *
 * `root` is looked at again for each request, so that a link flipped to deploy a new release is followed.
 *
 * `prefix` is compared with next::path(), so that a percent sign in it is written `%25`, and nothing else is encoded.
 *
 * @throws std::invalid_argument for a `prefix` that does not start with a slash or a `root` that is no directory, and
 * std::filesystem::filesystem_error for a `root` that does not exist.
 */
[[nodiscard]] http::router::middleware static_files(std::string_view prefix, std::filesystem::path const &root,
                                                    static_files_options options = {});

} // namespace quoll::server

#endif // QUOLL_SERVER_STATIC_FILES_HPP
