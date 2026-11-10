// Node's own client (fetch, undici; the global WebSocket) against Quoll's server, at QUOLL_URL.
const base = process.env.QUOLL_URL;
let failures = 0;

async function check(name, body) {
  try {
    await body();
    console.log(`ok ${name}`);
  } catch (failure) {
    ++failures;
    console.log(`FAIL ${name}: ${failure.message ?? failure}`);
  }
}

function expect(holds, what) {
  if (!holds) throw new Error(what);
}

const pieces = (count) => Array.from({ length: count }, (_, index) => `piece ${index}\n`).join('');

await check('fetch GET /hello', async () => {
  const response = await fetch(`${base}/hello`);

  expect(response.status === 200, `status ${response.status}`);
  expect((await response.text()) === 'hello', 'body');
});
await check('fetch five GETs, kept alive', async () => {
  for (let turn = 0; turn < 5; ++turn) expect((await (await fetch(`${base}/hello`)).text()) === 'hello', `GET ${turn}`);
});
await check('fetch POST echoed', async () => {
  const response = await fetch(`${base}/echo`, { method: 'POST', body: 'ping', headers: { 'Content-Type': 'text/plain' } });

  expect((await response.text()) === 'ping', 'body');
});
await check('fetch POST streamed, chunked, echoed', async () => {
  const body = new ReadableStream({
    start(controller) {
      for (let index = 0; index < 10; ++index) controller.enqueue(new TextEncoder().encode(`part ${index};`));
      controller.close();
    },
  });
  const response = await fetch(`${base}/echo`, { method: 'POST', body, duplex: 'half' });
  const expected = Array.from({ length: 10 }, (_, index) => `part ${index};`).join('');

  expect((await response.text()) === expected, 'body');
});
await check('fetch a body in pieces', async () => {
  expect((await (await fetch(`${base}/pieces/1000`)).text()) === pieces(1000), 'pieces differ');
});
await check('fetch server-sent events', async () => {
  const response = await fetch(`${base}/events/5`);

  expect(response.headers.get('content-type').startsWith('text/event-stream'), 'content type');

  const text = await response.text();

  for (let index = 0; index < 5; ++index) {
    expect(text.includes(`id: ${index}\n`) && text.includes(`data: event ${index}\n`), `event ${index}`);
  }
});
await check('WebSocket echo, text and binary', async () => {
  const url = `${base.replace(/^http/, 'ws')}/ws`;
  const socket = new WebSocket(url);
  const received = [];

  socket.binaryType = 'arraybuffer';
  await new Promise((resolve, reject) => {
    socket.onopen = () => {
      socket.send('hello');
      socket.send(new Uint8Array([0, 1, 2, 255]));
    };
    socket.onmessage = (message) => {
      received.push(message.data);

      if (received.length === 2) socket.close(1000);
    };
    socket.onclose = () => resolve();
    socket.onerror = (error) => reject(new Error(error.message ?? 'error'));
  });
  expect(received[0] === 'hello', 'text');
  expect(new Uint8Array(received[1]).join(',') === '0,1,2,255', 'binary');
});

process.exitCode = failures > 0 ? 1 : 0;
