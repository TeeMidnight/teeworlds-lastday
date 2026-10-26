"""Sword: hammer-style swing rendered as the ninja weapon.

Mirrors CWeaponSword: hammer damage + 2, no resource harvesting.
"""


MELEE_RELOAD_DIVISOR = 3


def on_fire(ctx):
    extra = ctx.extra
    swing = ctx.MeleeSwing(
        ctx.player,
        ctx.pos,
        ctx.dir,
        extra.default_damage,  # hammer damage + 2
        ctx.range_scale,
        False,  # a sword cannot harvest
        extra.snap_style,  # ninja hit style
        True,  # show the melee hit spark
    )
    if swing.hits:
        ctx.reload_ticks = ctx.tick_speed // MELEE_RELOAD_DIVISOR
    return swing.hits
