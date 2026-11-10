// A Node server with the routes of tests/interop/server.cpp, which Quoll's client checks: node:http, and ws.
import http from 'node:http';
import { WebSocketServer } from 'ws';

const server = http.createServer((request, response) => {
  const url = new URL(request.url, 'http://x');
  const pieces = url.pathname.match(/^\/pieces\/(\d+)$/);
  const events = url.pathname.match(/^\/events\/(\d+)$/);

  if (url.pathname === '/hello') {
    response.end('hello');
  } else if (url.pathname === '/echo' && request.method === 'POST') {
    const parts = [];

    request.on('data', (part) => parts.push(part));
    request.on('end', () => response.end(Buffer.concat(parts)));
  } else if (pieces) {
    for (let index = 0; index < Number(pieces[1]); ++index) {
      response.write(`piece ${index}\n`);
    }

    response.end();
  } else if (events) {
    response.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store' });

    for (let index = 0; index < Number(events[1]); ++index) {
      response.write(`event: count\nid: ${index}\ndata: event ${index}\n\n`);
    }

    response.end();
  } else if (url.pathname === '/close') {
    response.setHeader('Connection', 'close');
    response.end('bye');
  } else {
    response.statusCode = 404;
    response.end();
  }
});

const sockets = new WebSocketServer({ server, path: '/ws', maxPayload: 64 * 1024 * 1024 });

sockets.on('connection', (socket) => {
  socket.on('message', (data, binary) => socket.send(data, { binary }));
});

server.listen(8080, () => console.log('node server on 8080'));
