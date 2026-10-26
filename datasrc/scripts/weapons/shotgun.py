"""Shotgun: spread burst of pellets, full auto. Mirrors CWeaponShotgun."""

import math

import lastday


SOUND_SHOTGUN_FIRE = 1
WEAPON_SHOTGUN = 2
DEFAULT_PELLETS = 5
DEFAULT_SPEEDDIFF = 0.8
SPREAD_HALF_ANGLE = 0.185
MUZZLE_OFFSET = 0.75


def _pellet_spread(index, pellets, speeddiff):
    """Return (angle offset, speed factor) for one pellet in the burst."""
    if pellets <= 1:
        return 0.0, 1.0

    t = float(index) / float(pellets - 1)
    offset = -SPREAD_HALF_ANGLE + 2.0 * SPREAD_HALF_ANGLE * t
    # outer pellets fly slower, exactly like the built-in shotgun
    speed_factor = 1.0 - abs(t - 0.5) * 2.0 * (1.0 - speeddiff)
    return offset, speed_factor


def on_fire(ctx):
    ammo = ctx.ammo
    extra = ctx.extra
    direction = ctx.dir
    damage = ammo.damage or extra.default_damage
    muzzle = ctx.pos + direction * extra.proximity_radius * MUZZLE_OFFSET
    lifetime_ticks = int(
        lastday.tuning("shotgun_lifetime") * ammo.lifetime * ctx.range_scale * ctx.tick_speed
    )

    # an ammo item may override the pellet count (e.g. a slug shell = one)
    pellets = max(1, ammo.projectiles or DEFAULT_PELLETS)
    speeddiff = lastday.tuning("shotgun_speeddiff")
    if speeddiff <= 0.0:
        speeddiff = DEFAULT_SPEEDDIFF

    base_angle = math.atan2(direction.y, direction.x)
    for index in range(pellets):
        offset, speed_factor = _pellet_spread(index, pellets, speeddiff)
        angle = base_angle + offset
        speed = speed_factor * ammo.speed
        velocity = (math.cos(angle) * speed, math.sin(angle) * speed)
        ctx.Projectile(
            ctx.player,
            muzzle,
            velocity,
            lifetime_ticks,
            damage,
            False,
            -1,
            WEAPON_SHOTGUN,
        )

    lastday.sound(ctx.pos.x, ctx.pos.y, SOUND_SHOTGUN_FIRE)
    return 0
