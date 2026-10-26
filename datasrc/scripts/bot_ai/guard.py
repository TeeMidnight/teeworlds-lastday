# Guard bot AI - example script for CScriptedBotAI.
#
# Select it with:
#   sv_bot_ai script:scripts/bot_ai/guard.py
# or a bare name, resolved to scripts/bot_ai/<name>.py:
#   sv_bot_ai guard
#
# The bot patrols until a player (or another hittable) comes within
# GUARD_RADIUS and is in line of sight, then walks in and swings. When the
# target leaves, it goes back to patrol.
#
# Callbacks receive the shared class-based context: ctx.entity, ctx.player,
# ctx.state, ctx.result, ctx.now, ctx.params, ... The embedded lastday_bot
# module takes entity ids, so the bot passes ctx.entity.id.
#
# The native strategy wrapped by this script is only a fallback for a script
# that fails to load or raises. Setting ctx.consumed (or result["handled"])
# keeps it from overwriting our input on a tick we drive ourselves.

import lastday_bot

GUARD_RADIUS = 260.0
MELEE_RANGE = 48.0
JUMP_HEIGHT = -48.0
PATROL_FLIP_TICKS = 100  # 2 seconds at the default 50 Hz
PATROL_AIM_DISTANCE = 100.0
FIRE_INTERVAL_TICKS = 2


def _engage(ctx, entity, target):
    """Walk toward the target and swing when it is within melee reach."""
    tx, ty = lastday_bot.get_pos(target)
    px, py = lastday_bot.get_pos(entity.id)
    dx = tx - px
    dy = ty - py

    direction = 0
    if dx > MELEE_RANGE:
        direction = 1
    elif dx < -MELEE_RANGE:
        direction = -1

    jump = dy < JUMP_HEIGHT
    # Fire in pulses: DoWeapon only swings on a fresh press, so holding the
    # button down across ticks would not keep attacking.
    in_range = dx * dx + dy * dy < MELEE_RANGE * MELEE_RANGE
    fire = in_range and ctx.now % FIRE_INTERVAL_TICKS == 0
    lastday_bot.set_input(entity.id, direction, jump, fire, dx, dy)
    ctx.state["mode"] = "engage"


def _patrol(ctx, entity):
    """Walk one way for a while, then turn around to avoid a wall."""
    # Keep one direction for a while, then turn around, so the bot does not
    # drift into a wall. The aim follows the walk direction.
    direction = ctx.state.get("direction", 1)
    if ctx.now % PATROL_FLIP_TICKS == 0:
        direction = -direction
        ctx.state["direction"] = direction
    lastday_bot.set_input(
        entity.id, direction, False, False, PATROL_AIM_DISTANCE * direction, 0.0
    )
    ctx.state["mode"] = "patrol"


def on_tick(ctx):
    # Observation hook: runs every tick before on_action. It does not consume
    # the tick, so on_action still decides this tick's input.
    ctx.state["ticks"] = ctx.state.get("ticks", 0) + 1


def on_action(ctx):
    if ctx.entity is None:
        return

    entity = ctx.entity
    target = lastday_bot.find_target(entity.id, GUARD_RADIUS)
    if target >= 0 and lastday_bot.has_line_of_sight(entity.id, target):
        _engage(ctx, entity, target)
    else:
        _patrol(ctx, entity)

    # Suppress the native fallback for this tick. ctx.consumed is the value
    # the shared runtime copies back to C++; result["handled"] documents the
    # same intent for callers that read the result object.
    ctx.consumed = True
    ctx.result["handled"] = True


def on_spawn(ctx):
    if ctx.entity is not None:
        x, y = lastday_bot.get_pos(ctx.entity.id)
        lastday_bot.log("guard: bot %d online at %s,%s" % (ctx.entity.id, x, y))


def on_damaged(ctx):
    # Optional hook: remember the attacker so a later tick could retaliate.
    ctx.state["last_attacker"] = ctx.params.get("source", [0.0, 0.0])


# A visible line in the server log when this AI module is (re)loaded.
lastday_bot.log("guard.py loaded")
