# Scripting (PocketPy)

The server embeds [PocketPy](https://github.com/blueloveTH/pocketpy) 2.2.0 as a
process-wide VM. Scripts run only on the server tick thread; clients never need
PocketPy. The script sources live in `datasrc/scripts/` and are copied to
`data/scripts/` by `ninja copy-data` (or a plain `ninja`).

## Layout

- `src/game/server/scripting/script_runtime.*` — VM lifetime, module loading,
  watchdog, exception isolation, JSON state/result transfer, and the active
  world of the running callback.
- `src/game/server/scripting/script_api.cpp` — embedded `lastday` module
  (players, items, buffs, projectiles, lasers, sounds, tuning, worlds and
  per-floor snapshot ids).
- `datasrc/scripts/_context.py` — class-based callback context. Every callback
  receives one `Context` object.
- `datasrc/scripts/*.pyi` — type hints for `lastday`, `lastday_entity`,
  `lastday_bot`, `lastday_weapon` and `_context`. They are copied to
  `data/scripts/` but never executed; PocketPy executes the `.py` files.
  Scripts can annotate handlers with `from _context import Context` under
  `typing.TYPE_CHECKING`.
- `src/game/server/scripting/buff_system.*` — buff definitions and per-tick
  buff instances.
- `src/game/server/scripting/scripted_weapon.*` + `weaponmanager.*` — weapon
  overrides.
- `src/game/server/scripting/scripted_entity.*` — generic scripted prop and the
  `lastday_entity` module (health jar is the first migrated prop).
- `src/game/server/scripting/scripted_bot_ai.*` — bot AI strategies and the
  `lastday_bot` module.
- `src/game/server/scripting/script_commands.cpp` — admin commands.

## Callback context

```python
def on_use(ctx):
    return ctx.player.heal(4) > 0   # True consumes one copy

def on_tick(ctx):
    if ctx.now % ctx.tick_speed == 0:
        ctx.player.heal(1)
```

Useful fields:

- `ctx.player` — player wrapper (`.id`, `.pos`, `.velocity`, `.grounded`,
  `.aim_direction`, `.health`, `.alive`, `.team`, `.name`, `.weapon`,
  `.heal()`, `.damage()`, `.chat()`, `.teleport()`, `.item_count()`,
  `.give_item()`, `.add_buff()`, `.forget()`, ...) or `None`.
- `ctx.entity` — scripted entity wrapper (`.id`, `.pos`, `.health`, `.exists`,
  `.world_id`, `.damage()`, `.destroy()`, `.set_visual()`, ...) or `None`.
- `ctx.item` — item wrapper (`.id`, `.count(player)`, `.give()`, `.take()`).
- `ctx.buff` — active buff wrapper (`.id`, `.stacks`, `.state`, `.remove()`).
- `ctx.pos`, `ctx.dir` — `Vec2` with `.x`, `.y`, indexing, `.length()`,
  `.normalized()`, basic arithmetic.
- `ctx.ammo` — `Ammo` (`.damage`, `.projectiles`, `.speed`, `.lifetime`, `.effect`).
- `ctx.extra` — `Extra` for subsystem-specific values (missing keys read as 0).
- `ctx.params`, `ctx.state`, `ctx.result` — plain dictionaries.
- `ctx.now`, `ctx.tick_speed`, `ctx.range_scale`, `ctx.weapon`, `ctx.kind`.
- `ctx.world`, `ctx.world_name`, `ctx.floor_name`, `ctx.world_kind` — the map
  and floor the callback runs in. Generated instances share a floor name
  (`Depths_1` and `Depths_23` are both floor `Depths`).
- `ctx.snap_new_id()` / `ctx.snap_free_id(id)` — snapshot ids for custom
  snapshot items. Ids are pooled per floor: all instances of one floor share
  a pool, while an unrelated floor has its own, so ring neighbours can be
  snapped together without collisions. The context passes its own world when
  freeing, because the same numeric id may be live on two different floors.
- Output flags: `ctx.reload_ticks`, `ctx.destroy`; `ctx.consumed` is still
  honored by the bot fallback and older item scripts.
- Constructor classes: `ctx.Projectile`, `ctx.Laser`, `ctx.HealthDrop`,
  `ctx.DroppedPickup`, `ctx.MeleeSwing`. Their constructors call the existing
  C++ `new CProjectile(...)` / `new CLaser(...)` / etc. The returned object
  only carries the entity id; the game world owns and deletes the C++ entity.

```python
def on_fire(ctx):
    span = int(lastday.tuning("gun_lifetime") * ctx.ammo.lifetime * ctx.tick_speed)
    start = ctx.pos + ctx.dir * ctx.extra.proximity_radius * 0.75
    ctx.Projectile(ctx.player, start, ctx.dir * ctx.ammo.speed, span,
                   ctx.ammo.damage or ctx.extra.default_damage,
                   False, -1, 1)
```

`ctx["player"]` etc. still work as a compatibility shim for older scripts and
return the primitive id/name; new scripts should use the class attributes.

## Host API

`import lastday` exposes the server bridge:

- players: `player_pos`, `player_health`, `player_armor_value`,
  `player_alive`, `player_max_health`, `player_range_scale`,
  `player_speed_scale`, `player_team`, `player_name`, `player_velocity`,
  `player_heal`, `player_armor`, `player_damage`, `player_forget`,
  `player_teleport`, `player_weapon`, `player_weapon_id`,
  `player_grounded`, `player_aim_direction`, `player_set_weapon`, `chat`,
  `chat_localized`.
- items / buffs: `item_count`, `item_give`, `item_take`, `item_drill`,
  `item_health_jar`, `buff_add`, `buff_remove`, `buff_stacks`.
- entities: `create_projectile`, `create_laser`, `sound`, `hammer_hit`,
  `tuning`, `random_int`, `random_int_range`, `log`.
- server / worlds: `tick`, `tick_speed`, `active_world`, `world_name`,
  `floor_name`, `world_kind`, `world_test_box`, `world_floor_y`, `drill_near`,
  `drill_spawn`.
- snapshot ids: `snap_new_id()`, `snap_new_id_for(world_id)`,
  `snap_free_id(id)`, `snap_free_id_for(id, world_id)`. The `_for` variants
  name the world explicitly; the plain ones use the callback's world. Freeing
  needs the world because different floors may hold the same numeric id.

`import lastday_entity` adds the scripted-prop surface (`spawn`, `exists`,
`pos`, `health`, `damage`, `destroy`, `set_visual`, `set_health`,
`set_persistent`, `owner_near`, `player_near`, `world_id`, health/items drops)
and its own `snap_new_id` / `snap_free_id` for code that only imported the
entity module.

## Data formats

Item (`datasrc/items/<category>/<id>.json`):

```json
{
  "name": "Bandage",
  "item_type": ["medicine"],
  "script": "scripts/items/bandage.py"
}
```

The item script may implement `on_use`; its return value decides whether C++
removes one copy. Return `True` when the effect actually applied, `False` when
nothing changed (full health, no forgetting to restore, ...). For compatibility
`ctx.consumed = True` is still accepted as a fallback.

A building item implements `on_place` instead. The script owns the placement
checks and spawns the world object; returning `True` consumes one item:

```python
def on_place(ctx):
    if not ctx.player.grounded:
        ctx.player.chat("Stand on the ground to place a drill.", "Drill")
        return False
    has_drill, max_length, power = lastday.item_drill(ctx.item.id)
    ...
    return lastday.drill_spawn(ctx.item.id, pos.x, pos.y, max_length, power, ctx.player.id) >= 0
```

Placement primitives: `lastday.item_drill`, `lastday.item_health_jar`,
`lastday.player_grounded`, `lastday.player_aim_direction`,
`lastday.world_test_box`, `lastday.world_floor_y`, `lastday.drill_near`,
`lastday.drill_spawn`, and on `lastday_entity`: `health_jar_size`,
`health_jar_near`, `spawn_health_jar`.

Player-visible script text should use the localized form of `chat`:

```python
def on_apply(ctx):
    ctx.player.chat("Regeneration", "Buff")
```

The second argument is the localization context. `scripts/convert_l10n.py`
extracts these `chat("text", "context")` calls from `datasrc/scripts/**/*.py`
alongside the C++ `Localize(...)` calls, so language files contain the same keys
as the runtime lookup. Keep the literals in double quotes so extraction stays
simple.

Buff (`datasrc/buffs/<id>.json`):

```json
{
  "id": "swiftness",
  "duration": 15,
  "max_stacks": 3,
  "stack_duration": true,
  "script": "scripts/buffs/swiftness.py",
  "modifiers": { "health": 0, "range": 0.0, "speed": 0.05 }
}
```

Buffs call `on_apply`, `on_tick` (every game tick) and `on_remove`. C++ owns
duration, stacking and the modifier sums.

Weapon (`datasrc/weapons/<name>.json`):

```json
{
  "name": "gun",
  "script": "scripts/weapons/gun.py",
  "snap_style": 1,
  "fire_delay": 125,
  "full_auto": false,
  "default_ammo": 10,
  "max_ammo": 10
}
```

A scripted weapon overrides the native class of the same name and implements
`on_fire`; `on_tick` and `on_switch` are optional. The native class remains
as fallback if the script is missing or broken.

Entity (`datasrc/entities/<kind>.json`):

```json
{
  "kind": "health_jar",
  "script": "scripts/entities/health_jar.py",
  "health": 10,
  "radius": 32,
  "persistent": true
}
```

Entity callbacks: `on_spawn`, `on_load`, `on_tick`, `on_hit`, `on_destroy`,
`on_save`. Use `lastday_entity` for spawn/destroy/visuals/drops/owner checks.

A scripted entity does **not** reserve snapshot ids itself. The script asks for
as many as it draws **once per entity lifetime** and reuses them for every
visual update:

```python
_SNAP_IDS = {}

def _snap_ids(ctx):
    ids = _SNAP_IDS.get(ctx.entity.id)
    if ids is None:
        ids = [ctx.snap_new_id() for _ in range(2)]
        _SNAP_IDS[ctx.entity.id] = ids
    return ids

def on_spawn(ctx):
    _SNAP_IDS.pop(ctx.entity.id, None)  # a recycled entity id starts clean
    _set_visual(ctx)

def _set_visual(ctx):
    ids = _snap_ids(ctx)
    ctx.entity.set_visual({
        "lasers": [
            {"id": ids[0], "x1": -16, "y1": 0, "x2": 16, "y2": 0},
            {"id": ids[1], "x1": 0, "y1": -16, "x2": 0, "y2": 16},
        ],
    })
```

`set_visual()` replaces the whole geometry. `update_visual()` is the
incremental form: primitives are matched by `id`, only the fields present in
the update change, and primitives that are not mentioned keep their previous
geometry:

```python
ctx.entity.update_visual({
    "lasers": [
        {"id": ids[0], "x2": 32.0},   # y1/y2/x1 stay as they were
    ],
})
```

Both calls are explicit. If the script calls neither, the last visual stays on
screen; there is no automatic per-tick refresh. Neither call allocates or
releases ids. The entity keeps the union of the ids it has been given and
releases all of them when it is destroyed. `ctx.entity.release_visual()` drops
the current visual and returns the ids early; allocate fresh ones before setting
a visual again after that. Scripts must not free ids that they passed in. A
primitive with `id < 0` (for example because the pool is exhausted) is not
drawn.

Bot AI: `sv_bot_ai` selects `chase` (default), `wander`, `stand`, a bare
example name (`guard`), or `script:scripts/bot_ai/<name>.py`. Scripts implement
`on_spawn`, `on_tick`, `on_action`, `on_damaged`, `on_hit` and drive the
body through `lastday_bot.set_input(...)`. Set `ctx.consumed = True` (or
`ctx.result["handled"] = True`) to suppress the native fallback for that tick.

## Admin commands

No `sv_` / `cl_` prefix:

- `reload_scripts` — re-exec item/buff/weapon/entity scripts in place.
- `buff <client> <buff_id> [seconds] [stacks]`
- `give_item <client> <item_id> [count]`
- `script_selftest` — run `scripts/selftest.py` through the class context.

(Settings keep the engine's normal `sv_` convention, e.g. `sv_bot_ai`.)

## Migration status

Scripted now: medicine item use, weapon firing for gun/shotgun/grenade/laser/
hammer/hand/sword, buffs, health jar prop, and bot AI. Health drops are merged
into `CDroppedPickup` as `KIND_HEALTH` (one physics/collection path, not a
separate entity type). The ninja weapon, player character, projectile/laser
simulation, resource/pickup/drill entities remain native (hot paths or still
stateful). New prop kinds can use the generic scripted entity; migrate them one
at a time while keeping the native fallback.
