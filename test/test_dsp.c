/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Davidian. See LICENSE and NOTICE.md. */
/* Tests for the circuit model and the plugin. Usage: test_dsp
 * Compiles plugin.c in directly, so it tests the same code the bundle runs. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <time.h>

#include "../src/plugin.c"

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: ");                                                                    \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static const AmpControls DEFAULTS = { 0.5, 0.5, 0.5, 0.5, 0.5, 0.6, 0.5, 0.5, 0.5, 0.35, 0.5, 0.3, 0 };

/* ---- 1. Operating points against the voltages marked on the schematic ---- */

static void check_point(const char* name, double vp, double vk, double vp_sch, double vk_sch)
{
    printf("  %-22s plate %6.1f V (schematic %6.1f)   cathode %6.2f V (schematic %6.2f)\n", name, vp,
           vp_sch, vk, vk_sch);
    CHECK(fabs(vp - vp_sch) < 0.12 * vp_sch, "%s plate voltage off by more than 12%%", name);
    CHECK(fabs(vk - vk_sch) < 0.25 * vk_sch, "%s cathode voltage off by more than 25%%", name);
}

static void test_operating_points(void)
{
    static Amp a;
    printf("Operating points (Koren 12AX7A model vs schematic; 12AX7s vary by about 20%%):\n");
    amp_init(&a, 192000.0, &DEFAULTS);
    check_point("V1B (clean 1st)", a.v1b.vp_q, a.v1b.vk_q, 209.6, 1.716);
    /* The schematic marks 206 V on this plate, but its own 1.788 V across 3k3 (0.54 mA)
     * through 150k from 323 V puts the plate at 242 V. Compare with that. */
    check_point("V1A (clean 2nd)", a.v1a.vp_q, a.v1a.vk_q, B5_V - 150e3 * 1.788 / 3.3e3, 1.788);
    check_point("V2B (OD 1st)", a.v2a.vp_q, a.v2a.vk_q, 227.5, 2.03);
    check_point("V2B (OD 2nd)", a.v2b.vp_q, a.v2b.vk_q, 229.0, 2.03);
    {
        const double vk = (PI_RFB + PI_RTAIL + PI_RBIAS) * a.pi.iq;
        printf("  %-22s plates %5.1f / %5.1f V (schematic 303 / 317)   cathodes %5.1f V (schematic 60.9)\n",
               "V3 phase inverter", a.pi.vp1_q, a.pi.vp2_q, vk);
        CHECK(fabs(a.pi.vp1_q - 303.0) < 0.12 * 303.0 && fabs(a.pi.vp2_q - 317.0) < 0.12 * 317.0, "phase inverter plates");
        CHECK(fabs(vk - 60.9) < 0.2 * 60.9, "phase inverter cathodes");
    }
    printf("  %-22s bias %.1f V for %.0f mA idle per tube (estimate, not on schematic)\n", "EL34 pair",
           a.pw.vbias, EL34_IDLE * 1e3);
    CHECK(a.pw.vbias < -20.0 && a.pw.vbias > -60.0, "EL34 bias outside the usual range");
}

/* ---- 2. Networks: time-domain solver against an independent complex nodal solve ---- */

/* Complex nodal analysis of the same netlist: Y v = I with Y = 1/R + jwC. Written out
 * with a small complex type, since MSVC has no C99 complex arithmetic. */
typedef struct { double re, im; } Cx;
static Cx cx(double re, double im) { Cx z; z.re = re; z.im = im; return z; }
static Cx cx_add(Cx a, Cx b) { return cx(a.re + b.re, a.im + b.im); }
static Cx cx_sub(Cx a, Cx b) { return cx(a.re - b.re, a.im - b.im); }
static Cx cx_mul(Cx a, Cx b) { return cx(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re); }
static Cx cx_div(Cx a, Cx b)
{
    const double d = b.re * b.re + b.im * b.im;
    return cx((a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d);
}
static double cx_abs(Cx a) { return sqrt(a.re * a.re + a.im * a.im); }

static void cx_stamp(Cx Y[NET_MAX_NODES][NET_MAX_NODES + 1], int a, int b, Cx y)
{
    if (a) Y[a - 1][a - 1] = cx_add(Y[a - 1][a - 1], y);
    if (b) Y[b - 1][b - 1] = cx_add(Y[b - 1][b - 1], y);
    if (a && b) {
        Y[a - 1][b - 1] = cx_sub(Y[a - 1][b - 1], y);
        Y[b - 1][a - 1] = cx_sub(Y[b - 1][a - 1], y);
    }
}

static Cx net_ac(const Net* n, double f, int out_node)
{
    Cx Y[NET_MAX_NODES][NET_MAX_NODES + 1], v[NET_MAX_NODES];
    const int N = n->nodes;
    const double w = 2.0 * 3.14159265358979 * f;
    int i, j, k;
    memset(Y, 0, sizeof Y);
    for (i = 0; i < n->nr; ++i) cx_stamp(Y, n->res[i].a, n->res[i].b, cx(1.0 / n->res[i].r, 0.0));
    for (i = 0; i < n->nc; ++i) cx_stamp(Y, n->cap[i].a, n->cap[i].b, cx(0.0, w * n->cap[i].c));
    for (i = 0; i < n->ns; ++i) {
        cx_stamp(Y, n->src[i].node, 0, cx(1.0 / n->src[i].r, 0.0));
        if (i == 0) Y[n->src[i].node - 1][N].re += 1.0 / n->src[i].r; /* 1 V on source 0 */
    }
    for (k = 0; k < N; ++k) { /* Gaussian elimination with partial pivoting */
        int piv = k;
        for (i = k + 1; i < N; ++i)
            if (cx_abs(Y[i][k]) > cx_abs(Y[piv][k])) piv = i;
        for (j = 0; j <= N; ++j) { Cx t = Y[k][j]; Y[k][j] = Y[piv][j]; Y[piv][j] = t; }
        for (i = k + 1; i < N; ++i) {
            const Cx m = cx_div(Y[i][k], Y[k][k]);
            for (j = k; j <= N; ++j) Y[i][j] = cx_sub(Y[i][j], cx_mul(m, Y[k][j]));
        }
    }
    for (i = N - 1; i >= 0; --i) {
        Cx s = Y[i][N];
        for (j = i + 1; j < N; ++j) s = cx_sub(s, cx_mul(Y[i][j], v[j]));
        v[i] = cx_div(s, Y[i][i]);
    }
    return v[out_node - 1];
}

/* Gain of the time-domain solver at f, measured with a sine. */
static double net_td(Net* n, double f, int out_node)
{
    const double fs = n->fs;
    const int settle = (int)(fs * 0.5), len = (int)(fs * 0.2);
    double sc = 0.0, ss = 0.0;
    int i;
    net_clear(n);
    for (i = 0; i < settle + len; ++i) {
        double x = sin(2.0 * 3.14159265358979 * f * i / fs);
        net_step(n, &x);
        if (i >= settle) {
            sc += n->v[out_node] * cos(2.0 * 3.14159265358979 * f * i / fs);
            ss += n->v[out_node] * sin(2.0 * 3.14159265358979 * f * i / fs);
        }
    }
    return 2.0 * sqrt(sc * sc + ss * ss) / len;
}

static double db(double x) { return 20.0 * log10(x > 1e-30 ? x : 1e-30); }

static void test_networks(void)
{
    static Amp a;
    static const double F[] = { 50, 200, 1000, 3000, 8000 };
    struct { const char* name; Net* n; int node; } nets[5];
    int i, j;
    double worst = 0.0;
    printf("Networks: time-domain solver vs complex nodal solution, 192 kHz:\n");
    amp_init(&a, 192000.0, &DEFAULTS);
    {
        AmpControls k = DEFAULTS;
        k.overdrive = 1;
        amp_set(&a, &k);
    }
    nets[0].name = "clean stack->V1A"; nets[0].n = &a.clean; nets[0].node = CL_G;
    nets[1].name = "V1A->OD trim->V2"; nets[1].n = &a.mid; nets[1].node = MD_G;
    nets[2].name = "drive network";    nets[2].n = &a.drive; nets[2].node = DR_G;
    nets[3].name = "OD stack->level";  nets[3].n = &a.od; nets[3].node = OD_Z;
    nets[4].name = "feedback node";    nets[4].n = &a.fb; nets[4].node = FB_N;
    for (i = 0; i < 5; ++i) {
        printf("  %-18s", nets[i].name);
        for (j = 0; j < 5; ++j) {
            const double ac = db(cx_abs(net_ac(nets[i].n, F[j], nets[i].node)));
            const double td = db(net_td(nets[i].n, F[j], nets[i].node));
            if (fabs(ac - td) > worst) worst = fabs(ac - td);
            printf(" %5.0fHz %6.1f/%6.1f", F[j], ac, td);
        }
        printf("\n");
    }
    printf("  worst difference %.3f dB\n", worst);
    CHECK(worst < 0.1, "time-domain networks differ from the AC solution by %.3f dB", worst);
}

/* ---- 3. Tone controls move the response the right way ---- */

static double clean_stack_db(AmpControls k, double f)
{
    static Amp a;
    amp_init(&a, 192000.0, &k);
    return db(cx_abs(net_ac(&a.clean, f, CL_G)));
}

static double od_stack_db(AmpControls k, double f)
{
    static Amp a;
    amp_init(&a, 192000.0, &k);
    return db(cx_abs(net_ac(&a.od, f, OD_Z)));
}

static void test_tone_controls(void)
{
    AmpControls lo = DEFAULTS, hi = DEFAULTS;
    printf("Tone controls (stack response, dB, knob at 0 -> 10):\n");
#define SWEEP(field, fn, f)                                                                        \
    do {                                                                                           \
        double a0, a1;                                                                             \
        lo = DEFAULTS; hi = DEFAULTS; lo.field = 0.0; hi.field = 1.0;                             \
        a0 = fn(lo, f); a1 = fn(hi, f);                                                            \
        printf("  %-10s at %5d Hz: %6.1f -> %6.1f\n", #field, (int)(f), a0, a1);                   \
        CHECK(a1 > a0 + 1.0, #field " does not raise the response at %d Hz", (int)(f));            \
    } while (0)
    SWEEP(treble, clean_stack_db, 5000.0);
    SWEEP(bass, clean_stack_db, 80.0);
    SWEEP(middle, clean_stack_db, 600.0);
    SWEEP(volume, clean_stack_db, 1000.0);
    SWEEP(od_treble, od_stack_db, 5000.0);
    SWEEP(od_bass, od_stack_db, 80.0);
    SWEEP(od_middle, od_stack_db, 600.0);
    SWEEP(od_level, od_stack_db, 1000.0);
#undef SWEEP
}

/* ---- 4. The whole plugin ---- */

typedef struct {
    Plugin* p;
    float ctl[PORT_COUNT];
} Host;

static void host_open(Host* h, double rate)
{
    int i;
    h->p = (Plugin*)instantiate(&descriptor, rate, "", NULL);
    h->ctl[PORT_VOLUME] = 0.5f; h->ctl[PORT_TREBLE] = 0.5f; h->ctl[PORT_MIDDLE] = 0.5f;
    h->ctl[PORT_BASS] = 0.5f; h->ctl[PORT_MASTER] = 0.5f; h->ctl[PORT_DRIVE] = 0.6f;
    h->ctl[PORT_OD_TREBLE] = 0.5f; h->ctl[PORT_OD_MIDDLE] = 0.5f; h->ctl[PORT_OD_BASS] = 0.5f;
    h->ctl[PORT_OD_LEVEL] = 0.35f; h->ctl[PORT_OD_TRIM] = 0.5f; h->ctl[PORT_PRESENCE] = 0.3f;
    h->ctl[PORT_OVERDRIVE] = 0.0f; h->ctl[PORT_CAB] = 0.0f; h->ctl[PORT_VOLTS] = 1.0f;
    h->ctl[PORT_OUTPUT] = 0.0f; h->ctl[PORT_ENABLED] = 1.0f;
    for (i = PORT_VOLUME; i < PORT_COUNT; ++i)
        connect_port(h->p, (uint32_t)i, &h->ctl[i]);
}

/* Runs a sine and returns the output's fundamental amplitude, THD and peak. */
static void sine_run(Host* h, double rate, double f, double amp, double secs, double* fund, double* thd, double* peak)
{
    const int n = (int)(rate * secs), start = n / 2;
    float* in = (float*)malloc(sizeof(float) * n);
    float* out = (float*)malloc(sizeof(float) * n);
    double c[6] = { 0 }, s[6] = { 0 }, total = 0.0, harm = 0.0;
    int i, k;
    for (i = 0; i < n; ++i)
        in[i] = (float)(amp * sin(2.0 * 3.14159265358979 * f * i / rate));
    connect_port(h->p, PORT_IN, in);
    connect_port(h->p, PORT_OUT, out);
    activate(h->p);
    for (i = 0; i < n; i += 256) {
        connect_port(h->p, PORT_IN, in + i);
        connect_port(h->p, PORT_OUT, out + i);
        run(h->p, (uint32_t)(n - i < 256 ? n - i : 256));
    }
    *peak = 0.0;
    for (i = start; i < n; ++i) {
        const double y = out[i];
        if (!isfinite(y)) { *peak = 1e9; break; }
        if (fabs(y) > *peak) *peak = fabs(y);
        total += y * y;
        for (k = 1; k <= 5; ++k) {
            c[k] += y * cos(2.0 * 3.14159265358979 * f * k * i / rate);
            s[k] += y * sin(2.0 * 3.14159265358979 * f * k * i / rate);
        }
    }
    for (k = 1; k <= 5; ++k) {
        const double a2 = 4.0 * (c[k] * c[k] + s[k] * s[k]) / ((double)(n - start) * (n - start));
        if (k == 1) *fund = sqrt(a2); else harm += a2;
    }
    *thd = *fund > 0.0 ? sqrt(harm) / *fund : 0.0;
    (void)total;
    free(in);
    free(out);
}

static void test_plugin(void)
{
    static const double RATES[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    Host h;
    double fund, thd, peak;
    int r;
    printf("Plugin, 220 Hz sine:\n");
    for (r = 0; r < 4; ++r) {
        host_open(&h, RATES[r]);
        h.ctl[PORT_VOLUME] = 0.3f;
        sine_run(&h, RATES[r], 220.0, 0.05, 1.0, &fund, &thd, &peak);
        printf("  %6.0f Hz  clean, 50 mV in:  out %6.3f  THD %5.2f%%  peak %.3f\n", RATES[r], fund, 100 * thd, peak);
        CHECK(isfinite(peak) && peak < 4.0 && fund > 0.005, "clean channel at %.0f Hz", RATES[r]);
        h.ctl[PORT_OVERDRIVE] = 1.0f;
        h.ctl[PORT_DRIVE] = 0.8f;
        sine_run(&h, RATES[r], 220.0, 0.2, 1.0, &fund, &thd, &peak);
        printf("  %6.0f Hz  overdrive, 200 mV: out %6.3f  THD %5.2f%%  peak %.3f\n", RATES[r], fund, 100 * thd, peak);
        CHECK(isfinite(peak) && peak < 4.0, "overdrive output not bounded at %.0f Hz", RATES[r]);
        CHECK(thd > 0.05, "overdrive channel barely distorts at %.0f Hz", RATES[r]);
        CHECK(*h.p->port[PORT_LATENCY] == (float)(h.p->os > 1 ? OS_LATENCY : 0), "latency port");
        cleanup(h.p);
    }
    /* Distortion rises with drive. */
    {
        double t_lo, t_hi;
        host_open(&h, 48000.0);
        h.ctl[PORT_OVERDRIVE] = 1.0f;
        h.ctl[PORT_DRIVE] = 0.1f;
        sine_run(&h, 48000.0, 220.0, 0.05, 1.0, &fund, &t_lo, &peak);
        h.ctl[PORT_DRIVE] = 1.0f;
        sine_run(&h, 48000.0, 220.0, 0.05, 1.0, &fund, &t_hi, &peak);
        printf("  drive 1 -> 10 at 50 mV: THD %.1f%% -> %.1f%%\n", 100 * t_lo, 100 * t_hi);
        CHECK(t_hi > t_lo, "THD does not rise with drive");
        cleanup(h.p);
    }
    /* Presence raises the top end: less high-frequency feedback. */
    {
        double lo5, hi5, lo1, hi1;
        host_open(&h, 48000.0);
        h.ctl[PORT_VOLUME] = 0.2f;
        h.ctl[PORT_PRESENCE] = 0.0f;
        sine_run(&h, 48000.0, 5000.0, 0.01, 0.5, &lo5, &thd, &peak);
        sine_run(&h, 48000.0, 500.0, 0.01, 0.5, &lo1, &thd, &peak);
        h.ctl[PORT_PRESENCE] = 1.0f;
        sine_run(&h, 48000.0, 5000.0, 0.01, 0.5, &hi5, &thd, &peak);
        sine_run(&h, 48000.0, 500.0, 0.01, 0.5, &hi1, &thd, &peak);
        printf("  presence 0 -> 10: 5 kHz relative to 500 Hz %+.1f dB -> %+.1f dB\n", db(lo5 / lo1), db(hi5 / hi1));
        CHECK(db(hi5 / hi1) > db(lo5 / lo1) + 1.0, "presence does not brighten");
        cleanup(h.p);
    }
    /* Silence in, near silence out; bypass passes the input delayed by the latency. */
    {
        float in[4096] = { 0 }, out[4096];
        double m = 0.0;
        int i;
        host_open(&h, 48000.0);
        connect_port(h.p, PORT_IN, in);
        connect_port(h.p, PORT_OUT, out);
        activate(h.p);
        for (i = 0; i < 20; ++i) run(h.p, 4096);
        for (i = 0; i < 4096; ++i) if (fabs(out[i]) > m) m = fabs(out[i]);
        printf("  silence: output peak %.2e\n", m);
        CHECK(m < 1e-4, "noise with no input");
        h.ctl[PORT_ENABLED] = 0.0f;
        activate(h.p);
        in[10] = 1.0f;
        run(h.p, 4096);
        printf("  bypass: impulse at 10 comes out at %d\n", out[10 + OS_LATENCY] == 1.0f ? 10 + OS_LATENCY : -1);
        CHECK(out[10 + OS_LATENCY] == 1.0f, "bypass delay is not the reported latency");
        cleanup(h.p);
    }
}

/* ---- 5. Cost ---- */

static void test_cpu(void)
{
    Host h;
    const int n = 48000 * 10;
    float* in = (float*)malloc(sizeof(float) * n);
    float* out = (float*)malloc(sizeof(float) * n);
    clock_t t0;
    double secs;
    int i;
    for (i = 0; i < n; ++i)
        in[i] = (float)(0.3 * sin(2.0 * 3.14159265358979 * 110.0 * i / 48000.0));
    host_open(&h, 48000.0);
    h.ctl[PORT_OVERDRIVE] = 1.0f;
    h.ctl[PORT_DRIVE] = 1.0f;
    h.ctl[PORT_CAB] = 1.0f;
    activate(h.p);
    t0 = clock();
    for (i = 0; i < n; i += 256) {
        connect_port(h.p, PORT_IN, in + i);
        connect_port(h.p, PORT_OUT, out + i);
        run(h.p, 256);
    }
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("CPU: 10 s of audio at 48 kHz (4x oversampled, full drive) in %.2f s = %.1f%% of one core\n",
           secs, 10.0 * secs);
    CHECK(secs < 5.0, "too slow for real time");
    cleanup(h.p);
    free(in);
    free(out);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_operating_points();
    test_networks();
    test_tone_controls();
    test_plugin();
    test_cpu();
    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
