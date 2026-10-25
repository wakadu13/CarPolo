// Real-time synthesis in an AudioStream callback (mono float, 44.1 kHz)
#include "audio.h"
#include "raylib.h"
#include <math.h>
#include <string.h>

#define SR 44100.0f

static AudioStream stream;
static bool ready, enabled = true, paused;

// parameters written by the game thread
static volatile float pRpm = 850, pThrottle, pSpeed, pSkid, pOff, pRun;
static volatile int trig[SFX_COUNT];

// synth state (audio thread)
static float ph1, ph2, ph3, lp1, lp2, bp1, bp2, wl, rumble, engVol, skidVol, offVol, rpmS = 850;
static unsigned noiseS = 22222;
typedef struct { float t, dur; bool on; } Voice;
static Voice v[SFX_COUNT];

static inline float noise(void)
{
    noiseS = noiseS * 1664525u + 1013904223u;
    return ((noiseS >> 9) & 0xFFFF) / 32768.0f - 1.0f;
}

static float sfxSample(int k, float t)
{
    switch (k) {
    case SFX_CRASH: {
        float e = expf(-t * 6.0f);
        return (noise() * 0.9f * e + sinf(2 * PI * 58 * t) * expf(-t * 9) * 0.9f
                + (sinf(2 * PI * 930 * t) + sinf(2 * PI * 1370 * t)) * 0.18f * expf(-t * 10)) * 0.9f;
    }
    case SFX_SCRAPE: return noise() * 0.35f * expf(-t * 5) * (0.6f + 0.4f * sinf(2 * PI * 37 * t));
    case SFX_HORN:
    case SFX_HONK_OTHER: {
        float f1 = k == SFX_HORN ? 415 : 370, f2 = k == SFX_HORN ? 523 : 466;
        float s = (sinf(2 * PI * f1 * t) > 0 ? 1.f : -1.f) * 0.5f + (sinf(2 * PI * f2 * t) > 0 ? 1.f : -1.f) * 0.5f;
        float env = fminf(1, t * 60) * fminf(1, (v[k].dur - t) * 30);
        return s * env * (k == SFX_HORN ? 0.22f : 0.12f);
    }
    case SFX_BEEP: return sinf(2 * PI * 880 * t) * 0.35f * fminf(1, (v[k].dur - t) * 40);
    case SFX_GO: return sinf(2 * PI * 1320 * t) * 0.35f * fminf(1, (v[k].dur - t) * 20);
    case SFX_FINISH: {
        static const float notes[5] = { 523.25f, 659.25f, 783.99f, 1046.5f, 1046.5f };
        int n = (int)(t / 0.16f);
        if (n > 4) n = 4;
        float lt = t - n * 0.16f;
        float f = notes[n];
        return (sinf(2 * PI * f * t) * 0.6f + sinf(2 * PI * f * 2 * t) * 0.2f) * expf(-lt * (n == 4 ? 2.5f : 8.0f)) * 0.4f;
    }
    case SFX_CLICK: return noise() * expf(-t * 300) * 0.4f;
    case SFX_WHOOSH: { float e = sinf(PI * fminf(1, t / v[k].dur)); return noise() * e * 0.18f; }
    default: return 0;
    }
}

static const float sfxDur[SFX_COUNT] = { 1.2f, 0.55f, 0.5f, 0.12f, 0.45f, 1.3f, 0.03f, 0.5f, 0.4f };

static void callback(void *buffer, unsigned int frames)
{
    float *out = (float *)buffer;
    for (int k = 0; k < SFX_COUNT; k++) {
        if (trig[k]) { trig[k] = 0; v[k].on = true; v[k].t = 0; v[k].dur = sfxDur[k]; }
    }
    float tRpm = pRpm, thr = pThrottle, spd = pSpeed, skid = pSkid, off = pOff;
    float run = (enabled && !paused) ? pRun : 0;
    for (unsigned int i = 0; i < frames; i++) {
        rpmS += (tRpm - rpmS) * 0.0008f;
        float f0 = rpmS / 60.0f * 2.0f;           // 4-cylinder firing frequency
        ph1 += f0 / SR; if (ph1 >= 1) ph1 -= 1;
        ph2 += f0 * 0.5f / SR; if (ph2 >= 1) ph2 -= 1;
        ph3 += f0 * 4.5f / SR; if (ph3 >= 1) ph3 -= 1;
        // combustion pulses: sharp decaying spike each firing
        float pulse = expf(-ph1 * 7.0f) * 2.0f - 0.55f;
        float sub = (ph2 < 0.5f ? 1.0f : -1.0f) * 0.35f;
        float raw = pulse * 0.6f + sub + noise() * (0.08f + thr * 0.12f);
        float cut = 0.06f + thr * 0.10f + rpmS / 6000.0f * 0.08f;
        lp1 += (raw - lp1) * cut;
        lp2 += (lp1 - lp2) * cut;
        float eng = lp2 + sinf(2 * PI * ph3) * 0.03f * thr;
        float targetVol = run * (0.20f + thr * 0.22f);
        engVol += (targetVol - engVol) * 0.0004f;
        float s = tanhf(eng * 1.6f) * engVol;

        // tyre screech: resonant band-pass noise ~1.9 kHz
        float tgtSkid = run * skid;
        skidVol += (tgtSkid - skidVol) * 0.0008f;
        if (skidVol > 0.001f) {
            float fc = 2 * sinf(PI * (1900.0f + 300.0f * sinf(ph2 * 6.28f)) / SR);
            float in = noise();
            bp1 += fc * bp2; bp2 += fc * (in - bp1 - 0.08f * bp2);
            s += bp1 * 0.10f * skidVol;
        }
        // rumble off-road and wind with speed
        offVol += (run * off - offVol) * 0.001f;
        rumble += (noise() - rumble) * 0.02f;
        s += rumble * offVol * 0.9f;
        float wind = run * fminf(1, spd / 45.0f);
        wl += (noise() - wl) * 0.05f;
        s += wl * wind * wind * 0.16f;

        // effects
        if (enabled) {
            for (int k = 0; k < SFX_COUNT; k++) {
                if (!v[k].on) continue;
                s += sfxSample(k, v[k].t);
                v[k].t += 1.0f / SR;
                if (v[k].t >= v[k].dur) v[k].on = false;
            }
        }
        out[i] = fmaxf(-1.0f, fminf(1.0f, s));
    }
}

void AudioInit(void)
{
    InitAudioDevice();
    if (!IsAudioDeviceReady()) return;
    SetAudioStreamBufferSizeDefault(1024);
    stream = LoadAudioStream(44100, 32, 1);
    SetAudioStreamCallback(stream, callback);
    PlayAudioStream(stream);
    ready = true;
}

void AudioShutdown(void)
{
    if (!ready) return;
    StopAudioStream(stream);
    UnloadAudioStream(stream);
    CloseAudioDevice();
    ready = false;
}

void AudioEngine(float rpm, float throttle, float speedMs, bool running)
{
    pRpm = rpm; pThrottle = throttle; pSpeed = fabsf(speedMs); pRun = running ? 1.0f : 0.0f;
}

void AudioTyres(float skid, float offroad) { pSkid = skid; pOff = offroad; }
void AudioPlay(int sfx) { if (sfx >= 0 && sfx < SFX_COUNT && enabled) trig[sfx] = 1; }
void AudioSetEnabled(bool on) { enabled = on; }
bool AudioEnabled(void) { return enabled; }
void AudioPause(bool p)
{
    paused = p;
    if (!ready) return;
    if (p) PauseAudioStream(stream); else ResumeAudioStream(stream);
}
