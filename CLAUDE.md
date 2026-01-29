# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

iButton tester for embedded Linux (IMX6UL ARM boards). Monitors 1-Wire (w1) bus devices and communicates via MQTT using the Paho C client library.

## Build Commands

```bash
# Build (cross-compiles for ARM Cortex-A7)
make all

# Clean
make clean
```

Cross-compiler: `arm-poky-linux-gnueabi-gcc` with sysroot at `/opt/fsl-imx-fb/6.6-scarthgap/sysroots/cortexa7t2hf-neon-poky-linux-gnueabi`

## Running

```bash
ibutton-tester [-c PATH] [-v|-q|--log-level=N] [--dry-run]
```

- `-c, --config PATH` - Config file (default: `/etc/ibutton-tester/config.json`)
- `-v` - Increase verbosity (repeatable)
- `--dry-run` - Load config, print devices, exit

## Architecture

Single-file C application (`src/ibutton_tester.c`) with embedded JSMN JSON parser (`src/jsmn.h`).

**Key components:**
- **Configuration**: JSON config parsed via JSMN, includes MQTT broker settings and 1-Wire device filtering
- **1-Wire scanning**: Reads `/sys/bus/w1/devices/` directory, filters by family ID (e.g., "01" for iButton)
- **MQTT pub/sub**: Subscribes to command topic, publishes JSON events to state topic
- **Commands**: `status`, `scan`, `test` (presence detection with timeout/debounce)

**Constraints:**
- Fixed-size arrays (MAX_DEVICES=32, MAX_JSON=1024)
- No dynamic memory allocation
- Logging macros: `LOGE()`, `LOGW()`, `LOGI()`, `LOGD()`

**JSON protocol** - All MQTT payloads have an "event" field:
```json
{"event": "test", "result": "pass", "device": "01-abcdef", "elapsed_ms": 1234}
```
