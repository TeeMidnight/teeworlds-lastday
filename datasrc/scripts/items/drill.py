"""Placement behaviour shared by every drill item.

The item definition still owns the drill stats; this script owns the placement
checks and the entity spawn. C++ consumes one item when on_place returns True.
"""

import lastday


DUPLICATE_RADIUS = 40.0


def on_place(ctx):
    if ctx.player is None or ctx.item is None:
        return False
    if not ctx.player.alive:
        ctx.player.chat("You have to be alive to place a drill.", "Drill")
        return False
    if not ctx.player.grounded:
        ctx.player.chat("Stand on the ground to place a drill.", "Drill")
        return False

    has_drill, max_length, power = lastday.item_drill(ctx.item.id)
    if not has_drill:
        return False

    pos = ctx.player.pos
    if lastday.drill_near(pos.x, pos.y, DUPLICATE_RADIUS):
        ctx.player.chat("There is already a drill here.", "Drill")
        return False

    if lastday.drill_spawn(ctx.item.id, pos.x, pos.y, max_length, power, ctx.player.id) < 0:
        ctx.player.chat("The drill could not be placed.", "Drill")
        return False

    ctx.player.chat(
        "Drill placed. Hold the scoreboard next to it to take control; jump to let go.",
        "Drill",
    )
    return True
