"""Bandage: heals a small amount when used."""


HEAL_AMOUNT = 4


def on_use(ctx):
    return ctx.player.heal(HEAL_AMOUNT) > 0
