"""Medkit: heals a large amount when used."""


HEAL_AMOUNT = 10


def on_use(ctx):
    return ctx.player.heal(HEAL_AMOUNT) > 0
