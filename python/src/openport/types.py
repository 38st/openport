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


class Walk(TypedDict):
    step: str
    seconds: int
    limit: str


class WalkStep(TypedDict):
    time: str
    limit_price: str


class OrderWalk(TypedDict, total=False):
    walk: Walk | None
    next_walk: WalkStep | None


class Order(OrderWalk):
    id: str
    client_order_id: str
    actor: str
    symbol: str | None
    quantity: int
    filled_quantity: int
    limit_price: str | None
    time_in_force: str
    good_till: str | None
    limit_ticks: int | None
    status: str


class FeeSchedule(TypedDict):
    open: str
    close: str
    leg_cap: str
    clearing: str
    regulatory: str
    index: dict[str, str]
    exercise: str


class FillFees(TypedDict):
    commission: str
    clearing: str
    regulatory: str
    index: str


class Fill(TypedDict):
    id: str
    order_id: str
    actor: str
    symbol: str
    quantity: int
    price: str
    fee: str
    fees: FillFees | None
    time: str
    quote: JSON | None


class Orders(TypedDict):
    account_version: str
    orders: list[Order]


class Fills(TypedDict):
    account_version: str
    fills: list[Fill]


class OrderResult(TypedDict):
    account_version: str
    order: Order


class Settlements(TypedDict):
    account_version: str
    settlements: list[JSON]


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


class AccountRules(TypedDict, total=False):
    plan_id: str | None
    plan: str | None
    phase: str
    profit_target: str | None
    max_drawdown: str | None
    drawdown_mode: str
    lock_balance: str | None
    buy_only: bool
    defined_risk: bool
    slippage_ticks: int
    fill_latency_ms: int
    impact_ticks: int
    inside_fill_percent: int
    margin: str
    buying_power: bool
    expiry_cutoff_seconds: int
    payouts: JSON | None
    fees: FeeSchedule


class AccountDamage(TypedDict):
    reason: str
    last_good_seq: int
    last_good_time: str | None


class JournalSize(TypedDict):
    bytes: int
    records: int
    warning: str | None


class AccountStorage(TypedDict, total=False):
    damaged: AccountDamage | None
    journal_size: JournalSize


class Account(AccountStorage):
    account_version: str
    time: str
    rules: AccountRules
    evaluation: JSON


class ReplayListing(TypedDict):
    directory: str
    recordings: list[JSON]
    replay: JSON | None


class RunVerification(TypedDict, total=False):
    status: str
    message: str
    progress: float
    transactions: int
    equity: str | None
    head: str | None
    build: str
    time: str | None
    finished_at: str
    run: JSON


class ReplayFeedback(TypedDict, total=False):
    settled_through: str
    aborted: bool
    message: str


class ReplayResult(ReplayFeedback):
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


class RiskProfile(TypedDict):
    account_version: str
    time: str
    underlying: str | None
    benchmark: str | None
    reference: str
    spot: float | None
    vol_points: float
    equity: str
    room: str | None
    soft_room: str | None
    percent: list[float]
    prices: list[float | None]
    curves: list[JSON]
    betas: list[JSON]
    weighted_dollar_delta: float | None
    weighted_delta: float | None
    complete: bool
    model: str


class Probability(TypedDict):
    symbol: str
    as_of: str
    spot: float | None
    horizons: list[JSON]
    model: str


class OrderPreview(TypedDict):
    next_walk: WalkStep | None
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
    liquidity: list[JSON]
    warnings: list[JSON]
    fee: str | None
    fees: FillFees | None
    simulated: bool


class WhatIfResult(TypedDict):
    account_version: str
    current: JSON
    candidates: list[JSON]
    simulated: bool


class StockPreview(TypedDict):
    account_version: str
    simulated: bool
    decision: str
    reason: JSON | None
    price: str | None
    cost: str | None
    current: JSON
    after: JSON


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


class EquityPaging(TypedDict, total=False):
    next: str | None
    error_time: str | None
    error_market_time: str | None
    error_recovered: bool


class EquityHistory(EquityPaging):
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
    residuals: list[JSON]
