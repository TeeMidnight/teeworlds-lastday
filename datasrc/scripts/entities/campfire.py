# Campfire: a persistent fire prop built from the generic scripted entity.
#
# The base is drawn with snapshot lasers: a small hexagon of stones and two
# crossed sticks. The flames are intentionally made of WEAPON_HAMMER
# projectiles: the stock client draws no projectile sprite for that type but
# still emits its bullet trail particle for every live projectile, so a few
# short-lived stationary puffs read as embers. They are spawned with
# PLAYER_TEAM_FRIENDLY as the owner, which is the engine sentinel that hurts
# hostile bots but never players, friendly units or neutral units: a base
# campfire should not cook its own owner.
#
# The puffs are emitted as a rising wave: each emission advances one phase from
# the base outward, so the flames visibly grow from the bottom of the fire.

import math

import lastday


WEAPON_HAMMER = 0
PLAYER_TEAM_FRIENDLY = -4

DEFAULT_EMIT_TICKS = 5
DEFAULT_RANGE = 96
DEFAULT_DAMAGE = 1
PUFFS_PER_EMIT = 3

# Bottom of the entity in world units; the floor is 24 units below the centre.
BASE_Y = 22.0
SNAP_ID_KEY = "_snap_ids"
BASE_SEGMENT_COUNT = 8

# Rising wave. The flame starts at the base (phase 0) and fans outward as the
# phase advances to SPREAD_PHASES - 1.
SPREAD_PHASES = 5
FLAME_RISE = 42.0
FLAME_SIDE = 36.0
# Smallest safe distance from the entity centre for an ember. The entity's
# hitbox is radius 8 and a hammer projectile checks 6 units around its point,
# so anything closer than 14 would hit (and be absorbed by) the campfire.
SAFE_RADIUS = 15.0

PUFF_SPAN_MIN = 8
PUFF_SPAN_MAX = 12


def _random01():
    return (lastday.random_int() % 1000) / 1000.0


def _normalize(ctx):
    """Return a valid state dict and fill the defaults a fresh save may miss."""
    state = ctx.state
    if not isinstance(state, dict):
        state = {}
        ctx.state = state

    params = ctx.params if isinstance(ctx.params, dict) else {}
    if "emit_ticks" not in state:
        state["emit_ticks"] = int(params.get("emit_ticks", DEFAULT_EMIT_TICKS))
    if "range" not in state:
        state["range"] = int(params.get("range", DEFAULT_RANGE))
    if "damage" not in state:
        state["damage"] = int(params.get("damage", DEFAULT_DAMAGE))
    if "phase" not in state:
        state["phase"] = 0
    return state


def _snap_ids(ctx):
    """Return this campfire's base ids, allocating them once per lifetime."""
    state = ctx.state
    ids = state.get(SNAP_ID_KEY)
    if not isinstance(ids, list) or len(ids) != BASE_SEGMENT_COUNT:
        ids = [ctx.snap_new_id() for _ in range(BASE_SEGMENT_COUNT)]
        state[SNAP_ID_KEY] = ids
    return ids


def _laser(snap_id, x1, y1, x2, y2):
    return {"id": snap_id, "x1": x1, "y1": y1, "x2": x2, "y2": y2}


def _draw_base(ctx):
    """Draw the stone ring and crossed sticks with snapshot lasers."""
    if ctx.entity is None:
        return

    ids = _snap_ids(ctx)
    ctx.entity.set_visual({
        "lasers": [
            # hexagon of stones, bottom edge on the ground
            _laser(ids[0], -22, 18, -11, 24),
            _laser(ids[1], -11, 24, 11, 24),
            _laser(ids[2], 11, 24, 22, 18),
            _laser(ids[3], 22, 18, 11, 12),
            _laser(ids[4], 11, 12, -11, 12),
            _laser(ids[5], -11, 12, -22, 18),
            # crossed sticks inside the ring
            _laser(ids[6], -20, 15, 20, 21),
            _laser(ids[7], -20, 21, 20, 15),
        ],
    })


def _spawn_puff(ctx, state, t):
    pos = ctx.entity.pos
    base_x = pos.x
    base_y = pos.y + BASE_Y

    rise = 4.0 + (FLAME_RISE - 4.0) * t

    # Keep the ember outside the prop's hitbox: at the heights where a purely
    # vertical flame would pass through the centre, push it sideways by at
    # least the remaining safe distance.
    vertical = abs(BASE_Y - rise)
    if vertical < SAFE_RADIUS:
        min_lateral = math.sqrt(SAFE_RADIUS * SAFE_RADIUS - vertical * vertical)
    else:
        min_lateral = 0.0

    spread = 2.0 + (FLAME_SIDE - 2.0) * t
    lateral = min_lateral + 4.0 + spread * _random01()
    side = 1.0 if (lastday.random_int() & 1) else -1.0
    x = base_x + side * lateral
    y = base_y - rise

    # Do not burn through a wall next to the fire.
    if lastday.world_test_line(base_x, base_y, x, y, 4.0):
        return

    span = PUFF_SPAN_MIN + (lastday.random_int() % (PUFF_SPAN_MAX - PUFF_SPAN_MIN + 1))
    ctx.Projectile(
        PLAYER_TEAM_FRIENDLY,
        ctx.Vec2(x, y),
        ctx.Vec2(0.0, 0.0),
        span,
        max(1, int(state["damage"])),
        False,
        -1,
        WEAPON_HAMMER,
    )


def on_spawn(ctx):
    state = _normalize(ctx)
    # A recycled entity id may still carry stale saved handles; start clean.
    state.pop(SNAP_ID_KEY, None)
    _draw_base(ctx)


def on_load(ctx):
    # on_spawn already allocated fresh ids for this process; reuse them.
    _normalize(ctx)
    _draw_base(ctx)


def on_hit(ctx):
    _normalize(ctx)


def on_destroy(ctx):
    state = _normalize(ctx)
    state.pop(SNAP_ID_KEY, None)


def on_save(ctx):
    state = _normalize(ctx)
    # The C++ destructor returns the live ids; keep them out of the save.
    state.pop(SNAP_ID_KEY, None)


def on_tick(ctx):
    state = _normalize(ctx)
    if ctx.entity is None:
        return

    emit_ticks = max(1, int(state["emit_ticks"]))
    # Stagger neighbouring fires so they do not all emit on the same tick.
    if (int(ctx.now) + int(ctx.entity.id)) % emit_ticks != 0:
        return

    phase = int(state["phase"]) % SPREAD_PHASES
    t = float(phase) / float(SPREAD_PHASES - 1)
    for _ in range(PUFFS_PER_EMIT):
        _spawn_puff(ctx, state, t)
    state["phase"] = (phase + 1) % SPREAD_PHASES
