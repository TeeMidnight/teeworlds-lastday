# Campfire: a persistent fire prop built from the generic scripted entity.
#
# The base is drawn with two crossed snapshot lasers (the logs). The flames are
# intentionally made of CFireParticle entities that snap as WEAPON_HAMMER
# projectiles: the stock client draws no projectile sprite for that type but
# still emits its bullet trail particle for every live projectile, so a stream
# of short-lived stationary puffs reads as embers. The puffs are spawned with
# zero velocity and zero gravity: the phase wave below, not physics, shapes the
# flame, and a stationary puff uses CFireParticle's point hit test. They are
# spawned with PLAYER_TEAM_FRIENDLY as the owner, which is the engine sentinel
# that hurts
# hostile bots but never players, friendly units or neutral units: a base
# campfire should not cook its own owner. CFireParticle owns the point hit
# test, so a stationary puff can actually apply its damage.
#
# The puffs are emitted as a rising wave: each emission advances one phase from
# the base outward, so the flames visibly grow from the bottom of the fire. The
# wave is a single central cone, not two side jets; each puff names the
# campfire itself as its ignore target so the flame can pass through the prop's
# own hitbox without being absorbed at the base.

import math

import lastday


WEAPON_HAMMER = 0
PLAYER_TEAM_FRIENDLY = -4

DEFAULT_EMIT_TICKS = 3
DEFAULT_RANGE = 96
DEFAULT_DAMAGE = 1
PUFFS_PER_EMIT = 7

# Bottom of the entity in world units; the floor is 24 units below the centre.
# 16 keeps the swept line check clear of the floor (radius 3 -> 5 units margin).
BASE_Y = 16.0
LINE_RADIUS = 3.0
SNAP_ID_KEY = "_snap_ids"
BASE_SEGMENT_COUNT = 2

# Rising wave. The flame starts wide at the base (phase 0) and tapers to the
# tip as the phase advances to SPREAD_PHASES - 1. The taper is curved and the
# whole flame is slightly asymmetric: every emission picks its own reach, lean
# and mid-height bend, so the tip wanders instead of ending at one fixed point.
SPREAD_PHASES = 5
FLAME_RISE = 86.0
FLAME_HEIGHT_MIN = 0.80
FLAME_HEIGHT_MAX = 1.10
# The flame bottom spans the top ends of the crossed logs: x = +-30 at y = 10.
FLAME_BASE_WIDTH = 30.0
FLAME_TIP_WIDTH = 3.0
FLAME_BULGE = 0.15
FLAME_LEAN = 11.0
FLAME_CURVE = 7.0

PUFF_SPAN_MIN = 12
PUFF_SPAN_MAX = 18


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
    # Old campfires were saved with the sparse cadence. Cap it so an existing
    # fire becomes dense after the script updates instead of staying thin.
    state["emit_ticks"] = min(max(1, int(state["emit_ticks"])), DEFAULT_EMIT_TICKS)
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
    """Draw the two crossed logs with snapshot lasers.

    The top ends sit at (-30, 10) and (30, 10), which is exactly the height and
    width the flame starts from, so the fire stands on the logs instead of
    floating over a wider base."""
    if ctx.entity is None:
        return

    ids = _snap_ids(ctx)
    ctx.entity.set_visual({
        "lasers": [
            _laser(ids[0], -30, 10, 30, 24),
            _laser(ids[1], -30, 24, 30, 10),
        ],
    })


def _spawn_puff(ctx, state, t, wave):
    pos = ctx.entity.pos
    base_x = pos.x
    base_y = pos.y + BASE_Y

    # Slight jitter around the phase so the wave fills the volume instead of
    # looking like a few discrete rings.
    t = t + (_random01() - 0.5) * 0.22
    t = max(0.0, min(1.0, t))

    rise = 6.0 + (wave["height"] - 6.0) * t
    # A real flame is widest near the base, has a soft bulge above it and
    # narrows to a tip. The exponent curves the taper; the sine adds the bulge.
    profile = (1.0 - t) ** 0.65
    width = FLAME_TIP_WIDTH + (FLAME_BASE_WIDTH - FLAME_TIP_WIDTH) * profile
    width *= 1.0 + FLAME_BULGE * math.sin(math.pi * t)

    # The centre line bends: a quadratic lean moves the tip, a sine bend makes
    # the body curve. Both change with every emission, so the top is not fixed.
    centre = wave["lean"] * t * t
    centre += wave["curve"] * math.sin(math.pi * t) * (1.0 - t)

    lateral = centre + width * (_random01() * 2.0 - 1.0)
    x = base_x + lateral
    y = base_y - rise - _random01() * 4.0

    # Do not burn through a wall next to the fire.
    if lastday.world_test_line(base_x, base_y, x, y, LINE_RADIUS):
        return

    span = PUFF_SPAN_MIN + (lastday.random_int() % (PUFF_SPAN_MAX - PUFF_SPAN_MIN + 1))
    # A dedicated point entity: it snaps like the stationary hammer projectile
    # for the ember trail, but does its own point hit test so the flame can
    # damage. The campfire's wave already animates the flame, so these puffs
    # stay still: zero velocity and zero gravity keep that silhouette.
    lastday.create_fire_particle(
        PLAYER_TEAM_FRIENDLY,
        x,
        y,
        0.0,
        0.0,
        span,
        max(1, int(state["damage"])),
        WEAPON_HAMMER,
        int(ctx.entity.id),
        0.0,
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

    # Each emission has its own silhouette: a different reach, a different lean
    # and a different mid-height bend. The living puffs from earlier emissions
    # fade underneath, so the flame tip wanders instead of standing still.
    wave = {
        "height": FLAME_RISE * (
            FLAME_HEIGHT_MIN + (FLAME_HEIGHT_MAX - FLAME_HEIGHT_MIN) * _random01()
        ),
        "lean": (_random01() * 2.0 - 1.0) * FLAME_LEAN,
        "curve": (_random01() * 2.0 - 1.0) * FLAME_CURVE,
    }
    for _ in range(PUFFS_PER_EMIT):
        _spawn_puff(ctx, state, t, wave)
    state["phase"] = (phase + 1) % SPREAD_PHASES
