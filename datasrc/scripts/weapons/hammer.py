"""Hammer: melee hit and resource harvesting.

The native MeleeSwing performs the same hit test as CWeaponMelee.
"""


WEAPON_HAMMER = 0
MELEE_RELOAD_DIVISOR = 3


def on_fire(ctx):
    extra = ctx.extra
    swing = ctx.MeleeSwing(
        ctx.player,
        ctx.pos,
        ctx.dir,
        extra.default_damage,
        ctx.range_scale,
        True,  # the hammer can harvest resources
        extra.snap_style,  # hit style (hammer)
        True,  # show the melee hit spark
    )
    if swing.hits:
        ctx.reload_ticks = ctx.tick_speed // MELEE_RELOAD_DIVISOR
    return swing.hits
