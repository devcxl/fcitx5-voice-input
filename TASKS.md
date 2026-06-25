# DAG 任务拆解

```
                        ┌──────────────┐
                        │  1. repo +   │
                        │  CMakeLists  │
                        └──────┬───────┘
                               │
              ┌────────────────┼────────────────┐
              ▼                ▼                ▼
       ┌──────────────┐ ┌──────────┐ ┌──────────────┐
       │ 2. utils/    │ │ 3. conf  │ │ 4. capture/  │
       │ ring_buffer  │ │ .conf    │ │ PipeWire     │
       │ thread_queue │ │ Config.h │ │ (no VAD)     │
       └──────┬───────┘ └────┬─────┘ └──────┬───────┘
              │              │              │
              │              │              ▼
              │              │       ┌──────────────┐
              │              │       │ 5. vad/      │
              │              │       └──────┬───────┘
              └──────┬───────┼──────────────┘
                     │       │
                     ▼       ▼
              ┌──────────────────┐
              │ 6. pipeline/     │
              │ 状态机 + VAD +   │
              │ ASR 调用         │
              └────────┬─────────┘
                       │
                       ▼
              ┌──────────────────┐
              │ 7. asr/          │
              │ sherpa-onnx      │
              │ command_engine   │
              └────────┬─────────┘
                       │
                       ▼
              ┌──────────────────┐
              │ 8. engine/       │
              │ VoiceInputEngine │
              │ keyEvent + 上屏  │
              └────────┬─────────┘
                       │
                       ▼
              ┌──────────────────┐
              │ 9. final integ   │
              │ build verify     │
              └──────────────────┘
```

## 执行顺序

| # | Task | 产出 | Deps |
|---|------|------|------|
| 1 | repo + CMake | `gh repo create`, CMakeLists.txt, cmake/ | - |
| 2 | utils | ring_buffer.h, thread_safe_queue.h | 1 |
| 3 | config | Config.h/cpp, voice-input-addon.conf | 1 |
| 4 | capture | pipewire_capture.h/cpp (PCM→ring buffer only) | 1, 2 |
| 5 | vad | vad.h/cpp (webrtcvad-like) | 1, 2 |
| 6 | asr | asr_engine.h, sherpa_asr.h/cpp | 1 |
| 7 | pipeline | pipeline.h/cpp (state machine) | 3, 4, 5, 6 |
| 8 | engine | engine.h/cpp (VoiceInputEngine) | 3, 7 |
| 9 | final | CMake compat, build verify | 8 |
