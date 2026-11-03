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


// A Node HTTP server for the quoll::emscripten tests, in the process running them. It answers:
// - /hang: never;
// - /gzip: "hello gzipped", compressed (Content-Encoding: gzip);
// - /status/<code>: that status;
// - anything else: 200, with the request's body as body ("hello over fetch" if empty).
// Every response tells the request's method, target and x-test field in x-method, x-target and x-test.

addToLibrary({
  $quollTestServer: {
    server: null,
    port: 0,
  },

  quoll_test_server_start__deps: ['$quollTestServer'],
  quoll_test_server_start: () => {
    const http = require('node:http');
    const zlib = require('node:zlib');
    const server = http.createServer((request, response) => {
      const chunks = [];

      request.on('data', (chunk) => chunks.push(chunk));
      request.on('end', () => {
        if (request.url === '/hang') {
          return;
        }

        if (request.url === '/gzip') {
          response.writeHead(200, { 'content-encoding': 'gzip' });
          response.end(zlib.gzipSync('hello gzipped'));
          return;
        }

        const status = request.url.startsWith('/status/') ? Number(request.url.slice('/status/'.length)) : 200;
        const body = Buffer.concat(chunks);

        response.writeHead(status, {
          'x-method': request.method,
          'x-target': request.url,
          'x-test': request.headers['x-test'] ?? '',
        });
        response.end(body.length > 0 ? body : 'hello over fetch');
      });
    });

    quollTestServer.server = server;
    quollTestServer.port = 0;
    server.on('error', (error) => {
      console.error(`quoll test server: ${error.message}`);
      quollTestServer.port = -1;
    });
    server.listen(0, '127.0.0.1', () => {
      quollTestServer.port = server.address().port;
    });
  },

  // 0 until the server listens, -1 if it failed to.
  quoll_test_server_port__deps: ['$quollTestServer'],
  quoll_test_server_port: () => quollTestServer.port,

  quoll_test_server_stop__deps: ['$quollTestServer'],
  quoll_test_server_stop: () => {
    quollTestServer.server.closeAllConnections();
    quollTestServer.server.close();
    quollTestServer.server = null;
    quollTestServer.port = 0;
  },
});
