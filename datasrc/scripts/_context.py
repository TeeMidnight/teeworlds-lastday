"""Runtime callback context for PocketPy scripts.

C++ calls make_context(raw) with a plain dict and turns it into the wrapper
classes below. After the callback the same object supplies state, result and
the consumed/reload/destroy flags back to C++. Type hints for editors live in
_context.pyi; keep the two files in sync when the API changes.
"""

import lastday


class Vec2:
    """A 2D vector; supports indexing and basic arithmetic."""

    def __init__(self, x=0.0, y=0.0):
        self.x = x
        self.y = y

    def __getitem__(self, index):
        return self.x if index == 0 else self.y

    def __iter__(self):
        return iter((self.x, self.y))

    def __len__(self):
        return 2

    def __repr__(self):
        return "Vec2(%s, %s)" % (self.x, self.y)

    def __add__(self, other):
        return Vec2(self.x + other.x, self.y + other.y)

    def __sub__(self, other):
        return Vec2(self.x - other.x, self.y - other.y)

    def __mul__(self, scale):
        return Vec2(self.x * scale, self.y * scale)

    def __truediv__(self, scale):
        return Vec2(self.x / scale, self.y / scale)

    def length(self):
        return (self.x * self.x + self.y * self.y) ** 0.5

    def normalized(self):
        n = self.length()
        if n == 0:
            return Vec2(0.0, 0.0)
        return Vec2(self.x / n, self.y / n)


class Player:
    """A connected player addressed by client id."""

    def __init__(self, client_id):
        self.id = client_id

    @property
    def pos(self):
        x, y = lastday.player_pos(self.id)
        return Vec2(x, y)

    @property
    def health(self):
        return lastday.player_health(self.id)

    @property
    def armor(self):
        return lastday.player_armor_value(self.id)

    @property
    def max_health(self):
        return lastday.player_max_health(self.id)

    @property
    def alive(self):
        return lastday.player_alive(self.id)

    @property
    def range_scale(self):
        return lastday.player_range_scale(self.id)

    @property
    def speed_scale(self):
        return lastday.player_speed_scale(self.id)

    @property
    def team(self):
        return lastday.player_team(self.id)

    @property
    def name(self):
        return lastday.player_name(self.id)

    @property
    def velocity(self):
        x, y = lastday.player_velocity(self.id)
        return Vec2(x, y)

    @property
    def grounded(self):
        return lastday.player_grounded(self.id)

    @property
    def aim_direction(self):
        x, y = lastday.player_aim_direction(self.id)
        return Vec2(x, y)

    @property
    def weapon(self):
        return lastday.player_weapon(self.id)

    @property
    def weapon_id(self):
        return lastday.player_weapon_id(self.id)

    def teleport(self, x, y):
        return lastday.player_teleport(self.id, float(x), float(y))

    def set_weapon(self, slot):
        return lastday.player_set_weapon(self.id, int(slot))

    def heal(self, amount):
        return lastday.player_heal(self.id, amount)

    def add_armor(self, amount):
        return lastday.player_armor(self.id, amount)

    def damage(self, amount, from_client=-1, weapon=-1):
        return lastday.player_damage(self.id, amount, from_client, weapon)

    def forget(self, delta):
        return lastday.player_forget(self.id, delta)

    def chat(self, text, context=""):
        if context:
            return lastday.chat_localized(self.id, str(text), str(context))
        return lastday.chat(self.id, str(text))

    def item_count(self, item_id):
        return lastday.item_count(self.id, str(item_id))

    def give_item(self, item_id, count=1):
        return lastday.item_give(self.id, str(item_id), count)

    def take_item(self, item_id, count=1):
        return lastday.item_take(self.id, str(item_id), count)

    def add_buff(self, buff_id, duration_ticks=-1, stacks=1, source=-1):
        return lastday.buff_add(self.id, str(buff_id), duration_ticks, stacks, source)

    def remove_buff(self, buff_id):
        return lastday.buff_remove(self.id, str(buff_id))

    def buff_stacks(self, buff_id):
        return lastday.buff_stacks(self.id, str(buff_id))

    def __int__(self):
        return self.id

    def __repr__(self):
        return "Player(%d)" % self.id


class Item:
    """A content item id, bound to a player only by its methods."""

    def __init__(self, item_id):
        self.id = item_id

    def count(self, player):
        return lastday.item_count(int(player), self.id)

    def give(self, player, count=1):
        return lastday.item_give(int(player), self.id, count)

    def take(self, player, count=1):
        return lastday.item_take(int(player), self.id, count)

    def __str__(self):
        return self.id

    def __repr__(self):
        return "Item(%s)" % self.id


class Entity:
    """A live scripted entity addressed by its entity id."""

    def __init__(self, entity_id):
        self.id = entity_id

    def _api(self):
        # The entity surface lives in lastday_entity; the old no-op shims that
        # were mirrored onto lastday are gone.
        import lastday_entity
        return lastday_entity

    @property
    def pos(self):
        x, y = self._api().pos(self.id)
        return Vec2(x, y)

    @property
    def health(self):
        return self._api().health(self.id)

    @property
    def exists(self):
        return self._api().exists(self.id)

    @property
    def world_id(self):
        return self._api().world_id(self.id)

    def damage(self, amount, from_client=-1, weapon=-1):
        return self._api().damage(self.id, amount, from_client, weapon)

    def destroy(self):
        return self._api().destroy(self.id)

    def set_visual(self, visual):
        return self._api().set_visual(self.id, visual)

    def update_visual(self, visual):
        return self._api().update_visual(self.id, visual)

    def release_visual(self):
        return self._api().release_visual(self.id)

    def set_health(self, health):
        return self._api().set_health(self.id, health)

    def set_persistent(self, persistent):
        return self._api().set_persistent(self.id, persistent)

    def owner_near(self):
        return self._api().owner_near(self.id)

    def player_near(self, radius=96.0):
        return self._api().player_near(self.id, float(radius))

    def __int__(self):
        return self.id

    def __repr__(self):
        return "Entity(%d)" % self.id


class Buff:
    """One active buff instance on a player."""

    def __init__(self, buff_id, player, state):
        self.id = buff_id
        self.player = player
        self.state = state

    @property
    def stacks(self):
        if self.player is None:
            return 0
        return lastday.buff_stacks(self.player.id, self.id)

    def remove(self):
        if self.player is None:
            return False
        return lastday.buff_remove(self.player.id, self.id)

    def __str__(self):
        return self.id

    def __repr__(self):
        return "Buff(%s)" % self.id


class Ammo:
    """Ammo values supplied by the weapon that is firing."""

    def __init__(self, data):
        self.damage = data["damage"]
        self.projectiles = data["projectiles"]
        self.speed = data["speed"]
        self.lifetime = data["lifetime"]
        self.effect = data["effect"]

    def __getitem__(self, key):
        return getattr(self, key)

    def __repr__(self):
        return "Ammo(damage=%s, projectiles=%s)" % (self.damage, self.projectiles)


class Extra:
    """Subsystem-specific values; a missing key reads as 0."""

    def __init__(self, data):
        self._data = data

    def __getattr__(self, key):
        if key in self._data:
            return self._data[key]
        return 0

    def __getitem__(self, key):
        return self.__getattr__(key)

    def __contains__(self, key):
        return key in self._data


def _as_id(value):
    if value is None:
        return -1
    if hasattr(value, "id"):
        return value.id
    return int(value)


def _as_vec(value):
    if hasattr(value, "x"):
        return value
    return Vec2(value[0], value[1])


def _entity_api():
    import lastday_entity
    return lastday_entity


class Projectile:
    """A C++ CProjectile created through its constructor.

    The handle is only an id: the game world owns the entity and removes it on
    impact/lifetime. The Python object never deletes it.
    """

    def __init__(self, owner, pos, direction, span, damage, explosive=False, sound_impact=-1, weapon=-1):
        p = _as_vec(pos)
        d = _as_vec(direction)
        self.id = lastday.create_projectile(
            _as_id(owner), p.x, p.y, d.x, d.y,
            int(span), int(damage), bool(explosive), int(sound_impact), int(weapon))

    def __repr__(self):
        return "Projectile(%d)" % self.id


class FireParticle:
    """A fire puff with velocity, gravity and its own hit test.

    The C++ entity snaps as a hammer projectile so the stock client still
    draws the ember trail. It deals its damage once; when it lands it keeps
    burning in place for a short fade before it disappears.
    """

    def __init__(self, owner, pos, velocity, span, damage, weapon=-1, ignore=None, gravity=0.0):
        p = _as_vec(pos)
        v = _as_vec(velocity)
        self.id = lastday.create_fire_particle(
            _as_id(owner), p.x, p.y, v.x, v.y,
            int(span), int(damage), int(weapon), _as_id(ignore), float(gravity))

    def __repr__(self):
        return "FireParticle(%d)" % self.id


class Laser:
    """A C++ CLaser created through its constructor."""

    def __init__(self, owner, pos, direction, reach, damage):
        p = _as_vec(pos)
        d = _as_vec(direction)
        self.id = lastday.create_laser(_as_id(owner), p.x, p.y, d.x, d.y, float(reach), int(damage))

    def __repr__(self):
        return "Laser(%d)" % self.id


class HealthDrop:
    """A C++ CHealthDropEntity created through its constructor."""

    def __init__(self, pos, velocity=None):
        p = _as_vec(pos)
        v = _as_vec(velocity) if velocity is not None else Vec2(0.0, 1.0)
        self.id = _entity_api().create_health_drop(p.x, p.y, v.x, v.y)

    @staticmethod
    def near(pos, radius=96.0):
        p = _as_vec(pos)
        return _entity_api().health_drop_near(p.x, p.y, float(radius))

    def __repr__(self):
        return "HealthDrop(%d)" % self.id


class DroppedPickup:
    """A C++ CDroppedPickup created through its constructor."""

    def __init__(self, pos, direction, item, count=1):
        p = _as_vec(pos)
        d = _as_vec(direction)
        self.id = _entity_api().create_dropped_pickup(
            p.x, p.y, d.x, d.y, str(item), int(count))

    def __repr__(self):
        return "DroppedPickup(%d)" % self.id


class MeleeSwing:
    """A native melee swing. C++ performs the hit test and returns hit count."""

    def __init__(self, owner, pos, direction, damage, range_scale=1.0,
                 can_harvest=False, hit_style=0, swing_effect=True):
        import lastday_weapon
        p = _as_vec(pos)
        d = _as_vec(direction)
        self.hits = lastday_weapon.melee_attack(
            _as_id(owner), p.x, p.y, d.x, d.y,
            int(damage), float(range_scale), bool(can_harvest),
            int(hit_style), bool(swing_effect))

    def __repr__(self):
        return "MeleeSwing(hits=%d)" % self.hits


class Context:
    """The object passed to every script callback.

    Attribute access is the supported API; dict-style item access is kept as a
    compatibility shim for older scripts.
    """

    def __init__(self, raw):
        self.event = raw["event"]
        client_id = raw["player"]
        self.player = Player(client_id) if client_id >= 0 else None
        entity_id = raw["entity"]
        self.entity = Entity(entity_id) if entity_id >= 0 else None
        item_id = raw["item"]
        self.item = Item(item_id) if item_id else None
        buff_id = raw["buff"]
        self.buff = Buff(buff_id, self.player, raw["state"]) if buff_id else None
        self.weapon = raw["weapon"] if raw["weapon"] else None
        self.kind = raw["kind"]
        # world (map) id of this callback; 0 when the source has no world
        self.world = raw["world"]
        self.pos = Vec2(raw["pos"][0], raw["pos"][1])
        self.dir = Vec2(raw["dir"][0], raw["dir"][1])
        # Constructor-style classes are available directly on the context, so
        # scripts do not need an extra import (and module import order cannot
        # break them).
        self.Vec2 = Vec2
        self.Player = Player
        self.Entity = Entity
        self.Item = Item
        self.Buff = Buff
        self.Projectile = Projectile
        self.FireParticle = FireParticle
        self.Laser = Laser
        self.HealthDrop = HealthDrop
        self.DroppedPickup = DroppedPickup
        self.MeleeSwing = MeleeSwing
        self.now = raw["now"]
        self.tick_speed = raw["tick_speed"]
        self.range_scale = raw["range_scale"]
        self.ammo = Ammo(raw["ammo"])
        self.extra = Extra(raw["extra"])
        self.params = raw["params"]
        self.state = raw["state"]
        self.result = raw["result"]
        self.consumed = bool(raw["consumed"])
        self.reload_ticks = raw["reload_ticks"]
        self.destroy = bool(raw["destroy"])

    # Dict-style access is a compatibility shim for older scripts. Handle-like
    # values are returned as their primitive id/name so code such as
    # lastday.player_heal(ctx["player"], 4) keeps working.
    def __getitem__(self, key):
        value = getattr(self, key)
        if key == "player" or key == "entity":
            return value.id if value is not None else None
        if key == "item" or key == "buff":
            return str(value) if value is not None else None
        return value

    def __setitem__(self, key, value):
        setattr(self, key, value)

    def __contains__(self, key):
        return hasattr(self, key)

    @property
    def world_name(self):
        return lastday.world_name(self.world)

    @property
    def floor_name(self):
        return lastday.floor_name(self.world)

    @property
    def world_kind(self):
        return lastday.world_kind(self.world)

    # Snapshot ids are pooled per floor. Allocate one for a custom snapshot
    # item and free it when the item goes away.
    def snap_new_id(self):
        return lastday.snap_new_id()

    def snap_free_id(self, snap_id):
        snap_id = int(snap_id)
        if self.world:
            return lastday.snap_free_id_for(snap_id, self.world)
        return lastday.snap_free_id(snap_id)

    def heal(self, amount):
        return self.player.heal(amount) if self.player is not None else 0

    def chat(self, text, context=""):
        return self.player.chat(text, context) if self.player is not None else False


def make_context(raw):
    """Build a Context from the plain dict C++ hands to the runtime."""
    return Context(raw)


# Also expose the constructor classes on the main host module so scripts can
# write lastday.Projectile(...) without a second import.
try:
    import lastday as _host

    _host.Vec2 = Vec2
    _host.Projectile = Projectile
    _host.FireParticle = FireParticle
    _host.Laser = Laser
    _host.HealthDrop = HealthDrop
    _host.DroppedPickup = DroppedPickup
    _host.MeleeSwing = MeleeSwing
except Exception:
    pass
