"""Python's clients (requests, httpx, websockets, http.client) against Quoll's server, at QUOLL_URL."""

import asyncio
import http.client
import os
import sys
import urllib.parse

import httpx
import requests
import websockets

base = os.environ["QUOLL_URL"]
failures = 0


def check(name, body):
    global failures

    try:
        body()
        print(f"ok {name}", flush=True)
    except Exception as failure:  # noqa: BLE001 - every failure of a check is reported
        failures += 1
        print(f"FAIL {name}: {failure!r}", flush=True)


def pieces(count):
    return "".join(f"piece {index}\n" for index in range(count))


def requests_session():
    with requests.Session() as session:
        for _ in range(5):
            assert session.get(f"{base}/hello").text == "hello"

        assert session.post(f"{base}/echo", data="ping").text == "ping"
        assert session.post(f"{base}/echo", data=b"x" * (1024 * 1024)).content == b"x" * (1024 * 1024)
        # A body of no length: chunked.
        assert session.post(f"{base}/echo", data=(part.encode() for part in ["a", "b", "c"])).text == "abc"
        assert session.get(f"{base}/pieces/1000").text == pieces(1000)
        assert session.get(f"{base}/close").text == "bye"
        assert session.get(f"{base}/hello").text == "hello"


def requests_events():
    with requests.get(f"{base}/events/5", stream=True) as response:
        assert response.headers["Content-Type"].startswith("text/event-stream")
        text = response.text

    for index in range(5):
        assert f"id: {index}\n" in text and f"data: event {index}\n" in text, text


def httpx_client():
    with httpx.Client() as client:
        assert client.get(f"{base}/hello").text == "hello"
        assert client.post(f"{base}/echo", content=b"pong").content == b"pong"

        with client.stream("GET", f"{base}/pieces/1000") as response:
            assert "".join(response.iter_text()) == pieces(1000)


def expect_continue():
    """A POST that waits for 100 Continue before its body, as curl and others send a large one."""
    parsed = urllib.parse.urlparse(base)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port)
    body = b"y" * 4096

    connection.putrequest("POST", "/echo")
    connection.putheader("Content-Length", str(len(body)))
    connection.putheader("Expect", "100-continue")
    connection.endheaders()
    connection.send(body)
    response = connection.getresponse()
    assert response.status == 200 and response.read() == body


def http_1_0():
    parsed = urllib.parse.urlparse(base)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port)
    connection._http_vsn, connection._http_vsn_str = 10, "HTTP/1.0"
    connection.request("GET", "/hello")
    response = connection.getresponse()
    assert response.status == 200 and response.read() == b"hello"


def websocket_echo():
    async def talk():
        url = base.replace("http", "ws", 1) + "/ws"

        async with websockets.connect(url, max_size=64 * 1024 * 1024) as socket:
            await socket.send("hello")
            assert await socket.recv() == "hello"
            await socket.send(b"\x00\x01\x02\xff")
            assert await socket.recv() == b"\x00\x01\x02\xff"
            large = "z" * (1024 * 1024)
            await socket.send(large)
            assert await socket.recv() == large
            await socket.ping()

    asyncio.run(talk())


check("requests: kept alive, POSTs, chunked, pieces, close", requests_session)
check("requests: server-sent events", requests_events)
check("httpx: GET, POST, streamed", httpx_client)
check("http.client: Expect: 100-continue", expect_continue)
check("http.client: HTTP/1.0", http_1_0)
check("websockets: echo, text, binary, 1 MiB, ping", websocket_echo)

sys.exit(1 if failures else 0)
