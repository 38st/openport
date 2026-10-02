# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Core wire types from docs/openapi.yaml. Results remain ordinary dictionaries.

Unknown fields are preserved. Money and identifiers stay strings, not floats.
"""
from typing import Any, TypedDict

JSON = dict[str, Any]


class Provider(TypedDict):
    name: str
    realtime: bool
    delay_seconds: int
    simulated: bool


class Status(TypedDict):
    provider: Provider
    underlyings: list[JSON]
    feed: JSON
    engine: JSON


class Summary(TypedDict):
    symbol: str
    spot: float | None
    as_of: str | None
    version: int
    expiries: list[JSON]
    exposure: JSON


class Chain(TypedDict):
    symbol: str
    spot: float | None
    as_of: str | None
    version: int
    expiry: JSON
    strikes: list[JSON]


class Surface(TypedDict):
    symbol: str
    as_of: str | None
    expiries: list[JSON]


class Order(TypedDict):
    id: str
    client_order_id: str
    actor: str
    symbol: str | None
    quantity: int
    filled_quantity: int
    limit_price: str | None
    status: str


class Fill(TypedDict):
    id: str
    order_id: str
    actor: str
    symbol: str
    quantity: int
    price: str
    fee: str
    time: str


class Orders(TypedDict):
    account_version: str
    orders: list[Order]


class Fills(TypedDict):
    account_version: str
    fills: list[Fill]


class OrderResult(TypedDict):
    account_version: str
    order: Order


class Trades(TypedDict):
    account_version: str
    attempt: int
    trades: list[JSON]


class Portfolio(TypedDict):
    account_version: str
    time: str
    cash: str
    equity: str
    positions: list[JSON]


class Account(TypedDict):
    account_version: str
    time: str
    rules: JSON
    evaluation: JSON


class ReplayListing(TypedDict):
    directory: str
    recordings: list[JSON]
    replay: JSON | None


class ReplayResult(TypedDict):
    replay: JSON | None


class Exposure(TypedDict):
    symbol: str
    as_of: str | None
    spot: float | None
    strikes: list[float]
    expiries: list[JSON]
    total_gex: list[float | None]
    exposure: JSON


class Volatility(TypedDict):
    symbol: str
    as_of: str
    units: str
    mfiv: JSON
    atm: JSON
    skew: JSON
    realized: JSON
    vrp: JSON
    implied_moves: JSON


class Candles(TypedDict):
    symbol: str
    interval: str
    bars: list[JSON]


class Risk(TypedDict):
    account_version: str
    limits_revision: str
    limits: JSON
    complete: bool
    daily_loss: str
    kill: JSON
    aggregate: JSON
    underlyings: dict[str, JSON]
    scenarios: JSON


class OrderPreview(TypedDict):
    account_version: str
    decision: str
    reason: JSON | None
    buying_power: JSON
    max_loss: str | None
    max_units: int | None
    max_units_buying_power: int | None
    max_units_floor: int | None
    breach: JSON
    execution: JSON
    simulated: bool


class WhatIfResult(TypedDict):
    account_version: str
    current: JSON
    candidates: list[JSON]
    simulated: bool


class FlattenPreview(TypedDict):
    account_version: str
    decision: str
    reason: JSON | None
    cancelled_orders: list[str]
    orders: list[JSON]
    fills: list[JSON]
    stock_fills: list[JSON]
    kept_stocks: list[JSON]
    remaining: list[JSON]
    remaining_shares: list[JSON]
    current: JSON | None
    after: JSON | None
    simulated: bool


class SubmitResult(OrderResult):
    fills: list[Fill]


class Plans(TypedDict):
    plans: list[JSON]


class Accounts(TypedDict):
    accounts: list[JSON]


class EquityHistory(TypedDict):
    samples: list[JSON]
    error: str | None


class CancelAllResult(TypedDict):
    account_version: str
    cancelled_orders: list[str]


class FlattenResult(CancelAllResult):
    orders: list[Order]
    fills: list[Fill]
    stock_fills: list[JSON]
    kept_stocks: list[JSON]
