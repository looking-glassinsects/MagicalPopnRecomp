#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string.h>
#include <math.h>
#include "audio.h"

static const GUID A_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID A_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID A_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID A_IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

#define OUT_RATE 48000
#define RING 16384                       /* frames, power of two */
#define TARGET_FILL 1100                 /* ~23 ms queued in the ring (+ the 20 ms device buffer) */

static IMMDeviceEnumerator *g_enum;
static IMMDevice *g_dev;
static IAudioClient *g_client;
static IAudioRenderClient *g_render;
static HANDLE g_event, g_thread;
static volatile LONG g_run;
static UINT32 g_buf_frames;
static volatile LONG g_underruns, g_fill_min = 1 << 30, g_fill_max, g_callbacks;
static volatile LONG g_lost;             /* the output device went away (unplugged, disabled): reopen */
static int g_want;                       /* audio_init was called: keep trying to have sound */
static DWORD g_retry_tick;

static int16_t g_ring[RING * 2];
static volatile LONG g_w, g_r;           /* write/read positions (frames), wrap at 2^31 */

/* resampler state */
static double g_pos;                     /* fractional position between hist[1] and hist[2] */
static float g_hist[4][2];

static int ring_fill(void) { return (int)(g_w - g_r); }

static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int fails = 0;
    while (g_run) {
        WaitForSingleObject(g_event, 100);
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(g_client, &pad))) {
            /* headphones unplugged, device disabled, ...: this stream is dead for good */
            if (++fails > 20) { InterlockedExchange(&g_lost, 1); break; }
            continue;
        }
        fails = 0;
        UINT32 n = g_buf_frames - pad;
        if (!n) continue;
        BYTE *data;
        if (FAILED(IAudioRenderClient_GetBuffer(g_render, n, &data))) continue;
        int16_t *out = (int16_t *)data;
        LONG r = g_r, avail = g_w - r;
        g_callbacks++;
        if (avail < g_fill_min) g_fill_min = avail;
        if (avail > g_fill_max) g_fill_max = avail;
        if ((UINT32)avail < n) g_underruns++;
        UINT32 i = 0;
        for (; i < n && avail > 0; i++, avail--, r++) {
            out[i * 2] = g_ring[(r & (RING - 1)) * 2];
            out[i * 2 + 1] = g_ring[(r & (RING - 1)) * 2 + 1];
        }
        /* underrun: fade the last sample out instead of clicking */
        int16_t l = i ? out[(i - 1) * 2] : 0, rr = i ? out[(i - 1) * 2 + 1] : 0;
        for (UINT32 k = 0; i < n; i++, k++) {
            float f = k < 64 ? 1.0f - k / 64.0f : 0.0f;
            out[i * 2] = (int16_t)(l * f); out[i * 2 + 1] = (int16_t)(rr * f);
        }
        InterlockedExchange(&g_r, r);
        IAudioRenderClient_ReleaseBuffer(g_render, n, 0);
    }
    return 0;
}

static void audio_close(void);
static int audio_open(void) {
    if (FAILED(CoCreateInstance(&A_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &A_IID_IMMDeviceEnumerator, (void **)&g_enum))) return 0;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, eRender, eConsole, &g_dev))) return 0;
    if (FAILED(IMMDevice_Activate(g_dev, &A_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&g_client))) return 0;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = OUT_RATE; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = OUT_RATE * 4;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000 /* AUTOCONVERTPCM */ | 0x08000000 /* SRC_DEFAULT_QUALITY */;
    if (FAILED(IAudioClient_Initialize(g_client, AUDCLNT_SHAREMODE_SHARED, flags, 200000 /* 20 ms */, 0, &wf, NULL))) return 0;
    IAudioClient_GetBufferSize(g_client, &g_buf_frames);
    if (FAILED(IAudioClient_GetService(g_client, &A_IID_IAudioRenderClient, (void **)&g_render))) return 0;
    if (!g_event) g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_event || FAILED(IAudioClient_SetEventHandle(g_client, g_event))) return 0;
    InterlockedExchange(&g_w, g_r);
    InterlockedExchange(&g_lost, 0);
    g_run = 1;
    g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    if (!g_thread) { g_run = 0; return 0; }
    IAudioClient_Start(g_client);
    return 1;
}

static void audio_close(void) {
    if (g_thread) { g_run = 0; SetEvent(g_event); WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); g_thread = 0; }
    if (g_client) IAudioClient_Stop(g_client);
    if (g_render) IAudioRenderClient_Release(g_render);
    if (g_client) IAudioClient_Release(g_client);
    if (g_dev) IMMDevice_Release(g_dev);
    if (g_enum) IMMDeviceEnumerator_Release(g_enum);
    g_render = 0; g_client = 0; g_dev = 0; g_enum = 0;
}

int audio_init(void) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_want = 1; g_retry_tick = GetTickCount();
    if (audio_open()) return 1;
    audio_close();                       /* nothing half-open is left behind; audio_push retries */
    return 0;
}

void audio_shutdown(void) { g_want = 0; audio_close(); }

void audio_clear(void) { InterlockedExchange(&g_w, g_r); }

double audio_latency_ms(void) { return (ring_fill() + (double)g_buf_frames) * 1000.0 / OUT_RATE; }

static float hermite(float x0, float x1, float x2, float x3, float t) {
    float c1 = 0.5f * (x2 - x0), c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3, c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

void audio_push(const int16_t *in, int frames, double in_rate, int volume) {
    /* no sound device, or it went away: look again once a second (it may be plugged back in, or
     * Windows may have another default device by now) */
    if (g_want && (g_lost || !g_client)) {
        DWORD now = GetTickCount();
        if (now - g_retry_tick < 1000) return;
        g_retry_tick = now;
        audio_close();
        if (!audio_open()) { audio_close(); return; }
    }
    if (!g_client) return;
    /* dynamic rate control: nudge the step so the ring stays near TARGET_FILL */
    int fill = ring_fill();
    if (fill > TARGET_FILL * 3) return;      /* way ahead (e.g. after a hitch): skip this block cleanly */
    double err = (double)(TARGET_FILL - fill) / TARGET_FILL;
    if (err > 1) err = 1; if (err < -1) err = -1;
    double step = in_rate / OUT_RATE * (1.0 - 0.01 * err);   /* +-1% pitch at most, inaudible */
    float vol = volume / 100.0f;
    LONG w = g_w;
    for (int i = 0; i < frames; i++) {
        memmove(g_hist[0], g_hist[1], sizeof(float) * 6);
        g_hist[3][0] = in[i * 2] * vol; g_hist[3][1] = in[i * 2 + 1] * vol;
        while (g_pos < 1.0) {
            if (w - g_r >= RING - 1) { g_pos = 1.0; break; }     /* can't happen in practice (guard) */
            float t = (float)g_pos;
            float l = hermite(g_hist[0][0], g_hist[1][0], g_hist[2][0], g_hist[3][0], t);
            float r = hermite(g_hist[0][1], g_hist[1][1], g_hist[2][1], g_hist[3][1], t);
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            g_ring[(w & (RING - 1)) * 2] = (int16_t)l;
            g_ring[(w & (RING - 1)) * 2 + 1] = (int16_t)r;
            w++;
            g_pos += step;
        }
        g_pos -= 1.0;
    }
    InterlockedExchange(&g_w, w);
}

void audio_stats(long *underruns, long *fill_min, long *fill_max, long *callbacks) {
    *underruns = g_underruns; *fill_min = g_fill_min; *fill_max = g_fill_max; *callbacks = g_callbacks;
    g_underruns = 0; g_fill_min = 1 << 30; g_fill_max = 0; g_callbacks = 0;
}
