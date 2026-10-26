"""Herb tonic: heals and restores a little of the mercury meter."""


HEAL_AMOUNT = 3
FORGET_DELTA = -15


def on_use(ctx):
    healed = ctx.player.heal(HEAL_AMOUNT)
    forgot = ctx.player.forget(FORGET_DELTA)
    if healed > 0 or forgot != 0:
        ctx.consumed = True
