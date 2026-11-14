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

#ifndef QUOLL_REST_DETAIL_PATH_TYPES_HPP
#define QUOLL_REST_DETAIL_PATH_TYPES_HPP

#include <quoll/rest/detail/checked_path.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/detail/type_list.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <cstddef>
#include <utility>

namespace quoll::rest::detail {

// The types of the first `Count` path parameters of `Path`, as its template says them: `{id:int64}` an int64.
template <fixed_string Path, std::size_t Count, class Indices = std::make_index_sequence<Count>>
struct template_path_types;

template <fixed_string Path, std::size_t Count, std::size_t... Index>
struct template_path_types<Path, Count, std::index_sequence<Index...>> {
  using type = type_list<typename param_kind_type<checked_path<Path>::parsed.params[Index].kind>::type...>;
};

// The types of all the path parameters of `Path`, as its template says them.
template <fixed_string Path>
using path_argument_types = template_path_types<Path, checked_path<Path>::parsed.size>;

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_PATH_TYPES_HPP
