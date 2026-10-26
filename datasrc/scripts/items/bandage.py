"""Bandage: heals a small amount when used."""


HEAL_AMOUNT = 4


def on_use(ctx):
    if ctx.player.heal(HEAL_AMOUNT) > 0:
        ctx.consumed = True
