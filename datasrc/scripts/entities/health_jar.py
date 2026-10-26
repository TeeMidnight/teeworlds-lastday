# Health jar: a scripted prop built from the generic entity module.
#
# The C++ entity owns position, health and lifetime. This script draws the jar,
# keeps its cooldown and owner in state, and drops a health pickup when its
# owner stands close. Damage itself is applied by CScriptedEntity after on_hit,
# so on_hit only has to keep the state.
#
# Class API: ctx.entity (Entity), ctx.state/ctx.params (dicts), and the
# HealthDrop / DroppedPickup constructor wrappers.
import lastday


DEFAULT_COOLDOWN = 45
DEFAULT_RANGE = 96


def _normalize(ctx):
    """Fill the state keys a fresh or an old save may be missing."""
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
        # a freshly placed jar is ready immediately, so placing one next to
        # yourself pays off
        state["next_drop"] = int(ctx.now)
    if "tick_speed" not in state:
        state["tick_speed"] = int(ctx.tick_speed)
    return state


def _set_visual(ctx):
    if ctx.entity is None:
        return

    # The entity no longer reserves snapshot ids up front: the script asks for
    # exactly what it draws and hands the ids over in set_visual(). The entity
    # owns them from here on and releases them when the visual is replaced or
    # the jar is destroyed, so the script must not free them itself.
    laser_ids = [ctx.snap_new_id() for _ in range(6)]
    pickup_id = ctx.snap_new_id()

    # a jar: closed bottom, straight walls, sloped shoulders and a narrow lid.
    # The pickup in the middle is a snapshot primitive only; the real pickup is
    # the HealthDrop the jar spits out.
    ctx.entity.set_visual({
        "lasers": [
            {"id": laser_ids[0], "x1": -32, "y1": 48, "x2": 32, "y2": 48},
            {"id": laser_ids[1], "x1": -32, "y1": 48, "x2": -32, "y2": -24},
            {"id": laser_ids[2], "x1": 32, "y1": 48, "x2": 32, "y2": -24},
            {"id": laser_ids[3], "x1": -32, "y1": -24, "x2": -14, "y2": -48},
            {"id": laser_ids[4], "x1": 32, "y1": -24, "x2": 14, "y2": -48},
            {"id": laser_ids[5], "x1": -14, "y1": -48, "x2": 14, "y2": -48},
        ],
        "pickups": [
            {"id": pickup_id, "x": 0, "y": 0, "type": 0},
        ],
    })


def on_spawn(ctx):
    _normalize(ctx)
    _set_visual(ctx)


def on_load(ctx):
    # the restored state already carries cooldown/owner; only stale or partial
    # documents need defaults
    _normalize(ctx)
    _set_visual(ctx)


def on_tick(ctx):
    state = _normalize(ctx)
    if ctx.entity is None:
        return

    now = int(ctx.now)
    next_drop = int(state["next_drop"])
    if now < next_drop:
        return

    # ten owner checks per second are plenty for a seconds-long cooldown, and
    # the entity id staggers jars so two of them never scan clients on one tick
    if (now + ctx.entity.id) % 10 != 0:
        return

    if ctx.entity.owner_near() < 0:
        return

    pos = ctx.entity.pos
    jitter = int(lastday.random_int() % 17) - 8
    # HealthDrop.near refuses while an unclaimed drop is already nearby, so the
    # cooldown only restarts when a pickup actually appeared
    if ctx.HealthDrop.near(pos, 96.0):
        return
    drop = ctx.HealthDrop(ctx.Vec2(pos.x + jitter, pos.y), ctx.Vec2(0.0, 1.0))
    if drop.id >= 0:
        state["next_drop"] = now + int(state["cooldown"]) * int(ctx.tick_speed)


def on_hit(ctx):
    # CScriptedEntity applies the damage after this callback. Keep the state
    # object (and therefore the cooldown) across the hit.
    _normalize(ctx)


def on_destroy(ctx):
    state = _normalize(ctx)
    item = state["item"]
    if not item or ctx.entity is None:
        return

    pos = ctx.entity.pos
    # dismantle: the jar item falls out where it stood
    ctx.DroppedPickup(pos, ctx.Vec2(0.0, 0.0), item, 1)


def on_save(ctx):
    _normalize(ctx)
