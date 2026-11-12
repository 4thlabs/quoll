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

#ifndef QUOLL_UI_HPP
#define QUOLL_UI_HPP

#include <quoll/ui/app/app.hpp>
#include <quoll/ui/app/app_options.hpp>
#include <quoll/ui/component/callable_component.hpp>
#include <quoll/ui/component/callback.hpp>
#include <quoll/ui/component/child_component.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/component/component_with_props.hpp>
#include <quoll/ui/component/function_component.hpp>
#include <quoll/ui/component/hooks.hpp>
#include <quoll/ui/component/mounted_component.hpp>
#include <quoll/ui/component/unmounted_component.hpp>
#include <quoll/ui/context/context_spec.hpp>
#include <quoll/ui/context/provide.hpp>
#include <quoll/ui/context/provided.hpp>
#include <quoll/ui/context/shared.hpp>
#include <quoll/ui/data/action.hpp>
#include <quoll/ui/data/async_state.hpp>
#include <quoll/ui/data/effect.hpp>
#include <quoll/ui/data/form_state.hpp>
#include <quoll/ui/data/run_error.hpp>
#include <quoll/ui/data/state.hpp>
#include <quoll/ui/routing/history.hpp>
#include <quoll/ui/routing/route.hpp>
#include <quoll/ui/routing/router.hpp>
#include <quoll/ui/view/decorators.hpp>
#include <quoll/ui/view/element.hpp>
#include <quoll/ui/view/event_function.hpp>
#include <quoll/ui/view/node_id.hpp>
#include <quoll/ui/view/renderer.hpp>
#include <quoll/ui/view/renderer_ref.hpp>
#include <quoll/ui/view/tags.hpp>
#include <quoll/ui/view/to_html.hpp>
#include <quoll/ui/widget/json_event.hpp>
#include <quoll/ui/widget/json_event_function.hpp>
#include <quoll/ui/widget/widget.hpp>
#include <quoll/ui/widget/widget_renderer.hpp>

#endif // QUOLL_UI_HPP
