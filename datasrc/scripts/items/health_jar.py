"""Placement behaviour for the health jar item.

The jar entity itself (drawing, cooldown, drops) lives in
scripts/entities/health_jar.py; this script only decides where it may be placed
and spawns it. C++ consumes one item when on_place returns True.
"""

import math

import lastday
import lastday_entity


FORWARD_OFFSET = 48.0
TILE = 32.0
FLOOR_SEARCH = 4.0 * TILE


def _jar_center(ctx, height):
    """Return the jar's x and y centre, or None when there is no floor."""
    pos = ctx.player.pos
    aim = ctx.player.aim_direction
    face = 1.0 if aim.x >= 0.0 else -1.0

    # floor(x + 0.5) matches the C++ roundf the old placement code used
    # (Python round() is banker's rounding and would differ at .5)
    center_x = math.floor((pos.x + face * FORWARD_OFFSET) / TILE + 0.5) * TILE
    found, floor_y = lastday.world_floor_y(center_x, pos.y, FLOOR_SEARCH)
    if not found:
        return None
    return center_x, floor_y - height / 2.0


def on_place(ctx):
    if ctx.player is None or ctx.item is None:
        return False
    if not ctx.player.alive:
        ctx.player.chat("You have to be alive to place a health jar.", "Health Jar")
        return False
    if not ctx.player.grounded:
        ctx.player.chat("Stand on the ground to place a health jar.", "Health Jar")
        return False

    has_jar, cooldown, jar_range, health = lastday.item_health_jar(ctx.item.id)
    if not has_jar:
        return False

    width, height = lastday_entity.health_jar_size()
    center = _jar_center(ctx, height)
    if center is None:
        ctx.player.chat("There is no floor below the jar.", "Health Jar")
        return False
    center_x, center_y = center

    # leave the bottom edge resting on the floor instead of inside it
    if lastday.world_test_box(center_x, center_y, width - 4.0, height - 6.0):
        ctx.player.chat("There is no room for a health jar here.", "Health Jar")
        return False
    if lastday_entity.health_jar_near(center_x, center_y, width):
        ctx.player.chat("There is already a health jar here.", "Health Jar")
        return False

    jar_id = lastday_entity.spawn_health_jar(
        ctx.item.id, center_x, center_y, cooldown, jar_range, health, ctx.player.id
    )
    if jar_id < 0:
        ctx.player.chat("Health jars are unavailable.", "Health Jar")
        return False

    ctx.player.chat(
        "Health jar placed. Its owner coming close makes it drop a health pickup.",
        "Health Jar",
    )
    return True
