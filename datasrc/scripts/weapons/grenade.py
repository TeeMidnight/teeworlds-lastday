# Grenade: explosive projectile, full auto. Mirrors CWeaponGrenade.
import lastday

SOUND_GRENADE_FIRE = 2
SOUND_GRENADE_EXPLODE = 6
WEAPON_GRENADE = 3


def on_fire(ctx):
    ammo = ctx.ammo
    extra = ctx.extra
    direction = ctx.dir

    damage = ammo.damage or extra.default_damage
    start = ctx.pos + direction * extra.proximity_radius * 0.75
    span = int(lastday.tuning("grenade_lifetime") * ammo.lifetime * ctx.range_scale * ctx.tick_speed)

    ctx.Projectile(ctx.player, start, direction * ammo.speed, span, damage,
                   True, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE)
    lastday.sound(ctx.pos.x, ctx.pos.y, SOUND_GRENADE_FIRE)
    return 0


def on_tick(ctx):
    pass


def on_switch(ctx):
    pass
