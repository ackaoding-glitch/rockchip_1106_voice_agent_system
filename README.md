# rockchip_1106_voice_agent_system

**A real-time voice agent system for Rockchip RV1106, with AI vision and face recognition.**

[中文文档](README_CN.md) | English

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-RV1106-green.svg)](#hardware)
[![Language](https://img.shields.io/badge/Language-C11-orange.svg)](#build)

---

## What is this?

This is the on-device client for an embedded voice AI agent running on the Rockchip RV1106 SoC. It handles the full device-side pipeline — from microphone capture and VAD to real-time duplex audio streaming, face recognition, and music playback — all in ~5K lines of C.

The system is designed for always-on, low-latency conversational AI on resource-constrained hardware (128MB RAM, ARM Cortex-A7).

### Key Capabilities

- **Real-time voice interaction** — sub-100ms VAD latency, Opus-encoded streaming uplink, UDP-based TTS downlink
- **Hardware-accelerated audio processing** — AEC, beamforming, noise reduction, AGC via Rockchip VQE pipeline
- **On-device face recognition** — face detection and face recognition(30fps)
- **Vision upload** — camera frame capture with server-side AI analysis
- **BLE provisioning** — zero-touch WiFi setup via GATT service
- **Robust connectivity** — auto-reconnect WebSocket, WiFi recovery, graceful degradation

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         zh_client                               │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌─────────────┐  ┌────────────┐  │
│  │  Audio    │  │  Audio   │  │    Face      │  │   Music    │  │
│  │  Uplink   │  │ Playback │  │ Recognition  │  │   Player   │  │
│  │          │  │          │  │             │  │            │  │
│  │ Capture → │  │ Opus  →  │  │ Camera  →   │  │ HTTP   →   │  │
│  │ VAD    → │  │ Decode → │  │ Detect  →   │  │ MP3    →   │  │
│  │ Opus   → │  │ PCM   → │  │ Embed   →   │  │ Resamp →   │  │
│  │ Upload   │  │ Speaker  │  │ Match/Upload │  │ Playback   │  │
│  └────┬─────┘  └────┬─────┘  └──────┬──────┘  └─────┬──────┘  │
│       │              │               │               │          │
│  ┌────┴──────────────┴───────────────┴───────────────┴──────┐  │
│  │                    bithion-core SDK                        │  │
│  │  HTTP/WS/UDP transport · VAD engine · Session management  │  │
│  └──────────────────────────┬────────────────────────────────┘  │
│                             │                                   │
│  ┌──────────────────────────┴────────────────────────────────┐  │
│  │              Rockchip RK_MPI / Rockit                      │  │
│  │           Audio I/O · VQE (AEC/BF/ANR/AGC)                │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
                         ┌────┴────┐
                         │ RV1106  │
                         │ Hardware│
                         └─────────┘
```

### Audio Pipeline

```
         UPLINK                                    DOWNLINK
  Mic ──→ Rockit Capture ──→ VQE/AEC          Server T(TS Opus/UDP)
              │                                      │
         2ch 16kHz                              Opus Decode
              │                                      │
             VAD                               Gain Adjust
              │                                      │
         Preroll Buffer (10 frames)             Rockit/ALSA
              │                                      │
         Opus Encode (16kbps mono)              Speaker Out
              │
         UDP Uplink → Server
```

### Session Lifecycle

```
Boot → WiFi Check ──[fail]──→ BLE Provisioning ──→ WiFi Connect
                │                                        │
                └──[ok]──→ HTTP Device Bind ─────────────┘
                                  │
                           WS Auth (5s timeout)
                                  │
                    ┌─────────────┴─────────────┐
                    │      Main Event Loop       │
                    │                            │
                    │  VAD Start ──→ Audio TX    │
                    │  STT Done  ──→ TTS RX     │
                    │  Face Detect → Upload      │
                    │  Music Cmd  → Playback     │
                    │                            │
                    └────────────┬───────────────┘
                                 │
                          [WS error/close]
                                 │
                          Auto Reconnect (3s)
```

### Build Artifacts

| Binary | Description |
|--------|-------------|
| `zh_client` | Main voice agent daemon |
| `zh_ble_gatt_server` | BLE WiFi provisioning service |

## Build

### Prerequisites

- **Host:** Ubuntu 22.04 x86_64
- **Toolchain:** `arm-rockchip830-linux-uclibcgnueabihf-gcc-8.3.0` (included in RK SDK)
- **Rockchip SDK:** Board-level SDK for RV1106 (minimal extraction provided)
- **bithion-core SDK:** Communication and VAD engine

### Quick Start

```bash
# Download SDKs
mkdir sdk && cd sdk
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/rv110x_ipc_min_sdk_final_20260323.tar.gz
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/bithion-core-sdk-20260323.tar.gz
tar xzf rv110x_ipc_min_sdk_final_20260323.tar.gz
tar xzf bithion-core-sdk-20260323.tar.gz
cd ..

# Build
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
  -DTOOLCHAIN_ROOT=sdk/rv110x_ipc_min_sdk_final_20260323/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf \
  -DRK_SDK_ROOT=sdk/rv110x_ipc_min_sdk_final_20260323 \
  -DBITHION_CORE_SDK_ROOT=sdk/bithion-core-sdk
cmake --build build -j$(nproc)
```

### Deployment

For full device deployment (flashing + installation), see the companion repository:
[rv1106_zh_clinet_installer](https://github.com/ackaoding-glitch/rockchip_1106_ai_client_installer)

## Hardware

| Component | Specification |
|-----------|--------------|
| **SoC** | Rockchip RV1106 (ARM Cortex-A7) |
| **RAM** | 128MB |
| **Audio In** | Dual-mic (stereo capture, AEC reference on R channel) |
| **Audio Out** | Mono speaker via codec |
| **Camera** | V4L2-compatible |
| **Network** | WiFi 802.11 b/g/n |
| **Bluetooth** | BLE for provisioning |
| **NPU** | RKNN for face model inference |
| **Storage** | NAND/eMMC (`/oem/`, `/data/` partitions) |

## Protocol Stack

| Layer | Protocol | Purpose |
|-------|----------|---------|
| Transport | **UDP** | Low-latency TTS audio streaming |
| Application | **WebSocket** | Bidirectional control & signaling |
| Application | **HTTP/TLS** | Device binding, file upload (S3 presigned) |
| IPC | **Unix Domain Socket** | Face engine communication |
| Bluetooth | **BLE GATT** | WiFi credential provisioning |

## Configuration

Key parameters in [`include/config.h`](include/config.h):

| Parameter | Default | Description |
|-----------|---------|-------------|
| `ZH_AUDIO_FRAME_SAMPLES` | 160 | Samples per frame (16kHz) |
| `ZH_TTS_GAIN` | 0.5 | TTS playback volume multiplier |
| `ZH_WS_RECONNECT_INTERVAL_MS` | 3000 | WebSocket reconnect delay |
| `ZH_RK_ENABLE_LOOPBACK` | 1 | AEC loopback reference enable |

Audio VQE pipeline configuration: [`scripts/installer/config_aivqe.json`](scripts/installer/config_aivqe.json)

## Open Source Boundary

This repository contains the **device-side application layer**. You can study:

- The host program architecture and threading model
- How a voice agent client integrates with a proprietary communication SDK
- Board-level deployment scripts and resource organization
- Audio processing pipeline design for embedded Linux

**Not included:** The `bithion-core` SDK (provided as a prebuilt binary) and the server-side infrastructure. This is not a standalone, self-contained runtime — it requires the companion SDKs and target hardware.

## Third-Party Licenses

| Component | License |
|-----------|---------|
| [Opus](https://opus-codec.org/) 1.6.1 | BSD-3-Clause |
| [ALSA](https://alsa-project.org/) 1.1.5 | LGPL-2.1+ |
| [minimp3](https://github.com/lieff/minimp3) | CC0-1.0 |

Full license texts: [`third_party/licenses/`](third_party/licenses/)

## License

```
Copyright 2026 Shanghai ZiHai Technology Co., Ltd.

Licensed under the Apache License, Version 2.0.
See LICENSE for details.
```

