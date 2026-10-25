import lastday


def on_apply(ctx):
    ctx.player.chat("Swiftness")


def on_tick(ctx):
    # Stat modifiers are applied by C++ from the JSON definition; the script
    # exists so a buff can add custom per-tick behaviour later.
    pass


def on_remove(ctx):
    ctx.player.chat("Swiftness fades")
