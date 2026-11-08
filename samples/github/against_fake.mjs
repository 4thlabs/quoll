// Under Emscripten: a fake GitHub served by Node, and quoll_github compiled to WebAssembly run against it in Node,
// through fetch. What against_fake.sh checks natively, with the same answers.
//
//   node against_fake.mjs <quoll_github.js>
import { execFile } from 'node:child_process';
import http from 'node:http';
import { promisify } from 'node:util';

const run = promisify(execFile);
const program = process.argv[2];
let issuesAsked = 0;

function answer(response, status, body) {
  response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  response.end(JSON.stringify(body));
}

const server = http.createServer((request, response) => {
  const url = new URL(request.url, 'http://fake');

  if (!request.headers['user-agent']) {
    return answer(response, 403, { message: 'a User-Agent header is required' });
  }

  if (url.pathname === '/repos/4thlabs/quoll') {
    return answer(response, 200, {
      full_name: '4thlabs/quoll',
      description: 'A C++23 web framework',
      stargazers_count: 42,
      open_issues_count: 2,
      html_url: 'https://github.com/4thlabs/quoll',
      private: false,
    });
  }

  if (url.pathname === '/repos/4thlabs/quoll/issues') {
    // Once busy: the program's retry sends it again.
    if (++issuesAsked === 1) {
      return answer(response, 503, { message: 'busy' });
    }

    if (url.searchParams.get('state') !== 'open' || url.searchParams.get('per_page') !== '5') {
      return answer(response, 200, []);
    }

    return answer(response, 200, [
      { number: 7, title: 'SSE', state: 'open', user: { login: 'ada' } },
      { number: 9, title: 'WebSocket', state: 'open', user: { login: 'grace' }, pull_request: { url: 'pulls/9' } },
    ]);
  }

  if (url.pathname === '/user') {
    if (request.headers.authorization !== 'Bearer test-token') {
      return answer(response, 401, { message: 'Bad credentials' });
    }

    return answer(response, 200, { login: 'ada', name: 'Ada Lovelace' });
  }

  return answer(response, 404, { message: 'Not Found' });
});

await new Promise((listening) => server.listen(0, '127.0.0.1', listening));

// The environment of this run, without a token of its own, and the fake's URL.
const { GITHUB_TOKEN: _, ...environment } = process.env;
environment.QUOLL_GITHUB_URL = `http://127.0.0.1:${server.address().port}`;

async function call(args, added = {}) {
  try {
    const { stdout, stderr } = await run(process.execPath, [program, ...args], { env: { ...environment, ...added } });

    return { code: 0, out: stdout + stderr };
  } catch (failure) {
    return { code: failure.code, out: (failure.stdout ?? '') + (failure.stderr ?? '') };
  }
}

function expect(holds, what, outcome) {
  if (!holds) {
    console.error(`${what}: ended with ${outcome.code}, and printed:\n${outcome.out}`);
    process.exitCode = 1;
  }
}

let outcome = await call(['4thlabs/quoll']);
expect(outcome.code === 0 && outcome.out.includes('4thlabs/quoll: 42 stars, 2 open issues') &&
         outcome.out.includes('  #9 WebSocket (grace) [pull request]'), 'the repository', outcome);

outcome = await call(['4thlabs/nothing']);
expect(outcome.code === 1 && outcome.out.includes('not found: Not Found'), 'a missing repository', outcome);

outcome = await call(['me'], { GITHUB_TOKEN: 'test-token' });
expect(outcome.code === 0 && outcome.out.includes('ada (Ada Lovelace)'), 'the account of a token', outcome);

outcome = await call(['me'], { GITHUB_TOKEN: 'wrong' });
expect(outcome.code === 1 && outcome.out.includes('the token is refused: Bad credentials'), 'a wrong token', outcome);

server.close();

if (!process.exitCode) {
  console.log('quoll_github through fetch against the fake GitHub: as expected');
}
