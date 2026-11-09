/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Davidian. See LICENSE and NOTICE.md. */
/* Davidian Humble Overdraft: LV2 plugin entry points. The circuit model is in amp.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "amp.h"
#include "lv2_abi.h"
#include "oversample.h"
#include "ports.h"

/* Flush denormals to zero while processing, so decaying filter states can't stall the CPU. */
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#    include <xmmintrin.h>
typedef unsigned FpState;
static FpState fp_enter(void)
{
    const FpState s = _mm_getcsr();
    _mm_setcsr(s | 0x8040); /* FTZ | DAZ */
    return s;
}
static void fp_leave(FpState s) { _mm_setcsr(s); }
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
typedef unsigned long long FpState;
static FpState fp_enter(void)
{
    FpState s;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(s));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(s | (1ULL << 24))); /* FZ */
    return s;
}
static void fp_leave(FpState s) { __asm__ __volatile__("msr fpcr, %0" : : "r"(s)); }
#else
typedef int FpState;
static FpState fp_enter(void) { return 0; }
static void fp_leave(FpState s) { (void)s; }
#endif

/* Speaker volts that map to 0 dBFS: about full power into 8 ohms (~27 V peak). */
#define SPEAKER_REF 25.0
#define CTRL_BLOCK 32

typedef struct {
    double b0, b1, b2, a1, a2, z1, z2;
} Biquad;

static double bq_run(Biquad* q, double x)
{
    const double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* RBJ cookbook filters. kind: 0 low-pass, 1 high-pass, 2 peak. */
static void bq_design(Biquad* q, int kind, double f, double qf, double db, double fs)
{
    const double w = 2.0 * 3.14159265358979 * f / fs, cw = cos(w), al = sin(w) / (2.0 * qf);
    const double A = pow(10.0, db / 40.0);
    double b0, b1, b2, a0, a1, a2;
    if (kind == 0) {
        b0 = b2 = (1.0 - cw) / 2.0; b1 = 1.0 - cw;
        a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al;
    } else if (kind == 1) {
        b0 = b2 = (1.0 + cw) / 2.0; b1 = -(1.0 + cw);
        a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al;
    } else {
        b0 = 1.0 + al * A; b1 = -2.0 * cw; b2 = 1.0 - al * A;
        a0 = 1.0 + al / A; a1 = -2.0 * cw; a2 = 1.0 - al / A;
    }
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0;
    q->a1 = a1 / a0; q->a2 = a2 / a0;
    q->z1 = q->z2 = 0.0;
}

/* Speaker emulation for direct recording: a generic closed-back 12" cabinet curve.
 * Not part of the schematic; switch to LINE to hear the speaker terminal. */
#define N_CAB 5
static void cab_design(Biquad* c, double fs)
{
    bq_design(&c[0], 1, 75.0, 0.707, 0.0, fs);
    bq_design(&c[1], 2, 110.0, 1.4, 3.0, fs);
    bq_design(&c[2], 2, 2200.0, 1.2, 3.0, fs);
    bq_design(&c[3], 0, 5000.0, 0.9, 0.0, fs);
    bq_design(&c[4], 0, 7500.0, 0.6, 0.0, fs);
}

typedef struct {
    float* port[PORT_COUNT];
    double rate;
    int os;
    Amp amp;
    Oversampler ovs;
    AmpControls ctl;
    int ctl_valid;
    Biquad cab[N_CAB];
    double cab_mix;    /* 0 line, 1 cabinet; ramps */
    double dry[OS_LATENCY + 1];
    int dry_pos, latency;
    double wet;        /* 1 enabled, 0 bypassed; ramps */
    double gain;       /* smoothed output gain */
    double ramp;       /* per-sample step for the ramps */
    double smooth;     /* per-block smoothing coefficient for the knobs */
} Plugin;

static float port_or(const Plugin* p, int i, float def)
{
    const float v = p->port[i] ? *p->port[i] : def;
    return isfinite(v) ? v : def;
}

static void read_targets(const Plugin* p, AmpControls* k)
{
    k->volume = port_or(p, PORT_VOLUME, 0.5f);
    k->treble = port_or(p, PORT_TREBLE, 0.5f);
    k->middle = port_or(p, PORT_MIDDLE, 0.5f);
    k->bass = port_or(p, PORT_BASS, 0.5f);
    k->master = port_or(p, PORT_MASTER, 0.5f);
    k->drive = port_or(p, PORT_DRIVE, 0.6f);
    k->od_treble = port_or(p, PORT_OD_TREBLE, 0.5f);
    k->od_middle = port_or(p, PORT_OD_MIDDLE, 0.5f);
    k->od_bass = port_or(p, PORT_OD_BASS, 0.5f);
    k->od_level = port_or(p, PORT_OD_LEVEL, 0.35f);
    k->od_trim = port_or(p, PORT_OD_TRIM, 0.5f);
    k->presence = port_or(p, PORT_PRESENCE, 0.3f);
    k->overdrive = port_or(p, PORT_OVERDRIVE, 0.0f) > 0.5f;
}

/* Moves the knobs toward their port values; returns 1 if anything changed. */
static int smooth_controls(Plugin* p)
{
    AmpControls t;
    double* cur = &p->ctl.volume;
    const double* tgt = &t.volume;
    const int n = (int)((&p->ctl.presence - &p->ctl.volume) + 1);
    int i, changed = 0;
    read_targets(p, &t);
    if (!p->ctl_valid) {
        p->ctl = t;
        p->ctl_valid = 1;
        return 1;
    }
    for (i = 0; i < n; ++i) {
        const double d = tgt[i] - cur[i];
        if (fabs(d) > 1e-5) {
            cur[i] += fabs(d) < 1e-3 ? d : d * p->smooth;
            changed = 1;
        } else if (d != 0.0) {
            cur[i] = tgt[i];
            changed = 1;
        }
    }
    if (t.overdrive != p->ctl.overdrive) {
        p->ctl.overdrive = t.overdrive;
        changed = 1;
    }
    return changed;
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle,
                              const LV2_Feature* const* features)
{
    Plugin* p = (Plugin*)calloc(1, sizeof(Plugin));
    (void)d;
    (void)bundle;
    (void)features;
    if (!p)
        return NULL;
    p->rate = rate;
    p->os = rate <= 50000.0 ? 4 : (rate <= 100000.0 ? 2 : 1);
    p->latency = p->os > 1 ? OS_LATENCY : 0;
    p->ramp = 1.0 / (0.01 * rate);
    p->smooth = 1.0 - exp(-CTRL_BLOCK / (0.02 * rate));
    return p;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
    Plugin* p = (Plugin*)h;
    if (port < PORT_COUNT)
        p->port[port] = (float*)data;
}

static void activate(LV2_Handle h)
{
    Plugin* p = (Plugin*)h;
    p->ctl_valid = 0;
    smooth_controls(p);
    amp_init(&p->amp, p->rate * p->os, &p->ctl);
    os_init(&p->ovs, p->os);
    cab_design(p->cab, p->rate);
    p->cab_mix = port_or(p, PORT_CAB, 1.0f) > 0.5f ? 1.0 : 0.0;
    memset(p->dry, 0, sizeof p->dry);
    p->dry_pos = 0;
    p->wet = port_or(p, PORT_ENABLED, 1.0f) > 0.5f ? 1.0 : 0.0;
    p->gain = pow(10.0, port_or(p, PORT_OUTPUT, 0.0f) / 20.0);
}

static void run(LV2_Handle h, uint32_t n)
{
    Plugin* p = (Plugin*)h;
    const float* in = p->port[PORT_IN];
    float* out = p->port[PORT_OUT];
    const double volts = port_or(p, PORT_VOLTS, 1.0f);
    const double gain_t = pow(10.0, port_or(p, PORT_OUTPUT, 0.0f) / 20.0);
    const double cab_t = port_or(p, PORT_CAB, 1.0f) > 0.5f ? 1.0 : 0.0;
    const double wet_t = port_or(p, PORT_ENABLED, 1.0f) > 0.5f ? 1.0 : 0.0;
    const double gain_a = 1.0 - exp(-1.0 / (0.02 * p->rate));
    double buf[OS_MAX];
    uint32_t i;
    int k;
    const FpState fp = fp_enter();
    if (p->port[PORT_LATENCY])
        *p->port[PORT_LATENCY] = (float)p->latency;
    if (!in || !out)
        goto done;
    for (i = 0; i < n; ++i) {
        double x = in[i], y, dry, line;
        if (i % CTRL_BLOCK == 0 && smooth_controls(p))
            amp_set(&p->amp, &p->ctl);
        if (!isfinite(x))
            x = 0.0;
        os_up(&p->ovs, x * volts, buf);
        for (k = 0; k < p->os; ++k)
            buf[k] = amp_run(&p->amp, buf[k]) / SPEAKER_REF;
        line = os_down(&p->ovs, buf);
        y = line;
        for (k = 0; k < N_CAB; ++k)
            y = bq_run(&p->cab[k], y);
        p->cab_mix += cab_t > p->cab_mix ? p->ramp : (cab_t < p->cab_mix ? -p->ramp : 0.0);
        p->cab_mix = clamp01(p->cab_mix);
        y = line + p->cab_mix * (y - line);
        p->gain += gain_a * (gain_t - p->gain);
        y *= p->gain;
        /* Bypass is delayed by the same latency, so switching does not jump in time. */
        p->dry[p->dry_pos] = x;
        dry = p->dry[(p->dry_pos + OS_LATENCY + 1 - p->latency) % (OS_LATENCY + 1)];
        p->dry_pos = (p->dry_pos + 1) % (OS_LATENCY + 1);
        p->wet += wet_t > p->wet ? p->ramp : (wet_t < p->wet ? -p->ramp : 0.0);
        p->wet = clamp01(p->wet);
        y = dry + p->wet * (y - dry);
        if (!isfinite(y)) { /* never pass garbage to the host; restart the model */
            y = 0.0;
            amp_init(&p->amp, p->rate * p->os, &p->ctl);
        }
        out[i] = (float)y;
    }
done:
    fp_leave(fp);
}

static void deactivate(LV2_Handle h) { (void)h; }

static void cleanup(LV2_Handle h) { free(h); }

static const void* extension_data(const char* uri)
{
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    HO_URI, instantiate, connect_port, activate, run, deactivate, cleanup, extension_data
};

LV2_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
