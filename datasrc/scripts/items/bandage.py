import lastday


def on_use(ctx):
    if ctx.player.heal(4) > 0:
        ctx.consumed = True
