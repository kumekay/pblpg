# pblpg

A collection of Pebble apps and watchfaces, built for the **Pebble Time 2** (`emery`, board codename `obelix`).

## Projects

| Project | Type | Description |
|---|---|---|
| [permes](permes/) | Watchapp | Talk to a Hermes agent from your Pebble using voice dictation. |

Each project is self-contained and includes its own source, resources, configuration, and documentation.

## Requirements

- Pebble SDK with `emery` support
- `pebble` CLI
- QEMU emulator or compatible Pebble hardware

## Build and run

Run commands from the project you want to build:

```sh
cd permes
pebble build
pebble install --emulator emery
```

To inspect logs or capture a screenshot:

```sh
pebble logs --emulator emery
pebble screenshot --no-open --emulator emery screenshot.png
```

See each project's README for setup, usage, and testing details.

## Repository structure

```text
<project>/
├── package.json    # App metadata, target platforms, and resources
├── wscript         # Pebble build configuration
├── src/            # Watch and optional PebbleKit JS source
├── resources/      # Images, fonts, and other assets
└── README.md       # Project-specific documentation
```

## License

No license has been specified.
