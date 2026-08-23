# pblpg — Pebble OS apps monorepo

This repo holds multiple independent Pebble OS app projects, one per top-level
directory. The first (and reference) project is `permes/`.

## Target hardware

All apps target **Pebble Time 2** — hardware board codename **obelix**, SDK
platform name **emery**.

| Property | Value |
|---|---|
| SDK platform | `emery` (QEMU: `--emulator emery`) |
| Display | 200×228, rectangular, 64-color |
| Language options | C (always supported) or Alloy/JS (emery + gabbro only) |

Build for `emery` only unless a project's README says otherwise. Legacy
platforms (aplite, basalt, chalk, diorite, flint) in scaffolded
`package.json` files are boilerplate — don't add compatibility code for them.

## Toolchain

- `pebble` CLI: Pebble Tool v5.0.39, active SDK **v4.33.1** (`pebble sdk list`)
- SDK docs (doxygen C API + guides + JS docs): `$HOME/p/coredevices/sdk-docs`
  - API headers/docs: `sdk-docs/{basalt,aplite}/doxygen_sdk`, `sdk-docs/js-docs`
  - Dev guides: `sdk-docs/docs/` (`development.md`, `troubleshooting.md`)
- PebbleOS firmware source (for hardware/board reference): `$HOME/p/coredevices/PebbleOS`
- Local skill: `.agents/skills/pebble-watchface/` — read `SKILL.md` plus the
  relevant `reference/*.md` before non-trivial UI work; it covers C and Alloy,
  AppMessage/pkjs patterns, and QEMU testing workflows.

## Project layout (per app)

Standard Pebble project, self-contained in its directory:

```
<app>/
  package.json     # UUID, displayName, platforms, messageKeys, resources
  wscript          # build rules (default scaffold — rarely needs edits)
  src/c/           # watch-side C code
  src/pkjs/        # PebbleKit JS (phone side), if used
  resources/       # images/fonts (declared in package.json)
  README.md        # what the app does, how to run it
```

There is no top-level build. Always `cd <app>` before running `pebble` commands.

## Common commands (run inside a project dir)

```sh
pebble build                                   # builds all targetPlatforms
pebble install --emulator emery                # install into QEMU
pebble logs --emulator emery                   # tail app logs
pebble screenshot --no-open --emulator emery shot.png
pebble emu-button click select --emulator emery   # launch/interact
pebble kill && pebble wipe                     # reset wedged/stale emulator
```

Emulator notes (from the skill, worth following):
- After a fresh `pebble` boot of the emulator, wait ~20s before `pebble install`.
- Install can land on the launcher with the app highlighted but not launched —
  that looks like a crash but isn't; press select via `emu-button`.
- If screenshots show the wrong app or install times out: `pebble kill && pebble wipe`, then reinstall.

## Projects

### permes/

Watchapp for the **hermes agent** (Valera, Nous Research hermes-agent running
on this host). C watchapp + pkjs bridge. UUID is fixed in `package.json` —
never regenerate it.

- hermes gateway exposes `platforms.api_server` (REST, Bearer key) on
  `0.0.0.0:8642`, fronted publicly by an HTTPS host (private — not in this
  repo). Config lives in `~/.hermes/config.yaml`; the phone app reads URL +
  key from `src/pkjs/config.local.js` (gitignored; see
  `config.local.js.example`).
- Threads = hermes sessions (`/api/sessions`); turns = async `/v1/runs`
  polled from pkjs; dictation via watch `DictationSession`.
- AppMessage protocol: `src/c/protocol.h` + `messageKeys` in package.json.
- Full protocol/UX/testing details: `permes/README.md`.
- QEMU runs the real pkjs runtime, so the whole flow is testable locally;
  `pebble transcribe` simulates dictation (no mic in the emulator).

## Conventions

- One directory per app; unique `pebble.uuid` per app (generate with
  `.agents/skills/pebble-watchface/scripts/generate_uuid.py`).
- Watchapp vs watchface: set `pebble.watchapp.watchface` accordingly;
  watchfaces must not subscribe to buttons.
- Battery: `tick_timer_service_subscribe` with `MINUTE_UNIT` only; no
  continuously-running app timers unless explicitly requested.
- Static resources live in `resources/` and must be declared in `package.json`
  `pebble.resources.media`.
- Verify changes end-to-end: build → install on emery emulator → screenshot →
  visually confirm before claiming done.
