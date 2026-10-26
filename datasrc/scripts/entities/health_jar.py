# Health jar: a scripted prop built from the generic entity module.
#
# C++ owns position, health and lifetime. This script only draws the jar,
# tracks its cooldown/owner in state, and drops a health pickup when its owner
# stands close. Damage is applied by CScriptedEntity after on_hit, so on_hit
# only has to keep the state.
#
# Snapshot ids are runtime handles: allocate them once per live jar, reuse them
# for every visual update, and let the C++ entity return them on destruction.

import lastday


DEFAULT_COOLDOWN = 45
DEFAULT_RANGE = 96

OWNER_CHECK_TICKS = 10
DROP_JITTER = 8
NEAR_DROP_RADIUS = 96.0

BEAM_COUNT = 6
# runtime-only key; removed before the state is persisted
SNAP_ID_KEY = "_snap_ids"
SNAP_ID_COUNT = BEAM_COUNT + 1  # six jar beams and the centre pickup


def _normalize(ctx):
    """Return a valid state dict and fill the defaults a fresh save may miss."""
    state = ctx.state
    if not isinstance(state, dict):
        state = {}
        ctx.state = state

    params = ctx.params
    if not isinstance(params, dict):
        params = {}

    if "cooldown" not in state:
        state["cooldown"] = int(params.get("cooldown", DEFAULT_COOLDOWN))
    if "range" not in state:
        state["range"] = int(params.get("range", DEFAULT_RANGE))
    if "item" not in state:
        state["item"] = params.get("item", "")
    if "owner" not in state:
        state["owner"] = params.get("owner", "")
    if "owner_name" not in state:
        state["owner_name"] = params.get("owner_name", "")
    if "next_drop" not in state:
        # a freshly placed jar is ready immediately
        state["next_drop"] = int(ctx.now)
    if "tick_speed" not in state:
        state["tick_speed"] = int(ctx.tick_speed)
    return state


def _visual_snap_ids(ctx):
    """Return this jar's ids, allocating them on first use and reusing after."""
    ids = ctx.state.get(SNAP_ID_KEY)
    if not isinstance(ids, list) or len(ids) != SNAP_ID_COUNT:
        ids = [ctx.snap_new_id() for _ in range(SNAP_ID_COUNT)]
        ctx.state[SNAP_ID_KEY] = ids
    return ids


def _laser(snap_id, x1, y1, x2, y2):
    return {"id": snap_id, "x1": x1, "y1": y1, "x2": x2, "y2": y2}


def _draw_jar(ctx):
    """Describe the jar as snapshot primitives; the ids stay unchanged."""
    if ctx.entity is None:
        return

    ids = _visual_snap_ids(ctx)
    beams = ids[:BEAM_COUNT]
    pickup_id = ids[BEAM_COUNT]

    # a jar: closed bottom, straight walls, sloped shoulders and a narrow lid.
    # The centre pickup is a snapshot primitive only; the real pickup is the
    # HealthDrop the jar spits out.
    ctx.entity.set_visual({
        "lasers": [
            _laser(beams[0], -32, 48, 32, 48),
            _laser(beams[1], -32, 48, -32, -24),
            _laser(beams[2], 32, 48, 32, -24),
            _laser(beams[3], -32, -24, -14, -48),
            _laser(beams[4], 32, -24, 14, -48),
            _laser(beams[5], -14, -48, 14, -48),
        ],
        "pickups": [
            {"id": pickup_id, "x": 0, "y": 0, "type": 0},
        ],
    })


def _ready_to_drop(ctx, state):
    """Whether this tick may start an owner check for a pickup drop."""
    now = int(ctx.now)
    if now < int(state["next_drop"]):
        return False
    # The entity id staggers the scan so several jars do not all check on the
    # same tick.
    if (now + ctx.entity.id) % OWNER_CHECK_TICKS != 0:
        return False
    # The building lock is enforced in C++: only the owner, their team or
    # everyone (depending on the lock) counts as an allowed visitor.
    return ctx.entity.allowed_player_near() >= 0


def _drop_health(ctx, state):
    pos = ctx.entity.pos
    jitter = int(lastday.random_int() % (DROP_JITTER * 2 + 1)) - DROP_JITTER

    # HealthDrop.near refuses while an unclaimed drop is already nearby, so the
    # cooldown only restarts when a pickup actually appeared.
    if ctx.HealthDrop.near(pos, NEAR_DROP_RADIUS):
        return
    drop = ctx.HealthDrop(ctx.Vec2(pos.x + jitter, pos.y), ctx.Vec2(0.0, 1.0))
    if drop.id >= 0:
        state["next_drop"] = int(ctx.now) + int(state["cooldown"]) * int(state["tick_speed"])


def on_spawn(ctx):
    state = _normalize(ctx)
    # A recycled entity id may still carry a stale saved key; start clean.
    state.pop(SNAP_ID_KEY, None)
    _draw_jar(ctx)


def on_load(ctx):
    # the restored state already carries cooldown/owner; only stale or partial
    # documents need defaults
    _normalize(ctx)
    _draw_jar(ctx)


def on_tick(ctx):
    state = _normalize(ctx)
    if ctx.entity is None or not _ready_to_drop(ctx, state):
        return
    _drop_health(ctx, state)


def on_hit(ctx):
    # CScriptedEntity applies the damage after this callback. Keep the state
    # object (and therefore the cooldown) across the hit.
    _normalize(ctx)


def on_destroy(ctx):
    state = _normalize(ctx)
    state.pop(SNAP_ID_KEY, None)

    item = state["item"]
    if not item or ctx.entity is None:
        return

    # dismantle: the jar item falls out where it stood
    pos = ctx.entity.pos
    ctx.DroppedPickup(pos, ctx.Vec2(0.0, 0.0), item, 1)


def on_save(ctx):
    state = _normalize(ctx)
    # The world is being torn down; the C++ destructor returns the ids, and
    # dropping the key keeps the persisted state free of runtime handles.
    state.pop(SNAP_ID_KEY, None)
