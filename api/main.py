"""Warehouse Robot Manager: the web API.

Run it (from the api folder):
    py -m uvicorn main:app --reload
then open  http://127.0.0.1:8000/docs  for an interactive page where you can try every endpoint.

What goes where
    SQLite (database.py)   stores floor, products, stock, orders, trips
    services.py            the business rules
    C++ engine             plans trips, shortest paths and the conveyor network
                           (called through engine_client.py)

Environment variables (all optional)
    WAREHOUSE_DB        path of the SQLite file   (default: api/warehouse.db)
    WAREHOUSE_ENGINE    path of the engine program (default: ../engine/engine_cli[.exe])
    ALLOWED_ORIGINS     comma-separated websites allowed to call this API from a browser
"""

import os
import sqlite3
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, Dict, Iterator, List, Literal, Optional

from fastapi import Depends, FastAPI, Query, Request, Response
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

import engine_client
import services
from database import DEFAULT_DB_PATH, connect, init_schema
from engine_client import EngineError
from schemas import (
    AdjustIn,FleetOut, FloorIn, FloorOut, MovementOut, NetworkOut, OrderIn, OrderOut, PathOut, PlanOut,
    ProductIn, ProductOut, ProductPatch, RestockIn, StatsOut, TripOut,
)

OrderStatus = Literal["pending", "planned", "completed", "cancelled", "rejected"]
TripStatus = Literal["planned", "completed", "cancelled"]

# Websites allowed to call the API from a browser. Add your own with ALLOWED_ORIGINS.
DEFAULT_ORIGINS = [
    "https://ananyaparihar.github.io",
    "http://localhost:3000", "http://127.0.0.1:3000",
    "http://localhost:5500", "http://127.0.0.1:5500",
    "http://localhost:8000", "http://127.0.0.1:8000",
]


def create_app(
    db_path: Optional[str] = None,
    engine_path: Optional[str] = None,
    engine_timeout: float = 30.0,
    allowed_origins: Optional[List[str]] = None,
) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        init_schema(app.state.db_path)          # create the tables on first start
        yield

    app = FastAPI(
        title="Warehouse Robot Manager API",
        description="Inventory, orders and robot trips. Trips are planned by a C++ engine "
                    "(min-heap, B+ Tree, knapsack DP, Dijkstra, Branch and Bound).",
        version="1.0",
        lifespan=lifespan,
    )
    app.state.db_path = db_path or os.environ.get("WAREHOUSE_DB") or str(DEFAULT_DB_PATH)
    app.state.engine = {"engine_path": engine_path, "timeout": engine_timeout}

    origins = allowed_origins
    if origins is None:
        extra = os.environ.get("ALLOWED_ORIGINS", "")
        origins = DEFAULT_ORIGINS + [o.strip() for o in extra.split(",") if o.strip()]
    app.add_middleware(CORSMiddleware, allow_origins=origins, allow_methods=["*"], allow_headers=["*"])

    # ---------- shared dependencies and error handling ----------

    def get_db(request: Request) -> Iterator[sqlite3.Connection]:
        conn = connect(request.app.state.db_path)
        try:
            yield conn
        finally:
            conn.close()

    def get_engine(request: Request) -> Dict[str, Any]:
        return request.app.state.engine

    @app.exception_handler(EngineError)
    async def engine_error_handler(request: Request, exc: EngineError) -> JSONResponse:
        # 502: this API is fine, the C++ engine behind it is not.
        return JSONResponse(status_code=502, content={"detail": f"engine problem: {exc}"})

    @app.exception_handler(sqlite3.OperationalError)
    async def database_error_handler(request: Request, exc: sqlite3.OperationalError) -> JSONResponse:
        return JSONResponse(status_code=503, content={"detail": f"database problem: {exc}"})

    Db = Depends(get_db)
    Engine = Depends(get_engine)

    # ---------- health and summary ----------

    @app.get("/health", tags=["system"])
    def health(conn: sqlite3.Connection = Db, engine: Dict[str, Any] = Engine) -> Any:
        """Checks the database and the C++ engine."""
        conn.execute("SELECT 1").fetchone()
        try:
            pong = engine_client.ping(**engine)
        except EngineError as exc:
            return JSONResponse(status_code=503, content={"status": "degraded", "database": "ok",
                                                          "engine": "unavailable", "detail": str(exc)})
        return {"status": "ok", "database": "ok", "engine": pong["version"]}

    @app.get("/stats", response_model=StatsOut, tags=["system"])
    def stats(conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """Counts for a dashboard."""
        return services.stats(conn)

    # ---------- floor ----------

    @app.get("/floor", response_model=FloorOut, tags=["floor"])
    def read_floor(conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        return services.require_floor(conn)

    @app.put("/floor", response_model=FloorOut, tags=["floor"])
    def replace_floor(body: FloorIn, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """Set the whole floor layout: node count, depot, robot capacity and aisles."""
        return services.set_floor(conn, body.model_dump())

    @app.get("/floor/path", response_model=PathOut, tags=["floor"])
    def floor_path(source: int = Query(ge=0), target: int = Query(ge=0),
                   conn: sqlite3.Connection = Db, engine: Dict[str, Any] = Engine) -> Dict[str, Any]:
        """Shortest route between two floor nodes (Dijkstra, in the C++ engine)."""
        return services.shortest_path(conn, source, target, engine)

    @app.get("/floor/network", response_model=NetworkOut, tags=["floor"])
    def floor_network(algorithm: Literal["kruskal", "prim"] = "kruskal",
                      conn: sqlite3.Connection = Db, engine: Dict[str, Any] = Engine) -> Dict[str, Any]:
        """Cheapest set of aisles that still connects every node (minimum spanning tree)."""
        return services.network(conn, algorithm, engine)

    # ---------- products and stock ----------

    @app.get("/products", response_model=List[ProductOut], tags=["products"])
    def list_products(search: Optional[str] = Query(default=None, max_length=100),
                      limit: int = Query(default=50, ge=1, le=500), offset: int = Query(default=0, ge=0),
                      conn: sqlite3.Connection = Db) -> List[Dict[str, Any]]:
        return services.list_products(conn, search, limit, offset)

    @app.post("/products", response_model=ProductOut, status_code=201, tags=["products"])
    def create_product(body: ProductIn, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        return services.create_product(conn, body.model_dump())

    @app.get("/products/{sku}", response_model=ProductOut, tags=["products"])
    def read_product(sku: int, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        return services.get_product(conn, sku)

    @app.patch("/products/{sku}", response_model=ProductOut, tags=["products"])
    def update_product(sku: int, body: ProductPatch, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """Change name, shelf node, unit weight or unit value. Stock changes go through restock/adjust."""
        return services.patch_product(conn, sku, body.model_dump())

    @app.delete("/products/{sku}", status_code=204, tags=["products"])
    def delete_product(sku: int, conn: sqlite3.Connection = Db) -> Response:
        services.delete_product(conn, sku)
        return Response(status_code=204)

    @app.post("/products/{sku}/restock", response_model=ProductOut, tags=["products"])
    def restock_product(sku: int, body: RestockIn, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """Add delivered units to stock."""
        return services.restock(conn, sku, body.quantity)

    @app.post("/products/{sku}/adjust", response_model=ProductOut, tags=["products"])
    def adjust_product(sku: int, body: AdjustIn, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """Correct stock after a count (damage, loss, miscount). Cannot drop below reserved units."""
        return services.adjust_stock(conn, sku, body.delta, body.reason)

    @app.get("/movements", response_model=List[MovementOut], tags=["products"])
    def list_movements(sku: Optional[int] = None, limit: int = Query(default=50, ge=1, le=500),
                       offset: int = Query(default=0, ge=0), conn: sqlite3.Connection = Db) -> List[Dict[str, Any]]:
        """Stock history, newest first."""
        return services.list_movements(conn, sku, limit, offset)

    # ---------- orders ----------

    @app.get("/orders", response_model=List[OrderOut], tags=["orders"])
    def list_orders(status: Optional[OrderStatus] = None, sku: Optional[int] = None,
                    limit: int = Query(default=50, ge=1, le=500), offset: int = Query(default=0, ge=0),
                    conn: sqlite3.Connection = Db) -> List[Dict[str, Any]]:
        return services.list_orders(conn, status, sku, limit, offset)

    @app.post("/orders", response_model=OrderOut, status_code=201, tags=["orders"])
    def create_order(body: OrderIn, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """Create a pending order. Rejected if stock is short or it is heavier than the robot can carry."""
        return services.create_order(conn, body.model_dump())

    @app.get("/orders/{order_id}", response_model=OrderOut, tags=["orders"])
    def read_order(order_id: int, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        return services.get_order(conn, order_id)

    @app.post("/orders/{order_id}/cancel", response_model=OrderOut, tags=["orders"])
    def cancel_order(order_id: int, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        return services.cancel_order(conn, order_id)

    # ---------- trips ----------

    @app.post("/trips/plan", response_model=PlanOut, tags=["trips"])
    def plan_trip(max_candidates: int = Query(default=12, ge=1, le=14,
                                              description="how many of the most urgent orders the engine may choose from"),
                  conn: sqlite3.Connection = Db, engine: Dict[str, Any] = Engine) -> Dict[str, Any]:
        """Plan ONE robot trip from the pending orders (the C++ engine does the work)."""
        return services.plan_trip(conn, max_candidates, engine)
    @app.post("/trips/plan-fleet", response_model=FleetOut, tags=["trips"])
    def plan_fleet(robots: int = Query(default=2, ge=1, le=20, description="how many robots share the pending orders"),
                   max_candidates: int = Query(default=12, ge=1, le=14,
                                               description="how many of the most urgent orders each robot may choose from"),
                   conn: sqlite3.Connection = Db, engine: Dict[str, Any] = Engine) -> Dict[str, Any]:
        """Plan one trip per robot: the most urgent orders go to robot 1, then robot 2, and so on."""
        return services.plan_fleet(conn, robots, max_candidates, engine)
    @app.delete("/trips/{trip_id}", status_code=204, tags=["trips"])
    def delete_trip(trip_id: int, conn: sqlite3.Connection = Db) -> Response:
        """Remove a cancelled trip from the list."""
        services.delete_trip(conn, trip_id)
        return Response(status_code=204)
    @app.get("/trips", response_model=List[TripOut], tags=["trips"])
    def list_trips(status: Optional[TripStatus] = None, limit: int = Query(default=50, ge=1, le=500),
                   offset: int = Query(default=0, ge=0), conn: sqlite3.Connection = Db) -> List[Dict[str, Any]]:
        return services.list_trips(conn, status, limit, offset)

    @app.get("/trips/{trip_id}", response_model=TripOut, tags=["trips"])
    def read_trip(trip_id: int, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        return services.get_trip(conn, trip_id)

    @app.post("/trips/{trip_id}/complete", response_model=TripOut, tags=["trips"])
    def complete_trip(trip_id: int, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """The robot finished: remove the picked goods from stock and close the orders."""
        return services.complete_trip(conn, trip_id)

    @app.post("/trips/{trip_id}/cancel", response_model=TripOut, tags=["trips"])
    def cancel_trip(trip_id: int, conn: sqlite3.Connection = Db) -> Dict[str, Any]:
        """Abandon a planned trip. Its orders go back to pending."""
        return services.cancel_trip(conn, trip_id)

    # ---------- the website (../web/index.html), served from the same address as the API ----------
    web_dir = Path(__file__).resolve().parent.parent / "web"
    if web_dir.is_dir():
        app.mount("/ui", StaticFiles(directory=str(web_dir), html=True), name="ui")

        @app.get("/", include_in_schema=False)
        def home() -> RedirectResponse:
            return RedirectResponse("/ui/")

    return app


# `uvicorn main:app` uses this. Nothing touches the disk until the server starts.
app = create_app()
