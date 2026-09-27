# Copyright (c) 2026 OpenPort contributors. MIT License.
import argparse
import os
from openport import Client
from . import create_server


def main():
    parser = argparse.ArgumentParser(description="MCP tools for openport's paper simulator")
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    parser.add_argument("--agent-name", default="openport")
    args = parser.parse_args()
    client = Client(args.url, token=os.environ.get("OPENPORT_WRITE_TOKEN"))
    create_server(client, args.agent_name).run("stdio")


if __name__ == "__main__":
    main()
