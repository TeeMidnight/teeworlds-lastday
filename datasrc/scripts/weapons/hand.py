"""Hand: the unarmed fallback punch. Mirrors CWeaponHand."""


MELEE_RELOAD_DIVISOR = 3


def on_fire(ctx):
    extra = ctx.extra
    swing = ctx.MeleeSwing(
        ctx.player,
        ctx.pos,
        ctx.dir,
        extra.default_damage,  # always 1
        ctx.range_scale,
        False,  # fists cannot harvest
        extra.snap_style,  # -1: the helper credits the hammer
        False,  # no hit spark
    )
    if swing.hits:
        ctx.reload_ticks = ctx.tick_speed // MELEE_RELOAD_DIVISOR
    return swing.hits
