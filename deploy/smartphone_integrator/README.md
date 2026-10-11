# CarPlay child supervisor deployment

Replace `children.carplay` in
`/mnt/system/etc/eso/production/smartphone_integrator.json` with
[`carplay_child.json`](carplay_child.json).

The important ownership rule is that `children.carplay.envs` contains **no**
`LD_PRELOAD`. `carplay_startup.sh` applies the hook only to its direct
`dio_manager` child. This prevents the hook from loading into `/bin/sh` or
`maneuver_render`.

Keep `IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production` in the child environment.
The supervisor inherits it and passes it unchanged to `dio_manager`; only the
hook variable is added on the final `dio_manager` command.

Install these executable files under `/mnt/app/root/hooks`:

- `carplay_startup.sh`
- `carplay_monitor.sh`
- `carplay_cleanup.sh`
- `carplay_processes.sh`
- `libcarplay_hook.so`
- `maneuver_render`
- `flag_atlas.rgba`

Do not overwrite `/etc/scripts/carplay_cleanup.sh`: the custom cleanup calls that
stock script only for Audi's mdnsd/PPS cleanup. The renderer is a persistent
service; it exits only on its own failure, a reboot or an explicit maintenance action.

Lifecycle semantics (owner file, adoption, backoff, cleanup, what the supervisor never touches) and the
QNX 6.5 compatibility notes are in
[supervisor-lifecycle](../../docs/deploy/supervisor-lifecycle.md).

Verify on the unit in `/tmp/carplay_wrapper.log`: `[monitor] adopted maneuver_render`
or `[monitor] starting maneuver_render`, followed on dio exit by
`renderer remains persistent`.

CarPlay uses `restartDelay: 3000`, `stopTimeout: 8000`; `portResetTime` remains 250 ms.
The full settling delay is intentional: rapid OTG stop/start cycles can leave the
iAP2 NCM accessory with only one of its two interfaces enabled. Renderer ownership
is independent of it and must not be used as a reason to shorten the USB/device-stack
delay.

