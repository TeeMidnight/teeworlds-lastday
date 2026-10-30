# Flamethrower and flame cannon.
#
# The stock 0.7 client draws a WEAPON_HAMMER projectile without a projectile
# sprite, but it still emits a bullet-trail particle for every live projectile.
# The fire jet is a cloud of those hammer-type particles: every puff gets its
# own angle and speed, gravity bends it into an arc, and a puff that hits the
# ground lands and fades out instead of floating forever.
#
# Wall clipping: lastday.world_test_line sweeps a projectile-sized box along
# the aim ray, so the muzzle clearance is known before the jet starts. The
# server-side CFireParticle then sweeps its own movement against walls and
# stops on the first solid tile.

import math

import lastday


WEAPON_HAMMER = 0
MUZZLE_OFFSET = 0.75
LINE_RADIUS = 5.0
CLEAR_BISECTIONS = 8
MUZZLE_START = 4.0
MIN_START = 1.0

SOUND_HAMMER_FIRE = 3
SOUND_SHOTGUN_FIRE = 1

# Per-weapon shape. The ammo item supplies the base pellet count, damage and
# lifetime; these values add the weapon's own character on top. "range" is the
# ray length used to find the first wall (and the base for range_scale); the
# actual reach comes from "speed" and "gravity".
PROFILES = {
    "flamethrower": {
        "range": 240.0,
        "half_angle": 0.20,
        "extra_pellets": 0,
        "damage_bonus": 0,
        "speed": 14.0,
        "gravity": 0.16,
        "span_min": 24,
        "span_max": 36,
        "sound": SOUND_HAMMER_FIRE,
    },
    "flame_cannon": {
        "range": 340.0,
        "half_angle": 0.30,
        "extra_pellets": 8,
        "damage_bonus": 1,
        "speed": 17.0,
        "gravity": 0.13,
        "span_min": 30,
        "span_max": 44,
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


def _spawn_puff(ctx, pos, velocity, damage, span, gravity):
    # A dedicated server-side entity: it snaps as a hammer projectile (the
    # client draws its ember trail), but owns the swept hit test and the
    # velocity/gravity simulation.
    ctx.FireParticle(
        ctx.player,
        pos,
        velocity,
        span,
        damage,
        WEAPON_HAMMER,
        None,
        gravity,
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

    pellets = max(1, ammo.projectiles + profile["extra_pellets"])
    damage = max(1, (ammo.damage or extra.default_damage) + profile["damage_bonus"])
    base_angle = math.atan2(direction.y, direction.x)
    life_scale = ammo.lifetime if ammo.lifetime > 0.0 else 1.0
    speed_scale = (ammo.speed if ammo.speed > 0.0 else 1.0) * range_scale
    # Start the jet just ahead of the muzzle, but never inside the wall the
    # player is already pressed against.
    start_distance = min(MUZZLE_START, max(MIN_START, clear * 0.5))

    for _ in range(pellets):
        # A flamethrower is a cone of independent embers, not one rigid stream:
        # each puff picks its own angle and speed, then gravity drops it.
        spread = profile["half_angle"] * (0.35 + 0.65 * _random01())
        angle = base_angle + (_random01() * 2.0 - 1.0) * spread
        speed = profile["speed"] * speed_scale * (0.75 + 0.5 * _random01())
        offset = ctx.Vec2(math.cos(angle), math.sin(angle)) * start_distance
        start = ctx.Vec2(muzzle.x + offset.x, muzzle.y + offset.y)
        if _blocked(ctx, muzzle, start):
            continue
        velocity = ctx.Vec2(math.cos(angle) * speed, math.sin(angle) * speed)

        span = int(
            (
                profile["span_min"]
                + _random01() * (profile["span_max"] - profile["span_min"])
            )
            * life_scale
        )
        if span < 2:
            span = 2
        _spawn_puff(ctx, start, velocity, damage, span, profile["gravity"])

    lastday.sound(ctx.pos.x, ctx.pos.y, profile["sound"])
    return 0
