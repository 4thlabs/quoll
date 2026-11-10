"""An aiohttp server with the routes of tests/interop/server.cpp, which Quoll's client checks."""

from aiohttp import web, WSMsgType


async def connection(request):
    """The connection's identity, for the checks of a kept connection."""
    return web.Response(text=str(id(request.transport)))


async def hello(request):
    return web.Response(text="hello")


async def echo(request):
    return web.Response(body=await request.read())


async def pieces(request):
    response = web.StreamResponse()
    await response.prepare(request)

    for index in range(int(request.match_info["count"])):
        await response.write(f"piece {index}\n".encode())

    await response.write_eof()
    return response


async def events(request):
    response = web.StreamResponse(headers={"Content-Type": "text/event-stream", "Cache-Control": "no-store"})
    await response.prepare(request)

    for index in range(int(request.match_info["count"])):
        await response.write(f"event: count\nid: {index}\ndata: event {index}\n\n".encode())

    await response.write_eof()
    return response


async def close(request):
    return web.Response(text="bye", headers={"Connection": "close"})


async def ws(request):
    socket = web.WebSocketResponse(max_msg_size=64 * 1024 * 1024)
    await socket.prepare(request)

    async for message in socket:
        if message.type == WSMsgType.TEXT:
            await socket.send_str(message.data)
        elif message.type == WSMsgType.BINARY:
            await socket.send_bytes(message.data)

    return socket


app = web.Application(client_max_size=64 * 1024 * 1024)
app.add_routes([
    web.get("/connection", connection),
    web.get("/hello", hello),
    web.post("/echo", echo),
    web.get(r"/pieces/{count:\d+}", pieces),
    web.get(r"/events/{count:\d+}", events),
    web.get("/close", close),
    web.get("/ws", ws),
])

web.run_app(app, host="0.0.0.0", port=8080)
