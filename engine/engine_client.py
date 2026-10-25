"""Python wrapper around the C++ warehouse engine (engine_cli).

Each call starts the engine program, sends one JSON request on its stdin and
reads one JSON response from its stdout. There is no shared state between
calls, so the caller (FastAPI + SQLite later) keeps the data and sends what the
engine needs each time.

    from engine_client import plan_trip

    result = plan_trip(
        floor={"nodes": 3, "edges": [[0, 1, 4], [1, 2, 3]]},
        capacity=50,
        inventory=[{"sku": 101, "node": 2}],
        orders=[{"id": 1, "sku": 101, "weight": 10, "value": 60, "urgency": 1}],
    )
    print(result["route"], result["distance"])

Where the engine program is looked for, in order:
  1. the `engine_path` argument,
  2. the WAREHOUSE_ENGINE environment variable,
  3. ../engine/engine_cli (engine_cli.exe on Windows) next to this folder.
"""

import json
import os
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

PathLike = Union[str, "os.PathLike[str]"]


class EngineError(RuntimeError):
    """The engine is missing, crashed, timed out, or rejected the request."""


def default_engine_path() -> Path:
    name = "engine_cli.exe" if os.name == "nt" else "engine_cli"
    return Path(__file__).resolve().parent.parent / "engine" / name


def call_engine(
    request: Dict[str, Any],
    *,
    engine_path: Optional[PathLike] = None,
    timeout: float = 30.0,
) -> Dict[str, Any]:
    """Send one request to the engine and return its JSON response as a dict.

    Raises EngineError if the engine cannot run or answers with "ok": false.
    """
    path = Path(engine_path or os.environ.get("WAREHOUSE_ENGINE") or default_engine_path())
    if not path.is_file():
        raise EngineError(
            f"engine program not found at {path}. Build it first: "
            "in the engine folder run  g++ -std=c++14 -O2 engine_cli.cpp -o engine_cli"
        )

    payload = json.dumps(request).encode("utf-8")
    try:
        proc = subprocess.run(
            [str(path)],
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        raise EngineError(f"the engine took longer than {timeout} seconds") from None
    except OSError as exc:
        raise EngineError(f"could not start the engine: {exc}") from exc

    raw = proc.stdout.decode("utf-8", errors="replace").strip()
    try:
        response = json.loads(raw)
    except ValueError:
        stderr = proc.stderr.decode("utf-8", errors="replace").strip()[:500]
        raise EngineError(
            f"the engine did not return valid JSON (exit code {proc.returncode}). {stderr}"
        ) from None

    if not response.get("ok"):
        raise EngineError(response.get("error", "unknown engine error"))
    return response


# ---------- one small function per operation ----------


def ping(**kw: Any) -> Dict[str, Any]:
    return call_engine({"op": "ping"}, **kw)


def plan_trip(
    floor: Dict[str, Any],
    capacity: int,
    inventory: List[Dict[str, int]],
    orders: List[Dict[str, Any]],
    depot: int = 0,
    max_candidates: int = 12,
    **kw: Any,
) -> Dict[str, Any]:
    """Orders -> one optimised robot trip (see engine/README.md for the fields)."""
    return call_engine(
        {
            "op": "plan_trip",
            "floor": floor,
            "depot": depot,
            "capacity": capacity,
            "max_candidates": max_candidates,
            "inventory": inventory,
            "orders": orders,
        },
        **kw,
    )


def shortest_path(floor: Dict[str, Any], source: int, target: int, **kw: Any) -> Dict[str, Any]:
    return call_engine({"op": "shortest_path", "floor": floor, "from": source, "to": target}, **kw)


def network_mst(floor: Dict[str, Any], algorithm: str = "kruskal", **kw: Any) -> Dict[str, Any]:
    return call_engine({"op": "network_mst", "floor": floor, "algorithm": algorithm}, **kw)


def lookup_sku(inventory: List[Dict[str, int]], sku: int, **kw: Any) -> Dict[str, Any]:
    return call_engine({"op": "lookup_sku", "inventory": inventory, "sku": sku}, **kw)


def sku_range(
    inventory: List[Dict[str, int]], lo: int, hi: int, limit: int = 1000, **kw: Any
) -> Dict[str, Any]:
    return call_engine(
        {"op": "sku_range", "inventory": inventory, "lo": lo, "hi": hi, "limit": limit}, **kw
    )
