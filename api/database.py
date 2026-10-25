"""SQLite storage for the warehouse: schema, connections and transactions.

Uses only the standard library (sqlite3), so there is nothing to install.

Tables
  floor            one row: node count, depot node, robot payload capacity
  floor_edges      the aisles: node_a, node_b, length
  products         SKU, name, shelf node, stock quantity, unit weight and value
  orders           what customers asked for, with a status
  trips            robot trips planned by the C++ engine
  stock_movements  every change to stock (restock, adjustment, pick), for history
"""

import sqlite3
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator, Union

DEFAULT_DB_PATH = Path(__file__).resolve().parent / "warehouse.db"

SCHEMA = """
CREATE TABLE IF NOT EXISTS floor (
    id        INTEGER PRIMARY KEY CHECK (id = 1),       -- only one floor layout
    nodes     INTEGER NOT NULL CHECK (nodes >= 1),
    depot     INTEGER NOT NULL CHECK (depot >= 0),
    capacity  INTEGER NOT NULL CHECK (capacity >= 0)
);

CREATE TABLE IF NOT EXISTS floor_edges (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    u       INTEGER NOT NULL CHECK (u >= 0),
    v       INTEGER NOT NULL CHECK (v >= 0),
    length  INTEGER NOT NULL CHECK (length >= 0)
);

CREATE TABLE IF NOT EXISTS products (
    sku          INTEGER PRIMARY KEY,
    name         TEXT    NOT NULL,
    node         INTEGER NOT NULL CHECK (node >= 0),
    quantity     INTEGER NOT NULL DEFAULT 0 CHECK (quantity >= 0),
    unit_weight  INTEGER NOT NULL CHECK (unit_weight >= 0),
    unit_value   INTEGER NOT NULL CHECK (unit_value >= 0)
);

CREATE TABLE IF NOT EXISTS trips (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    status        TEXT    NOT NULL CHECK (status IN ('planned', 'completed', 'cancelled')),
    route         TEXT    NOT NULL,      -- JSON list of floor nodes
    stops         TEXT    NOT NULL,      -- JSON list of {node, orders}
    distance      INTEGER NOT NULL,
    total_value   INTEGER NOT NULL,
    total_weight  INTEGER NOT NULL,
    stats         TEXT    NOT NULL,      -- JSON: engine statistics
    created_at    TEXT    NOT NULL,
    finished_at   TEXT
);

CREATE TABLE IF NOT EXISTS orders (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    sku         INTEGER NOT NULL REFERENCES products(sku),
    quantity    INTEGER NOT NULL CHECK (quantity > 0),
    urgency     INTEGER NOT NULL DEFAULT 0,          -- lower number = more urgent
    status      TEXT    NOT NULL CHECK (status IN ('pending', 'planned', 'completed', 'cancelled', 'rejected')),
    trip_id     INTEGER REFERENCES trips(id),
    note        TEXT,
    created_at  TEXT    NOT NULL
);

CREATE TABLE IF NOT EXISTS stock_movements (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    sku         INTEGER NOT NULL,                    -- no foreign key: history outlives the product
    delta       INTEGER NOT NULL,
    reason      TEXT    NOT NULL,                    -- restock | adjustment | pick | ...
    order_id    INTEGER,
    created_at  TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_orders_status ON orders(status);
CREATE INDEX IF NOT EXISTS idx_orders_sku    ON orders(sku);
CREATE INDEX IF NOT EXISTS idx_orders_trip   ON orders(trip_id);
CREATE INDEX IF NOT EXISTS idx_moves_sku     ON stock_movements(sku);
"""


def now() -> str:
    """Current UTC time as an ISO string, e.g. 2026-10-18T09:30:00+00:00."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def connect(db_path: Union[str, Path]) -> sqlite3.Connection:
    # isolation_level=None: we control transactions ourselves (see transaction()).
    # check_same_thread=False: FastAPI may open the connection in one worker thread
    # and use it in another. Each request has its own connection, so this is safe.
    conn = sqlite3.connect(str(db_path), isolation_level=None, timeout=10, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def init_schema(db_path: Union[str, Path]) -> None:
    conn = connect(db_path)
    try:
        conn.executescript(SCHEMA)
    finally:
        conn.close()


@contextmanager
def transaction(conn: sqlite3.Connection) -> Iterator[None]:
    """All-or-nothing block. Any exception inside rolls every change back.

    BEGIN IMMEDIATE takes the write lock up front, so two requests cannot both
    read the same stock level and then both subtract from it.
    """
    conn.execute("BEGIN IMMEDIATE")
    try:
        yield
    except BaseException:
        conn.execute("ROLLBACK")
        raise
    else:
        conn.execute("COMMIT")
