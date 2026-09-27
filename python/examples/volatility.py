# Copyright (c) 2026 OpenPort contributors. MIT License.
"""Print SPX's 30-day model-free IV and 25-delta skew, preserving missing data."""
import os
from openport import Client

client = Client(os.environ.get("OPENPORT_URL", "http://127.0.0.1:8080"), os.environ.get("OPENPORT_WRITE_TOKEN"))
provider = client.status()["provider"]
metrics = client.volatility("SPX")
print("SIMULATED PRICES" if provider.get("simulated") else "Provider market data", provider["name"],
      f"delay={provider['delay_seconds']}s", f"as_of={metrics['as_of']}")
iv = next((point for point in metrics["mfiv"]["constant"] if point["days"] == 30), None)
print("30-day model-free IV (vol points):", iv)
print("25-delta skew (vol points):", metrics["skew"]["delta25"], metrics["skew"]["reason"])
