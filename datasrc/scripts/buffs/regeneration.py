"""Regeneration: heals one heart per second while the buff is active."""


HEAL_PER_SECOND = 1


def on_apply(ctx):
    ctx.player.chat("Regeneration")


def on_tick(ctx):
    # one heart per second, independent of the server's tick rate
    if ctx.now % ctx.tick_speed == 0:
        ctx.player.heal(HEAL_PER_SECOND)


def on_remove(ctx):
    ctx.player.chat("Regeneration fades")
