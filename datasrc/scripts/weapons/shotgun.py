# Shotgun: spread burst of pellets, full auto. Mirrors CWeaponShotgun.
import math

import lastday

SOUND_SHOTGUN_FIRE = 1
WEAPON_SHOTGUN = 2
DEFAULT_PELLETS = 5


def on_fire(ctx):
    ammo = ctx.ammo
    extra = ctx.extra
    direction = ctx.dir

    damage = ammo.damage or extra.default_damage
    start = ctx.pos + direction * extra.proximity_radius * 0.75
    span = int(lastday.tuning("shotgun_lifetime") * ammo.lifetime * ctx.range_scale * ctx.tick_speed)

    # an ammo item may override the pellet count (e.g. a slug shell = one)
    pellets = ammo.projectiles or DEFAULT_PELLETS
    if pellets < 1:
        pellets = 1

    speeddiff = lastday.tuning("shotgun_speeddiff")
    if speeddiff <= 0.0:
        speeddiff = 0.8

    base_angle = math.atan2(direction.y, direction.x)
    for i in range(pellets):
        if pellets > 1:
            t = float(i) / float(pellets - 1)
            offset = -0.185 + 0.370 * t
            # outer pellets fly slower, exactly like the built-in shotgun
            v = 1.0 - abs(t - 0.5) * 2.0 * (1.0 - speeddiff)
        else:
            offset = 0.0
            v = 1.0

        angle = base_angle + offset
        velocity = (math.cos(angle) * v * ammo.speed, math.sin(angle) * v * ammo.speed)
        ctx.Projectile(ctx.player, start, velocity, span, damage, False, -1, WEAPON_SHOTGUN)

    lastday.sound(ctx.pos.x, ctx.pos.y, SOUND_SHOTGUN_FIRE)
    return 0


def on_tick(ctx):
    pass


def on_switch(ctx):
    pass
