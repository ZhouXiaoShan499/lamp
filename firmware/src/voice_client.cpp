#include "voice_client.h"

#include "lighting.h"
#include "pdm_mic.h"

#if !LAMP_HAS_VOICE

namespace voice {
void begin() {}
void tick(uint32_t) {}
bool connected() { return false; }
bool listening() { return false; }
bool monitor() { return false; }
void setMonitor(bool) {}
void requestListen() {}
void sendText(const char*) {}
void printStatus() { Serial.println("[voice] disabled at compile time (LAMP_HAS_VOICE=0)"); }
}  // namespace voice

#elif !defined(LAMP_WIFI_SSID) || !defined(LAMP_WIFI_PASS)

namespace voice {
void begin() {
    Serial.println("[voice] no WiFi credentials: copy include/secrets.h.example to include/secrets.h");
}
void tick(uint32_t) {}
bool connected() { return false; }
bool listening() { return false; }
bool monitor() { return false; }
void setMonitor(bool) {}
void requestListen() {}
void sendText(const char*) {}
void printStatus() { Serial.println("[voice] disabled: LAMP_WIFI_SSID/LAMP_WIFI_PASS missing"); }
}  // namespace voice

#else

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

namespace voice {
namespace {

WebSocketsClient ws;

bool wsStarted = false;
bool wsUp = false;
bool monitorOn = false;
bool forceListen = false;

uint32_t micRetryAt = 0;
uint32_t lastMonitorPrint = 0;

// 自适应底噪（增益后的 RMS）。静音时慢慢跟住环境，说话时不被带跑。
float noiseFloor = 120.0f;
float lastThreshold = MIC_VAD_RMS;

enum class State : uint8_t { Idle, Recording, Waiting };
State state = State::Idle;

uint32_t captureStart = 0;
uint32_t lastVoiceAt = 0;
uint32_t waitStart = 0;
uint32_t audioFrames = 0;
uint32_t audioSamples = 0;

int16_t frame[MIC_FRAME_SAMPLES];
int16_t preroll[MIC_PREROLL_FRAMES][MIC_FRAME_SAMPLES];
uint16_t prerollSamples[MIC_PREROLL_FRAMES] = {0};  // valid samples per ring slot
uint8_t prerollCount = 0;
uint8_t prerollHead = 0;  // next write position

// 整段录音缓冲：触发后先录满 2 秒，再一次性发出去（边录边发会被弱网拖死）。
int16_t uttBuf[MIC_RECORD_SAMPLES];
size_t uttSamples = 0;

const char* stateName() {
    switch (state) {
        case State::Idle: return "idle";
        case State::Recording: return "recording";
        case State::Waiting: return "waiting";
    }
    return "?";
}

void printLevel(float level) {
    int cells = static_cast<int>(level / 60.0f);
    if (cells < 0) cells = 0;
    if (cells > 40) cells = 40;
    char bar[41];
    for (int i = 0; i < 40; ++i) bar[i] = (i < cells) ? '#' : '.';
    bar[40] = '\0';
    Serial.printf("[mic] rms=%6.0f floor=%4.0f thr=%4.0f |%s| %s\n", level, noiseFloor, lastThreshold, bar,
                  level >= lastThreshold ? "SPEECH" : (level > noiseFloor * 2.0f ? "noise" : "quiet"));
}

void rememberPreroll(size_t count) {
    if (count > MIC_FRAME_SAMPLES) count = MIC_FRAME_SAMPLES;
    memcpy(preroll[prerollHead], frame, count * sizeof(int16_t));
    prerollSamples[prerollHead] = static_cast<uint16_t>(count);
    prerollHead = static_cast<uint8_t>((prerollHead + 1) % MIC_PREROLL_FRAMES);
    if (prerollCount < MIC_PREROLL_FRAMES) ++prerollCount;
}

// 把 preroll 环形缓冲里的内容拷进整段录音缓冲（不再边录边发）。
void copyPreroll() {
    const uint8_t count = prerollCount;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t index =
            static_cast<uint8_t>((prerollHead + MIC_PREROLL_FRAMES - count + i) % MIC_PREROLL_FRAMES);
        const size_t samples = prerollSamples[index];
        if (samples == 0) continue;
        if (uttSamples + samples > MIC_RECORD_SAMPLES) break;
        memcpy(uttBuf + uttSamples, preroll[index], samples * sizeof(int16_t));
        uttSamples += samples;
    }
    prerollCount = 0;
}

// 录满一段后一次性分块发出去。之前边录边发，WS 发送一阻塞就把主循环拖死
// （实测 10 秒只发出 0.16 秒音频，还会触发看门狗复位）。
void sendUtterance(uint32_t now) {
    if (!wsUp) {
        state = State::Idle;
        uttSamples = 0;
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"audio_start\",\"sample_rate\":%u,\"format\":\"s16le\",\"channels\":1}",
             static_cast<unsigned>(MIC_SAMPLE_RATE));
    ws.sendTXT(msg);

    const size_t kChunk = 2048;  // 4 KB 一块，避免单次 send 过大
    for (size_t off = 0; off < uttSamples; off += kChunk) {
        const size_t n = (uttSamples - off < kChunk) ? (uttSamples - off) : kChunk;
        ws.sendBIN(reinterpret_cast<uint8_t*>(uttBuf + off), n * sizeof(int16_t));
        delay(2);  // 给底层 TCP 一点排空时间
    }

    snprintf(msg, sizeof(msg), "{\"type\":\"audio_end\",\"samples\":%lu,\"reason\":\"fixed\"}",
             static_cast<unsigned long>(uttSamples));
    ws.sendTXT(msg);
    Serial.printf("[voice] sent %u samples (%.2f s), waiting for ASR...\n", static_cast<unsigned>(uttSamples),
                  static_cast<float>(uttSamples) / static_cast<float>(MIC_SAMPLE_RATE));
    state = State::Waiting;
    waitStart = now;
    uttSamples = 0;
}

// 服务端把"本地模型想做的动作"翻译成一条 cmd JSON，这里落地成真实的灯光操作。
// 没有这条通道时，模型只能回一句"好的，开灯"，灯并不会亮。
void applyCommand(JsonDocument& doc) {
    const char* light = doc["light"] | "";
    const char* preset = doc["preset"] | "";
    const int bri = doc["brightness"] | -1;
    const int cct = doc["cct"] | -1;

    bool changed = false;
    if (strcmp(light, "off") == 0) {
        lighting::setBrightness(0);
        changed = true;
    } else if (strcmp(light, "on") == 0) {
        // 已经亮着就保持当前亮度，别把用户刚调好的光又改掉
        const uint8_t target = (bri >= 0) ? static_cast<uint8_t>(constrain(bri, 1, 100))
                                          : (lighting::isOn() ? lighting::brightness() : 70);
        lighting::setBrightness(target);
        changed = true;
    } else if (strcmp(light, "toggle") == 0) {
        lighting::toggle();
        changed = true;
    }

    if (bri >= 0) {
        lighting::setBrightness(static_cast<uint8_t>(constrain(bri, 0, 100)));
        changed = true;
    }
    if (cct > 0) {
        lighting::setCct(static_cast<uint16_t>(cct));
        changed = true;
    }
    if (preset[0] != '\0') {
        lighting::Preset chosen = lighting::Preset::Study;
        if (strcmp(preset, "office") == 0) {
            chosen = lighting::Preset::Office;
        } else if (strcmp(preset, "rest") == 0) {
            chosen = lighting::Preset::Rest;
        } else if (strcmp(preset, "night") == 0) {
            chosen = lighting::Preset::Night;
        }
        lighting::setPreset(chosen);
        changed = true;
    }

    if (!changed) return;
    Serial.printf("[cmd] light=%s bri=%d cct=%d preset=%s -> on=%d pct=%u\n", light[0] ? light : "-", bri,
                  cct, preset[0] ? preset : "-", lighting::isOn() ? 1 : 0, lighting::brightness());
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsUp = true;
            wsStarted = true;
            Serial.printf("[voice] WebSocket connected -> ws://%s:%d/\n", LAMP_VOICE_SERVER_HOST,
                          LAMP_VOICE_SERVER_PORT);
            break;

        case WStype_DISCONNECTED:
            if (wsUp) Serial.println("[voice] WebSocket disconnected, retrying every 3 s");
            wsUp = false;
            if (state != State::Idle) {
                state = State::Idle;
                Serial.println("[voice] utterance aborted (link lost)");
            }
            break;

        case WStype_TEXT: {
            JsonDocument doc;
            const DeserializationError err = deserializeJson(doc, payload, length);
            if (err) {
                Serial.printf("[voice] bad reply json: %s\n", err.c_str());
                break;
            }
            const char* kind = doc["type"] | "";
            if (strcmp(kind, "asr") == 0) {
                const char* text = doc["text"] | "";
                Serial.printf("\n[ASR] heard: %s\n", text[0] != '\0' ? text : "(empty)");
            } else if (strcmp(kind, "cmd") == 0) {
                // 动作先到、文字后到，用户看到的是"灯已经亮了 + 一句说明"
                applyCommand(doc);
            } else if (strcmp(kind, "reply") == 0) {
                const char* text = doc["text"] | "";
                const char* asr = doc["asr"] | "";
                const long elapsed = doc["elapsed_ms"] | 0L;
                Serial.println();
                Serial.println("========== 本地模型 ==========");
                if (asr[0] != '\0') Serial.printf("you : %s\n", asr);
                Serial.printf("ai  : %s\n", text);
                Serial.printf("took: %ld ms\n", elapsed);
                Serial.println("==============================");
                Serial.println();
                state = State::Idle;
            } else if (strcmp(kind, "error") == 0) {
                const char* message = doc["message"] | "unknown";
                Serial.printf("[voice] server error: %s\n", message);
                state = State::Idle;
            } else if (strcmp(kind, "ready") == 0) {
                const char* model = doc["model"] | "?";
                Serial.printf("[voice] server ready (model=%s)\n", model);
            }
            break;
        }

        case WStype_ERROR:
            Serial.println("[voice] websocket error");
            break;

        default:
            break;
    }
}

void startUtterance(uint32_t now, float level) {
    uttSamples = 0;
    copyPreroll();
    state = State::Recording;
    captureStart = now;
    lastVoiceAt = now;
    Serial.printf("[voice] recording (rms=%.0f, preroll=%u)\n", level, static_cast<unsigned>(uttSamples));
}

}  // namespace

void begin() {
    Serial.printf("[voice] target ws://%s:%d/ (16 kHz mono s16le)\n", LAMP_VOICE_SERVER_HOST,
                  LAMP_VOICE_SERVER_PORT);
    if (!pdmmic::begin()) {
        Serial.println("[mic] open failed, retrying every 5 s");
        micRetryAt = millis();
    }
}

void tick(uint32_t now) {
    if (WiFi.status() == WL_CONNECTED) {
        if (!wsStarted) {
            // Register the handler *before* begin(): begin() may connect at once.
            ws.onEvent(onWsEvent);
            ws.setReconnectInterval(3000);
            ws.enableHeartbeat(15000, 3000, 2);
            ws.begin(LAMP_VOICE_SERVER_HOST, LAMP_VOICE_SERVER_PORT, "/");
            wsStarted = true;
            Serial.printf("[voice] connecting to ws://%s:%d/\n", LAMP_VOICE_SERVER_HOST,
                          LAMP_VOICE_SERVER_PORT);
        }
        ws.loop();
    } else if (wsStarted) {
        // No link: drop the socket explicitly, it is re-created on reconnect.
        ws.disconnect();
        wsStarted = false;
        wsUp = false;
    }

    if (!pdmmic::ready()) {
        if ((now - micRetryAt) >= MIC_RETRY_MS) {
            micRetryAt = now;
            pdmmic::begin();
        }
        if (!pdmmic::ready()) return;
    }

    // 512 samples = 32 ms of audio. The timeout must be longer than one frame,
    // otherwise every steady-state read times out and the frame cadence stutters.
    const size_t got = pdmmic::read(frame, MIC_FRAME_SAMPLES, 60);
    if (got == 0) return;
    const float level = pdmmic::rms(frame, got);

    if (monitorOn && (now - lastMonitorPrint) >= MIC_MONITOR_PERIOD_MS) {
        lastMonitorPrint = now;
        printLevel(level);
    }

    switch (state) {
        case State::Idle: {
            rememberPreroll(got);
            // 底噪缓慢跟住环境（只在 idle 更新，说话时不被自己带跑），
            // 阈值取 max(底噪 × 3, MIC_VAD_RMS)，安静房间和吵闹房间都能用。
            noiseFloor = noiseFloor * 0.98f + level * 0.02f;
            const float dynamic = noiseFloor * 3.0f;
            lastThreshold = (dynamic > MIC_VAD_RMS) ? dynamic : MIC_VAD_RMS;
            const bool trigger = forceListen || level >= lastThreshold;
            if (!trigger) break;
            forceListen = false;
            if (!wsUp) {
                Serial.println("[voice] speech detected but WebSocket is down, dropped");
                break;
            }
            Serial.printf("[voice] trigger rms=%.0f floor=%.0f thr=%.0f\n", level, noiseFloor, lastThreshold);
            startUtterance(now, level);
            break;
        }

        case State::Recording: {
            const size_t room = MIC_RECORD_SAMPLES - uttSamples;
            const size_t n = (got < room) ? got : room;
            if (n > 0) {
                memcpy(uttBuf + uttSamples, frame, n * sizeof(int16_t));
                uttSamples += n;
            }
            // 固定录满 2 秒再发；不依赖静音判断，弱网下也不会卡在录音状态。
            if (uttSamples >= MIC_RECORD_SAMPLES || (now - captureStart) >= MIC_RECORD_MS) {
                sendUtterance(now);
            }
            break;
        }

        case State::Waiting:
            if ((now - waitStart) >= MIC_WAIT_REPLY_MS) {
                Serial.println("[voice] reply timeout, back to idle");
                state = State::Idle;
            }
            break;
    }
}

bool connected() { return wsUp; }
bool listening() { return state == State::Recording; }
bool monitor() { return monitorOn; }

void setMonitor(bool on) {
    monitorOn = on;
    Serial.printf("[mic] monitor %s (VAD threshold %.0f)\n", on ? "on" : "off", MIC_VAD_RMS);
}

void requestListen() {
    if (!pdmmic::ready()) pdmmic::begin();
    if (!pdmmic::ready()) {
        Serial.println("[mic] not ready");
        return;
    }
    forceListen = true;
    Serial.println("[voice] armed: speak now (up to 8 s)");
}

void sendText(const char* text) {
    if (text == nullptr || text[0] == '\0') return;
    if (!wsUp) {
        Serial.println("[voice] not connected yet, wait for 'WebSocket connected'");
        return;
    }
    JsonDocument doc;
    doc["type"] = "text";
    doc["content"] = text;
    String out;
    serializeJson(doc, out);
    ws.sendTXT(out);
    Serial.printf("[voice] -> %s\n", text);
}

void printStatus() {
    const bool wifiUp = WiFi.status() == WL_CONNECTED;
    Serial.printf("[voice] wifi=%s ip=%s ws=%s mic=%s state=%s monitor=%d\n",
                  wifiUp ? "up" : "down",
                  wifiUp ? WiFi.localIP().toString().c_str() : "-",
                  wsUp ? "up" : "down", pdmmic::ready() ? "ready" : "missing", stateName(),
                  monitorOn ? 1 : 0);
    Serial.printf("[voice] target=ws://%s:%d threshold=%.0f silence=%lums\n",
                  LAMP_VOICE_SERVER_HOST, LAMP_VOICE_SERVER_PORT, MIC_VAD_RMS,
                  static_cast<unsigned long>(MIC_SILENCE_MS));
}

}  // namespace voice

#endif  // LAMP_HAS_VOICE