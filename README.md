# StreamBeast IRL (OBS Studio Plugin)

A native C++/Qt6 OBS Studio plugin built exclusively for the **[StreamBeast](https://streambeast.app) OBS Bridge** (`srt://srt.streambeast.app:8890?streamid=read:<key>`).

> [!IMPORTANT]
> **StreamBeast OBS Bridge Only:** This plugin is designed **specifically and exclusively** to work with the OBS Bridge provided by **[streambeast.app](https://streambeast.app)**. It is **not** a generic SRT/RTMP scene switcher and will not work with third-party SRT relays, direct peer-to-peer SRT feeds, RTMP sources, or other streaming services.

It monitors your StreamBeast IRL SRT feed inside OBS Studio and automatically switches scenes when your mobile connection drops, stalls, or degrades — then switches back to Live and restarts the Media Source once your signal recovers.

---

## How It Works

1. **StreamBeast OBS Bridge Telemetry:** The plugin extracts your OBS Bridge stream key from your OBS Media Source URL (`srt://srt.streambeast.app:8890?streamid=read:<key>`) and polls `POST https://streambeast.app/api/srt/plugin-status` for live/offline state, bitrate, resolution, FPS, and stream quality.
2. **OBS Media Source Health Watch:** Alongside bridge telemetry, the plugin monitors the OBS Media Source (`ffmpeg_source`) playback state (`PLAYING`, `OPENING`, `BUFFERING`, `ENDED`, `ERROR`, `STOPPED`) and checks whether `media_time` stops advancing (frozen frame detection).
3. **Debounced Scene Switching:** Using both signals, the state machine switches between your **Live**, **Fallback / BRB**, and optional **Low-Bitrate** scenes with configurable delays and hysteresis — even when *"Close file when inactive"* is enabled in OBS.

---

## Features

- **Hybrid Bridge + Local Monitoring**
  - Instant offline and recovery detection from the StreamBeast OBS Bridge API, even if OBS has closed an inactive Media Source.
  - Local frame-stall detection (`media_time` watchdog) to catch frozen decoder frames.
- **Smart Scene Switching**
  - **Fallback / BRB Scene:** Configurable drop delay (debounce) prevents brief 1–2s cellular blips from flickering your stream.
  - **Auto-Return to Live:** Waits for a configurable recovery delay before returning to your Live scene.
  - **Low-Bitrate Scene (Optional):** Switches to a low-signal warning scene when StreamBeast reports bitrate below your threshold, with a recovery buffer (hysteresis) to avoid rapid back-and-forth switching.
  - **Auto-Restart Stuck Media Source:** Automatically restarts your `ffmpeg_source` on a configurable cooldown if OBS stalls after a drop.
- **Streamer Safety Guards**
  - One-click **Auto-Switching ON/OFF** toggle in the OBS dock (`View → Docks → StreamBeast IRL`).
  - Optional **"Only auto-switch while Streaming or Recording"**.
  - Optional **"Only switch when on Live / Fallback / Low-Bitrate scenes"** so manual scenes (Intro, Outro, Desktop) are never interrupted.
- **Random Clips Player (Fallback / BRB)**
  - Automatically plays random video clips (`.mp4`, `.mkv`, `.mov`, `.webm`, `.flv`, `.ts`, `.avi`, and more) from a chosen directory on your **Fallback / BRB** scene when disconnected.
  - Shuffle-without-repeat mode cycles through every clip before repeating and avoids back-to-back repeats, plus a one-click **Create** button to add a configured Media Source directly to your Fallback scene.
- **Native OBS Remote Bridge**
  - Connect directly to **[StreamBeast OBS Remote](https://streambeast.app/tools/obs-remote)** by pasting your OBS Overlay URL — no Browser Source overlay or OBS WebSocket password required.
  - Remotely switch scenes, start/stop streaming, recording, and replay buffer, toggle scene item visibility, adjust audio mixer mute/volume, and stream live OBS telemetry.

---

## Requirements

- **OBS Studio** (tested on **Windows x64** and **Linux x86_64**; macOS Universal builds are produced via CI)
- An active **[StreamBeast](https://streambeast.app)** account using the **OBS Bridge** SRT feed

---

## Setup in OBS Studio

1. Download the latest installer or archive for your OS from [Releases](../../releases) (or build from source below).
2. In OBS Studio, add a **Media Source** using your StreamBeast OBS Bridge SRT URL (`srt://srt.streambeast.app:8890?streamid=read:<key>`).
3. Open the plugin dock via **View → Docks → StreamBeast IRL** (or **Tools → StreamBeast IRL...**).
4. In the **Auto Scene** tab:
   - Select your **SRT Media Source** (StreamBeast sources are labeled `[StreamBeast SRT]`).
   - Select your **Live Scene**, **Fallback / BRB** scene, and optionally a **Low-Bitrate Scene**.
   - Scroll down in the **Auto Scene** tab to adjust detection timings, low-bitrate thresholds, safety guards, or click **Test Connection** to verify communication with `streambeast.app`.
5. *(Optional)* In the **Random Clips** tab:
   - Select an existing **Clip Media Source** on your Fallback scene (or click **Create** to add one automatically), **Browse...** to choose your clips folder, and toggle **Random Clips: ON**.
6. *(Optional)* In the **OBS Remote** tab:
   - Paste your OBS Overlay URL from `streambeast.app/tools/obs-remote` and toggle **OBS Remote: ON**.

---

## Open-Source & Network Transparency

This repository contains the complete plugin source code so anyone can inspect how it operates:

- **External Network Requests:**
  - When StreamBeast API monitoring is enabled: `POST https://streambeast.app/api/srt/plugin-status` with JSON `{"streamKey":"<key>"}` (3-second timeout).
  - When **OBS Remote** is enabled: persistent SSE stream `GET https://streambeast.app/api/obs-remote/overlay/events?token=<token>` and state updates `POST https://streambeast.app/api/obs-remote/overlay/state`.
- **Local Persistence:** Plugin configuration is stored locally in OBS scene collection data and the OBS plugin config directory (`config.json`). No data is sent to any third party.

---

## Building & Testing

### Standalone Core Unit Tests (no OBS SDK required)

The switching state machine and URL/key parser in [src/switcher-core.cpp](src/switcher-core.cpp) have zero OBS/Qt dependencies and can be compiled and tested standalone:

```bash
cmake -S . -B build_tests -DBUILD_PLUGIN=OFF -DBUILD_TESTS=ON
cmake --build build_tests
ctest --test-dir build_tests --output-on-failure
```

### Full OBS Plugin Build

This project uses the official [`obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate) CMake + `buildspec.json` + GitHub Actions pipeline:

- **GitHub Actions:** Automatically builds and packages artifacts for **Windows x64**, **macOS Universal**, and **Ubuntu x86_64**.
- **Local Linux (Ubuntu x86_64):** Install OBS Studio development packages and Qt6 (`libobs-dev` / `obs-studio`, `qt6-base-dev`, `cmake`, `ninja-build`), then run:

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

- **Local Windows (x64):** Configure and build using the `windows-x64` preset from a Visual Studio 2022 environment:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

---

## License

Licensed under the **GNU General Public License v2.0 or later** — see [LICENSE](LICENSE).
