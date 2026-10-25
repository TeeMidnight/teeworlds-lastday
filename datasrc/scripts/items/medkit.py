import lastday


def on_use(ctx):
    if ctx.player.heal(10) > 0:
        ctx.consumed = True
