# StreamBeast SRT Switcher (OBS Plugin)

A native C++/Qt6 OBS Studio plugin built for **[StreamBeast](https://streambeast.app)** OBS Bridge users (`srt://srt.streambeast.app:8890?streamid=read:<key>`).

It watches your IRL SRT feed inside OBS and automatically switches scenes when your mobile connection drops, stalls, or degrades — then switches back and recovers the Media Source when your signal returns.

## Features

- **Hybrid SRT Health Monitoring**
  - **Direct OBS Media Source Watch**: Monitors playback state (`PLAYING`, `OPENING`, `BUFFERING`, `ENDED`, `ERROR`, `STOPPED`) and detects frozen video frames when `media_time` stops advancing.
  - **StreamBeast API Telemetry (Optional)**: Auto-extracts your OBS Bridge stream key from your Media Source URL and polls `POST /api/srt/plugin-status` for instant offline/recovery detection (even when *"Close file when inactive"* is checked in OBS) and live bitrate telemetry.
- **Smart Scene Switching**
  - **Fallback / BRB Scene**: Switches after a configurable drop delay (debounce) so brief 1–2s cellular blips don't flicker your stream.
  - **Auto-Return to Live**: Switches back to your Live scene once the feed is stable for the recovery delay.
  - **Low-Bitrate Scene (Optional)**: Switches to a warning/low-signal scene when bitrate drops below your threshold, with recovery hysteresis to prevent ping-ponging.
  - **Auto-Restart Stuck Media Source**: Automatically restarts `ffmpeg_source` on a configurable cooldown if OBS gets stuck after a drop.
- **Streamer Safety Guards**
  - One-click **Auto-Switching ON/OFF** toggle in the OBS Dock (`View → Docks → StreamBeast SRT Switcher`).
  - Optional **"Only auto-switch while Streaming or Recording"**.
  - Optional **"Only switch when on Live / Fallback / Low-Bitrate scenes"** so manual scenes (Intro, Outro, Desktop) are never hijacked.

## Building

### Standalone Core Unit Tests (no OBS SDK required)

```bash
cmake -S . -B build_tests -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON
cmake --build build_tests
ctest --test-dir build_tests --output-on-failure
```

### Full OBS Plugin Build (Linux / macOS / Windows)

This repository uses the official [`obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate) CMake + `buildspec.json` + GitHub Actions pipeline:

- **GitHub Actions**: Push to GitHub to automatically build installers/packages for **Windows x64**, **macOS Universal**, and **Ubuntu x86_64**.
- **Local Linux**: Install OBS development libraries and Qt6 (`libobs-dev` / `obs-studio` from `ppa:obsproject/obs-studio`, `qt6-base-dev`, `cmake`, `ninja-build`), then:

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```
