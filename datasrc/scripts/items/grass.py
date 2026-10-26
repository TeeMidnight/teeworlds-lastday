"""Grass: mildly restores health and clears a little forgetting."""


HEAL_AMOUNT = 2
FORGET_DELTA = -5


def on_use(ctx):
    healed = ctx.player.heal(HEAL_AMOUNT)
    forgot = ctx.player.forget(FORGET_DELTA)
    return healed > 0 or forgot != 0
