# Sword: hammer-style swing that renders as the ninja weapon and cuts harder.
# Mirrors CWeaponSword (hammer damage + 2, no resource harvesting).
def on_fire(ctx):
    extra = ctx.extra
    swing = ctx.MeleeSwing(
        ctx.player, ctx.pos, ctx.dir,
        extra.default_damage,   # hammer damage + 2
        ctx.range_scale,
        False,                  # a sword cannot harvest
        extra.snap_style,       # ninja hit style
        True)                   # show the melee hit spark
    if swing.hits:
        ctx.reload_ticks = ctx.tick_speed // 3
    return swing.hits


def on_tick(ctx):
    pass


def on_switch(ctx):
    pass
