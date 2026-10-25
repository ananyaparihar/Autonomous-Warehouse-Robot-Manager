"""Request and response shapes for the API (Pydantic validates them for us).

Limits are chosen so that an order's weight and value (unit x quantity) always
fit in the engine's 32-bit integers: 100,000 x 10,000 = 1,000,000,000 < 2^31.
"""

from typing import Any, Dict, List, Optional

from pydantic import BaseModel, ConfigDict, Field

INT32_MAX = 2_147_483_647


class Strict(BaseModel):
    """Base for requests: a misspelled field is an error, not silently ignored."""

    model_config = ConfigDict(extra="forbid")


# ---------------- floor ----------------

class Edge(Strict):
    u: int = Field(ge=0, description="node at one end of the aisle")
    v: int = Field(ge=0, description="node at the other end")
    length: int = Field(ge=0, le=1_000_000)


class FloorIn(Strict):
    nodes: int = Field(ge=1, le=100_000, description="floor nodes are numbered 0..nodes-1")
    depot: int = Field(default=0, ge=0, description="node where the robot starts and ends")
    capacity: int = Field(ge=0, le=1_000_000, description="robot payload limit")
    edges: List[Edge] = Field(default_factory=list, max_length=200_000)


class FloorOut(FloorIn):
    pass


class PathOut(BaseModel):
    reachable: bool
    distance: Optional[int]
    path: List[int]


class NetworkOut(BaseModel):
    algorithm: str
    spanning: bool
    components: int
    total_weight: int
    edges: List[List[int]]


# ---------------- products ----------------

class ProductIn(Strict):
    sku: int = Field(ge=0, le=INT32_MAX)
    name: str = Field(min_length=1, max_length=100)
    node: int = Field(ge=0, description="floor node where this product's shelf is")
    quantity: int = Field(default=0, ge=0, le=1_000_000_000, description="starting stock")
    unit_weight: int = Field(ge=0, le=100_000)
    unit_value: int = Field(ge=0, le=100_000)


class ProductPatch(Strict):
    name: Optional[str] = Field(default=None, min_length=1, max_length=100)
    node: Optional[int] = Field(default=None, ge=0)
    unit_weight: Optional[int] = Field(default=None, ge=0, le=100_000)
    unit_value: Optional[int] = Field(default=None, ge=0, le=100_000)


class ProductOut(BaseModel):
    sku: int
    name: str
    node: int
    quantity: int = Field(description="units on the shelf")
    reserved: int = Field(description="units promised to pending or planned orders")
    available: int = Field(description="quantity minus reserved")
    unit_weight: int
    unit_value: int


class RestockIn(Strict):
    quantity: int = Field(gt=0, le=1_000_000_000)


class AdjustIn(Strict):
    delta: int = Field(description="positive or negative correction, not zero")
    reason: str = Field(min_length=1, max_length=100)


class MovementOut(BaseModel):
    id: int
    sku: int
    delta: int
    reason: str
    order_id: Optional[int]
    created_at: str


# ---------------- orders ----------------

class OrderIn(Strict):
    sku: int = Field(ge=0, le=INT32_MAX)
    quantity: int = Field(gt=0, le=10_000)
    urgency: int = Field(default=0, ge=-1000, le=1000, description="lower number = more urgent")


class OrderOut(BaseModel):
    id: int
    sku: int
    quantity: int
    urgency: int
    status: str
    trip_id: Optional[int]
    note: Optional[str]
    weight: int
    value: int
    created_at: str


# ---------------- trips ----------------

class StopOut(BaseModel):
    node: int
    orders: List[int]


class TripOut(BaseModel):
    id: int
    status: str
    order_ids: List[int]
    stops: List[StopOut]
    route: List[int]
    distance: int
    total_value: int
    total_weight: int
    stats: Dict[str, Any]
    created_at: str
    finished_at: Optional[str]


class RejectedOut(BaseModel):
    order_id: int
    reason: str


class PlanOut(BaseModel):
    trip: Optional[TripOut]
    deferred: List[int] = Field(description="pending orders left for a later trip")
    rejected: List[RejectedOut]
    message: str


# ---------------- misc ----------------


class FleetOut(BaseModel):
    trips: List[TripOut] = Field(description="one trip per robot that has work, robot 1 first")
    deferred: List[int] = Field(description="pending orders left for a later round")
    rejected: List[RejectedOut]
    message: str
    
class StatsOut(BaseModel):
    products: int
    units_in_stock: int
    orders: Dict[str, int]
    trips: Dict[str, int]
