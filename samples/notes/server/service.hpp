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

// What the server does with each route of the contract, in memory: the handlers rest::mount routes requests to.

#ifndef QUOLL_SAMPLES_NOTES_SERVER_SERVICE_HPP
#define QUOLL_SAMPLES_NOTES_SERVER_SERVICE_HPP

#include "notes_api.hpp"

#include <quoll/http/status.hpp>
#include <quoll/rest.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace notes {

// The runtime runs the requests on one thread: no lock.
class service {
public:
  std::vector<note> operator()(list, note_filter filter) const {
    std::vector<note> found;

    for (auto const &[id, item] : notes_) {
      if (!filter.contains || item.text.contains(*filter.contains)) {
        found.push_back(item);
      }
    }

    return found;
  }

  note operator()(get, std::int64_t id) const {
    auto const found = notes_.find(id);

    if (found == notes_.end()) {
      // An error, which a route does not declare: answered as an RFC 9457 problem.
      throw quoll::http::http_error{quoll::http::status::not_found, "no note " + std::to_string(id)};
    }

    return found->second;
  }

  note operator()(create, new_note created) {
    note item{.id = next_id_++, .text = std::move(created.text)};

    notes_.emplace(item.id, item);
    return item;
  }

  void operator()(remove, std::int64_t id) { notes_.erase(id); }

private:
  std::map<std::int64_t, note> notes_;
  std::int64_t next_id_ = 1;
};

} // namespace notes

#endif // QUOLL_SAMPLES_NOTES_SERVER_SERVICE_HPP
