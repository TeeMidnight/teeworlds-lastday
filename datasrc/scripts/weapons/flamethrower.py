# Flamethrower and flame cannon.
#
# The stock 0.7 client draws a WEAPON_HAMMER projectile without a projectile
# sprite, but it still emits a bullet-trail particle for every live projectile.
# A handful of short-lived stationary hammer projectiles therefore reads as a
# puff of embers. CProjectile::GetPos has no speed for WEAPON_HAMMER in both the
# client and the server, so the particles are placed along the cone instead of
# being given a velocity.
#
# Wall clipping: lastday.world_test_line sweeps a projectile-sized box along
# the aim ray, so the cone is shortened to the first solid tile and a puff is
# only spawned when its own line from the muzzle is clear.

import math

import lastday


WEAPON_HAMMER = 0
MUZZLE_OFFSET = 0.75
LINE_RADIUS = 5.0
CLEAR_BISECTIONS = 8

SOUND_HAMMER_FIRE = 3
SOUND_SHOTGUN_FIRE = 1

# Per-weapon shape. The ammo item supplies the base pellet count and damage;
# these values add the weapon's own character on top.
PROFILES = {
    "flamethrower": {
        "range": 240.0,
        "min_range": 36.0,
        "half_angle": 0.20,
        "extra_pellets": 0,
        "damage_bonus": 0,
        "span_min": 6,
        "span_max": 10,
        "sound": SOUND_HAMMER_FIRE,
    },
    "flame_cannon": {
        "range": 340.0,
        "min_range": 48.0,
        "half_angle": 0.30,
        "extra_pellets": 6,
        "damage_bonus": 1,
        "span_min": 8,
        "span_max": 14,
        "sound": SOUND_SHOTGUN_FIRE,
    },
}

DEFAULT_PROFILE = "flamethrower"


def _profile(name):
    if name in PROFILES:
        return PROFILES[name]
    return PROFILES[DEFAULT_PROFILE]


def _random01():
    return (lastday.random_int() % 1000) / 1000.0


def _blocked(ctx, start, end):
    return lastday.world_test_line(
        start.x, start.y, end.x, end.y, LINE_RADIUS
    )


def _clear_range(ctx, start, direction, maximum):
    """Return the distance along the aim ray before the first solid tile."""
    end = start + direction * maximum
    if not _blocked(ctx, start, end):
        return maximum

    low = 0.0
    high = maximum
    for _ in range(CLEAR_BISECTIONS):
        middle = (low + high) * 0.5
        probe = start + direction * middle
        if _blocked(ctx, start, probe):
            high = middle
        else:
            low = middle
    return max(0.0, low - LINE_RADIUS)


def _spawn_puff(ctx, pos, damage, span):
    # The third argument is the velocity. A hammer projectile ignores it on
    # both sides, so (0, 0) documents that the puff is a stationary particle.
    ctx.Projectile(
        ctx.player,
        pos,
        ctx.Vec2(0.0, 0.0),
        span,
        damage,
        False,
        -1,
        WEAPON_HAMMER,
    )


def on_fire(ctx):
    profile = _profile(ctx.weapon)
    ammo = ctx.ammo
    extra = ctx.extra

    direction = ctx.dir
    if direction.length() < 0.0001:
        direction = ctx.Vec2(0.0, -1.0)
    else:
        direction = direction.normalized()

    range_scale = ctx.range_scale if ctx.range_scale > 0.0 else 1.0
    maximum = profile["range"] * range_scale
    if maximum <= 8.0:
        return 0

    muzzle = ctx.pos + direction * extra.proximity_radius * MUZZLE_OFFSET
    clear = _clear_range(ctx, muzzle, direction, maximum)
    if clear < 8.0:
        # Firing straight into a wall still costs ammo; make the hiss audible.
        lastday.sound(ctx.pos.x, ctx.pos.y, profile["sound"])
        return 0

    # If the wall is closer than the nominal minimum range, pull the cone in so
    # the flame still appears at the wall instead of leaving a gap.
    minimum = min(profile["min_range"], max(8.0, clear * 0.5))

    pellets = max(1, ammo.projectiles + profile["extra_pellets"])
    damage = max(1, (ammo.damage or extra.default_damage) + profile["damage_bonus"])
    base_angle = math.atan2(direction.y, direction.x)
    usable = max(0.0, min(clear, maximum) - minimum)
    life_scale = ammo.lifetime if ammo.lifetime > 0.0 else 1.0

    for index in range(pellets):
        # The first puff always sits just in front of the muzzle (a muzzle
        # flash); the rest spread evenly through the cone.
        if index == 0:
            distance = minimum
        else:
            distance = minimum + usable * _random01()
        spread = profile["half_angle"] * (0.35 + 0.65 * (distance / maximum))
        angle = base_angle + (_random01() * 2.0 - 1.0) * spread
        px = muzzle.x + math.cos(angle) * distance
        py = muzzle.y + math.sin(angle) * distance - _random01() * 10.0
        if _blocked(ctx, muzzle, ctx.Vec2(px, py)):
            continue

        span = int(
            (
                profile["span_min"]
                + _random01() * (profile["span_max"] - profile["span_min"])
            )
            * life_scale
        )
        if span < 2:
            span = 2
        _spawn_puff(ctx, ctx.Vec2(px, py), damage, span)

    lastday.sound(ctx.pos.x, ctx.pos.y, profile["sound"])
    return 0
