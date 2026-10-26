"""Swiftness: the speed modifier is applied by C++ from the JSON definition.

The script only owns the flavour messages for now; on_tick is intentionally
absent so the runtime does not call an empty handler every tick.
"""


def on_apply(ctx):
    ctx.player.chat("Swiftness")


def on_remove(ctx):
    ctx.player.chat("Swiftness fades")
