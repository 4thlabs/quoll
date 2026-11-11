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


import tailwindcss from '@tailwindcss/vite';
import { defineConfig } from 'vite';

import { quoll } from '../../../tools/vite-plugin/quoll-vite-plugin.js';

export default defineConfig({
  plugins: [
    tailwindcss(),
    quoll({
      target: 'quoll_dashboard_web',
      sources: [
        'main.cpp',
        'components/dashboard_page.hpp',
        'components/error_text.hpp',
        'components/field.hpp',
        'components/product_detail.hpp',
        'components/product_list.hpp',
        'components/product_row.hpp',
        'components/sales_chart.cpp',
        'components/sales_chart.hpp',
        'products_client.cpp',
        'products_client.hpp',
        '../api/product.hpp',
        '../api/products_api.hpp',
      ],
    }),
  ],
  server: {
    proxy: { '/api': 'http://localhost:8080' },
    // A folder shared with a container does not always report its changes: QUOLL_POLL=1 looks for them.
    watch: process.env.QUOLL_POLL ? { usePolling: true, interval: 100 } : undefined,
  },
});
