# Copyright (c) 2026 OpenPort contributors. MIT License.
import json


async def ticks(url: str = "ws://127.0.0.1:8080/ws", *, token: str | None = None):
    """Yield tick/replay_tick dictionaries. Requires openport[ws]."""
    try:
        from websockets.asyncio.client import connect
    except ImportError as error:
        raise ImportError("Install openport[ws] to stream ticks") from error
    headers = {"Authorization": "Bearer " + token} if token else None
    async with connect(url, additional_headers=headers) as socket:
        async for message in socket:
            yield json.loads(message)
