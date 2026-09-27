# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Optional tabular views; money stays as its original decimal string."""


def _pandas():
    try:
        import pandas
    except ImportError as error:
        raise ImportError("Install openport[pandas] to use frame helpers") from error
    return pandas


def chain_frame(chain):
    rows = []
    for strike in chain["strikes"]:
        for side in ("call", "put"):
            if strike.get(side) is not None:
                rows.append({**strike[side], "strike": strike["strike"], "side": side,
                             "expiry": chain["expiry"]["id"], "as_of": chain.get("as_of")})
    return _pandas().DataFrame(rows)


def smile_frame(surface):
    return _pandas().DataFrame([
        {**point, "expiry": expiry["id"], "as_of": surface.get("as_of")}
        for expiry in surface["expiries"] for point in expiry["points"]
    ])


def trades_frame(trades):
    return _pandas().DataFrame(trades["trades"])
