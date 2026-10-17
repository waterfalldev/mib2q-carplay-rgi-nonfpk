# Renderer host checks

Linux/Docker checks for `maneuver_render`: three test suites and a geometry fingerprint of
what the renderer draws. Runs on Linux, macOS and Git Bash on Windows; needs Docker and the
`qnx65-armv7-toolchain` image.

## Run

```bash
tests/renderer/run.sh                      # build + run the three suites
tests/renderer/run.sh . check              # compare the fingerprint with fingerprint.sum
tests/renderer/run.sh . digest > tests/renderer/fingerprint.sum   # accept an intended change
tests/renderer/run.sh <tree> fingerprint > a.txt                  # full fingerprint (~16 MB)
FP_VERTS=full tests/renderer/run.sh <tree> fingerprint > a.txt    # every vertex (~92 MB)
```

The first argument is the source tree (default: this repo); a git worktree of another commit
works too. Every mode exits non-zero on a build, test or unit failure.

Any change to `maneuver_render/` should leave `check` passing, or update `fingerprint.sum`
in the same commit when the drawing change is intended. To see what changed, fingerprint
the tree before and after the change and diff the two files.

## How it builds

The image is the host-test image `carplay-rgi-host-tests:local` (`tests/Dockerfile.host`: `qnx65-armv7-toolchain`
plus host `g++`; built once on first run, which needs network). Sources build with host
gcc/g++ as `-DPLATFORM_QNX -D__QNXNTO__` against the SDP's EGL/GLES2/KHR headers, with
`-fsanitize=undefined -fno-sanitize-recover=all` (no ASan, as in `tests/most/run-native-tests.sh`)
and C++11 without exceptions or RTTI. The tree is mounted read-only.

Fakes: `gl_fake.c` (every GLES2 call, `eglGetProcAddress`, `glGetString`; records draws when
asked), `fake_platform.c` (`platform_*`, plus the `map_layer_*` / `cluster_video_*` entry
points `main.c` references) and `shim/sys/neutrino.h` (`setprio`). The platform is the car
one (SSAA 1.6, car path), not `PLATFORM_MACOS`.

## Suites

`lane_guidance_test`, `renderer_peer_loss_test` (real loopback TCP inside the container) and
`maneuver_parity_test`, which includes `main.c` and `maneuver.c` and links `render.c`,
`route_path.c`, `server.c` and `scene/{scene,geometry,layout,lane_panel}.cpp`.

## Fingerprint

`fingerprint.c` drives `main.c`'s real engine (`apply_commands`, `engine_*`,
`prepare_engine_scenes`, tick, lane panel, the loop body) at 30 fps virtual time with the
production scene provider and scenes: 223 units, about 790 frames and 31k draws. Each unit
runs in a forked process from pristine state. Units cover:

- the settled frame of every icon for both driving sides, in 3D and 2D;
- turn angles -180..180 (road scene up to 100 degrees, native beyond);
- junction and ring-road families; arrive, U-turn, exit, merge and lane-change directions;
- the built-in fallback without a scene; slide positions 0..2;
- route progress (fill, path weight, glow) and the arrow progress protocol;
- current + next pushes (each icon as current and next, a 3-deep queue, progress during a push, 2D);
- the lane panel (4 layouts, under a push, fade out); the popup crop switch; reveal fade-in;
  the debug grid and overlay.

Each draw records the framebuffer, the program (a hash of its shader sources), pipeline
state, uniforms (only changes since the previous draw are printed), attribute layout and
vertices. Floats are rounded to 1e-4. Draws with up to 6 vertices print every vertex;
larger ones print per-64-vertex chunk hashes plus per-component min/max/sum. CLEAR lines,
per-frame engine state and SCENE lines (kind, fallback, command count, mesh hash, camera,
bounds, route segments) are included. Textures and FBOs are named by size and creation
order, not GL name. `CLOCK_MONOTONIC` is virtualised, since `render.c` animates the popup crop
and lane reframing from it.

`digest` reduces the fingerprint to one MD5 per unit; `fingerprint.sum` holds that digest
for the current tree.

## Limits

- Hashes are over values rounded to 1e-4: a change that moves a float by about 1e-7 can cross
  a rounding boundary and change a unit with no visible difference. Diff `FP_VERTS=full`
  fingerprints to judge.
- Not covered: the MOST opaque-output path (`render_set_output`, see `tests/most/`), the
  map/cluster video path, `platform_qnx.c`, screenshot readback, TCP command decoding on a
  live socket, real GPU rasterisation and shader compilation (shaders are identified by
  source hash, not run).
- Changing `fingerprint.c` or the fakes changes the digest of every unit.
