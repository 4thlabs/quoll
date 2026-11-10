"""Python's clients (requests, httpx, websockets, http.client) against Quoll's server, at QUOLL_URL."""

import asyncio
import http.client
import os
import socket
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
    """A POST that waits for 100 Continue before its body, as curl and others send a large one: by hand, as
    http.client sends the body at once."""
    parsed = urllib.parse.urlparse(base)
    body = b"y" * 4096

    with socket.create_connection((parsed.hostname, parsed.port), timeout=10) as connection:
        connection.sendall(
            f"POST /echo HTTP/1.1\r\nHost: {parsed.hostname}\r\nContent-Length: {len(body)}\r\n"
            "Expect: 100-continue\r\n\r\n".encode()
        )
        interim = b""

        while b"\r\n\r\n" not in interim:
            interim += connection.recv(4096)

        assert interim.startswith(b"HTTP/1.1 100 "), interim
        connection.sendall(body)
        final = b""

        while not final.endswith(body):
            part = connection.recv(65536)
            assert part, final
            final += part

        assert final.startswith(b"HTTP/1.1 200 "), final[:100]


def kept_alive():
    """Five GETs and two POSTs on one connection, which http.client would open again, silently, if it were closed."""
    parsed = urllib.parse.urlparse(base)
    connection = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=10)
    first = None

    for turn in range(7):
        if turn < 5:
            connection.request("GET", "/hello")
        else:
            connection.request("POST", "/echo", body=b"kept")

        response = connection.getresponse()
        response.read()
        assert response.status == 200
        first = first or connection.sock
        assert connection.sock is first, f"a new connection at request {turn}"


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
            pong = await socket.ping()
            await asyncio.wait_for(pong, timeout=10)

    asyncio.run(talk())


check("requests: kept alive, POSTs, chunked, pieces, close", requests_session)
check("requests: server-sent events", requests_events)
check("httpx: GET, POST, streamed", httpx_client)
check("raw: Expect: 100-continue, the body after the 100", expect_continue)
check("http.client: GETs and POSTs on one kept connection", kept_alive)
check("http.client: HTTP/1.0", http_1_0)
check("websockets: echo, text, binary, 1 MiB, ping", websocket_echo)

sys.exit(1 if failures else 0)
