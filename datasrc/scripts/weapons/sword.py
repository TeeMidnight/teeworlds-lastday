"""Sword: hammer-style swing rendered as the ninja weapon.

Hammer damage + 2, no resource harvesting and a longer reach than the hammer.
"""


MELEE_RELOAD_DIVISOR = 3
# the sword reaches 35% further than the hammer
SWORD_REACH_SCALE = 1.35


def on_fire(ctx):
    extra = ctx.extra
    swing = ctx.MeleeSwing(
        ctx.player,
        ctx.pos,
        ctx.dir,
        extra.default_damage,  # hammer damage + 2
        ctx.range_scale * SWORD_REACH_SCALE,  # longer than the hammer
        False,  # a sword cannot harvest
        extra.snap_style,  # ninja hit style
        True,  # show the melee hit spark
    )
    if swing.hits:
        ctx.reload_ticks = ctx.tick_speed // MELEE_RELOAD_DIVISOR
    return swing.hits
