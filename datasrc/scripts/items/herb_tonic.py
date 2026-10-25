import lastday


def on_use(ctx):
    healed = ctx.player.heal(3)
    forgot = ctx.player.forget(-15)
    if healed > 0 or forgot != 0:
        ctx.consumed = True
