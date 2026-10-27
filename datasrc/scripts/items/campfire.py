"""Placement behaviour for the campfire prop.

The prop itself (drawing, ember projectiles, persistence) lives in
scripts/entities/campfire.py. C++ consumes one item when on_place returns True.
"""

import math

import lastday
import lastday_entity


FORWARD_OFFSET = 56.0
TILE = 32.0
FLOOR_SEARCH = 4.0 * TILE
WIDTH = 56.0
HEIGHT = 48.0
DUPLICATE_RADIUS = 64.0

ENTITY_KIND = "campfire"
LOCK_SELF = 1


def _campfire_center(ctx):
    """Return the placed campfire centre, or None when there is no floor."""
    pos = ctx.player.pos
    aim = ctx.player.aim_direction
    face = 1.0 if aim.x >= 0.0 else -1.0

    # floor(x + 0.5) matches the C++ round used by the other placeables
    # (Python round() is banker's rounding and differs at .5).
    center_x = math.floor((pos.x + face * FORWARD_OFFSET) / TILE + 0.5) * TILE
    found, floor_y = lastday.world_floor_y(center_x, pos.y, FLOOR_SEARCH)
    if not found:
        return None
    return center_x, floor_y - HEIGHT / 2.0


def on_place(ctx):
    if ctx.player is None or ctx.item is None:
        return False
    if not ctx.player.alive:
        ctx.player.chat("You have to be alive to place a campfire.", "Campfire")
        return False
    if not ctx.player.grounded:
        ctx.player.chat("Stand on the ground to place a campfire.", "Campfire")
        return False

    center = _campfire_center(ctx)
    if center is None:
        ctx.player.chat("There is no floor below the campfire.", "Campfire")
        return False
    center_x, center_y = center

    if lastday.world_test_box(center_x, center_y, WIDTH - 8.0, HEIGHT - 10.0):
        ctx.player.chat("There is no room for a campfire here.", "Campfire")
        return False
    if lastday_entity.kind_near(ENTITY_KIND, center_x, center_y, DUPLICATE_RADIUS):
        ctx.player.chat("There is already a campfire here.", "Campfire")
        return False

    params = {
        "range": 96,
        "emit_ticks": 5,
        "drop_item": ctx.item.id,
        "drop_count": 1,
    }
    campfire_id = lastday_entity.spawn_owned(
        ENTITY_KIND,
        center_x,
        center_y,
        params,
        ctx.player.id,
        1,  # persistent: written into the world save
        0,  # max health 0 = use the entity definition
        LOCK_SELF,
    )
    if campfire_id < 0:
        ctx.player.chat("Campfires are unavailable.", "Campfire")
        return False

    ctx.player.chat("Campfire placed. It burns nearby monsters.", "Campfire")
    return True
