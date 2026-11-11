/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Davidian. See LICENSE and NOTICE.md. */
/* Circuit model of the reference schematic: a two-channel amplifier with four 12AX7
 * preamp stages, a long-tailed-pair phase inverter and two EL34s in push-pull. Component values and supply voltages are the schematic's;
 * estimates that are not on the schematic are marked "Estimate".
 *
 * Signal path (schematic names):
 *   input -> V1B -> clean tone stack -> Volume -> V1A -> .047uF -> Clean Master -.
 *                                                        \-> relay -> OD Trim -> V2B(1st)
 *   -> Drive -> V2B(2nd) -> OD tone stack -> OD Level -> relay -> V3 phase inverter
 *   -> EL34 pair -> output transformer -> speaker, with 4k7 feedback and Presence.
 * The schematic labels both overdrive triodes "V2B"; here they are v2a and v2b.
 *
 * The tube stages are solved sample by sample with Koren's models (tube.h). Each
 * passive network between them is solved exactly by network.h, driven by the stage's
 * plate as a Thevenin source (quiescent plate voltage removed, output resistance
 * Ra || rp). */
#ifndef AMP_H
#define AMP_H

#include "network.h"
#include "tube.h"

/* Supply rails as marked on the schematic. */
#define B1_V 465.0 /* output transformer centre tap */
#define B2_V 458.0 /* EL34 screens, via 1k each */
#define B3_V 433.0 /* phase inverter */
#define B4_V 361.0 /* overdrive stages */
#define B5_V 323.0 /* clean stages */

/* Grid input capacitance of a 12AX7 gain stage: Cgk + (1 + gain) Cgp with the datasheet
 * 1.6 pF and 1.7 pF and a stage gain of about 55. Estimate. */
#define C_MILLER 100e-12

/* Output stage. Estimates: the schematic gives no transformer data or bias current. */
#define RAA        3400.0 /* plate-to-plate load typical of two-EL34 output transformers */
#define R_SPEAKER  8.0
#define EL34_IDLE  0.035  /* idle plate current per tube, about 65% of 25 W at 465 V */
#define R_SAG      120.0  /* B+1 source resistance seen by the average plate current */
#define OT_LOW_HZ  20.0   /* primary inductance roll-off */
#define OT_HIGH_HZ 16000.0 /* leakage inductance roll-off */

/* ---- Pot tapers ---- */

#define TAPER_K 4.394449154672439 /* 2 ln 9: an audio taper at 10% of its value at half rotation */

static inline double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
static inline double taper_a(double x) { return (exp(TAPER_K * clamp01(x)) - 1.0) / (exp(TAPER_K) - 1.0); }
/* A pot section never quite reaches zero: wiper and end resistance. */
static inline double pot_r(double r) { return r < 10.0 ? 10.0 : r; }

/* ---- Common-cathode triode stage with bypassed cathode resistor ---- */

typedef struct {
    double ra, rk, bplus, rs; /* plate load, cathode resistor, supply, grid source resistance */
    double gk, kk;            /* cathode capacitor companion conductance; 1/(1/Rk + gk) */
    double ieq, ip, ig, vk;   /* state */
    double ip_prev;           /* previous sample, for the Newton starting guess */
    double vp_q, vk_q, ip_q, rp_q, rout;
} Stage;

static void stage_init(Stage* s, double ra, double rk, double ck, double bplus, double rs, double fs)
{
    double ip = 0.5e-3, dp = 0.0, dg = 0.0;
    int i;
    s->ra = ra;
    s->rk = rk;
    s->bplus = bplus;
    s->rs = rs;
    s->gk = 2.0 * ck * fs;
    s->kk = 1.0 / (1.0 / rk + s->gk);
    /* Operating point: no signal, capacitor open. */
    for (i = 0; i < 60; ++i) {
        const double f = triode_ip(&TUBE_12AX7, bplus - (ra + rk) * ip, -rk * ip, &dp, &dg);
        const double step = (ip - f) / (1.0 + dp * (ra + rk) + dg * rk);
        ip -= step;
        if (ip < 0.0) ip = 0.0;
        if (fabs(step) < 1e-15) break;
    }
    triode_ip(&TUBE_12AX7, bplus - (ra + rk) * ip, -rk * ip, &dp, &dg);
    s->ip_q = s->ip = s->ip_prev = ip;
    s->vk_q = s->vk = rk * ip;
    s->vp_q = bplus - ra * ip;
    s->rp_q = dp > 0.0 ? 1.0 / dp : 1e9;
    s->rout = ra * s->rp_q / (ra + s->rp_q);
    s->ig = 0.0;
    s->ieq = s->gk * s->vk; /* capacitor charged, no current */
}

/* vg: open-circuit grid voltage from the network ahead. Returns the plate's AC voltage. */
/* Back to the operating point, as after a long silence. */
static void stage_reset(Stage* s)
{
    s->ip = s->ip_prev = s->ip_q;
    s->vk = s->vk_q;
    s->ieq = s->gk * s->vk_q;
    s->ig = 0.0;
}

static double stage_run(Stage* s, double vg)
{
    const double vg_open = vg;
    /* Start from a linear prediction: usually one or two Newton steps then suffice. */
    double ip = 2.0 * s->ip - s->ip_prev, ig = 0.0, dig = 0.0, dp, dg;
    int i;
    /* Grid current through the source resistance clamps positive peaks. */
    if (vg_open > s->vk) {
        for (i = 0; i < 8; ++i) {
            double f;
            ig = triode_ig(vg - s->vk, &dig);
            f = vg - vg_open + s->rs * ig;
            vg -= f / (1.0 + s->rs * dig);
            if (fabs(f) < 1e-9) break;
        }
        ig = triode_ig(vg - s->vk, &dig);
    }
    for (i = 0; i < 10; ++i) {
        const double vk = s->kk * (ip + ig + s->ieq);
        const double f = triode_ip(&TUBE_12AX7, s->bplus - s->ra * ip - vk, vg - vk, &dp, &dg);
        const double step = (ip - f) / (1.0 + dp * (s->ra + s->kk) + dg * s->kk);
        ip -= step;
        if (ip < 0.0) ip = 0.0;
        if (ip > s->bplus / s->ra) ip = s->bplus / s->ra;
        /* The step was already applied, and Newton converges quadratically, so a 10 nA step
         * leaves an error many orders below a microvolt at the plate. */
        if (fabs(step) < 1e-8) break;
    }
    s->vk = s->kk * (ip + ig + s->ieq);
    s->ieq = 2.0 * s->gk * s->vk - s->ieq;
    s->ip_prev = s->ip;
    s->ip = ip;
    s->ig = ig;
    return s->bplus - s->ra * ip - s->vp_q;
}

/* ---- Long-tailed-pair phase inverter (V3A/V3B) ----
 * Plate loads 91k (V3A) and 110k (V3B) from B+3, 820R from the cathodes to the grid-leak
 * junction, 24k from there to the feedback node, which has 390R to ground. V3B's grid is
 * coupled to the feedback node by 0.1uF, so the pair amplifies (input - feedback).
 * Grid-leak bias and the coupling capacitors hold their DC values (the 1M leaks and
 * 0.1uF give corners near 1.6 Hz). */
#define PI_R1   91e3
#define PI_R2   110e3
#define PI_RBIAS 820.0
#define PI_RTAIL 24e3
#define PI_RFB  390.0

typedef struct {
    double i1, i2, iq;
    double i1_prev, i2_prev;
    double vp1_q, vp2_q, rout1, rout2;
} PhaseInv;

/* Residuals and Jacobian of the pair's two plate currents. */
static void pi_eval(const PhaseInv* p, double vin, double vfb, double i1, double i2, double F[2], double J[2][2])
{
    const double rk = PI_RFB + PI_RTAIL + PI_RBIAS;
    const double it = i1 + i2;
    const double vk = vfb + rk * it;
    const double vgk1 = vin + (PI_RTAIL + PI_RFB) * p->iq - vk;
    const double vgk2 = vfb + PI_RFB * it + PI_RTAIL * p->iq - vk;
    double dp1, dg1, dp2, dg2;
    const double f1 = triode_ip(&TUBE_12AX7, B3_V - PI_R1 * i1 - vk, vgk1, &dp1, &dg1);
    const double f2 = triode_ip(&TUBE_12AX7, B3_V - PI_R2 * i2 - vk, vgk2, &dp2, &dg2);
    F[0] = i1 - f1;
    F[1] = i2 - f2;
    /* d(vk)/d(i) = rk; d(vgk1)/d(i) = -rk; d(vgk2)/d(i) = -(rk - PI_RFB). */
    J[0][0] = 1.0 + dp1 * (PI_R1 + rk) + dg1 * rk;
    J[0][1] = dp1 * rk + dg1 * rk;
    J[1][0] = dp2 * rk + dg2 * (rk - PI_RFB);
    J[1][1] = 1.0 + dp2 * (PI_R2 + rk) + dg2 * (rk - PI_RFB);
}

static void pi_solve(PhaseInv* p, double vin, double vfb, int iters)
{
    int k;
    for (k = 0; k < iters; ++k) {
        double F[2], J[2][2], det, d1, d2;
        pi_eval(p, vin, vfb, p->i1, p->i2, F, J);
        det = J[0][0] * J[1][1] - J[0][1] * J[1][0];
        if (fabs(det) < 1e-30) break;
        d1 = (F[0] * J[1][1] - F[1] * J[0][1]) / det;
        d2 = (J[0][0] * F[1] - J[1][0] * F[0]) / det;
        p->i1 -= d1;
        p->i2 -= d2;
        if (p->i1 < 0.0) p->i1 = 0.0;
        if (p->i2 < 0.0) p->i2 = 0.0;
        if (fabs(d1) + fabs(d2) < 1e-8) break;
    }
}

static void pi_init(PhaseInv* p)
{
    int k;
    double dp, dg;
    p->i1 = p->i2 = 1.2e-3;
    p->iq = 2.4e-3;
    for (k = 0; k < 40; ++k) {
        pi_solve(p, 0.0, 0.0, 30);
        p->iq = p->i1 + p->i2;
    }
    p->i1_prev = p->i1;
    p->i2_prev = p->i2;
    {
        const double vk = (PI_RFB + PI_RTAIL + PI_RBIAS) * p->iq;
        p->vp1_q = B3_V - PI_R1 * p->i1;
        p->vp2_q = B3_V - PI_R2 * p->i2;
        triode_ip(&TUBE_12AX7, p->vp1_q - vk, -PI_RBIAS * p->iq, &dp, &dg);
        p->rout1 = PI_R1 / (1.0 + PI_R1 * dp);
        triode_ip(&TUBE_12AX7, p->vp2_q - vk, -PI_RBIAS * p->iq, &dp, &dg);
        p->rout2 = PI_R2 / (1.0 + PI_R2 * dp);
    }
}

/* Returns the AC plate voltages. */
static void pi_run(PhaseInv* p, double vin, double vfb, double* a1, double* a2)
{
    const double g1 = 2.0 * p->i1 - p->i1_prev, g2 = 2.0 * p->i2 - p->i2_prev;
    p->i1_prev = p->i1;
    p->i2_prev = p->i2;
    p->i1 = g1 > 0.0 ? g1 : 0.0;
    p->i2 = g2 > 0.0 ? g2 : 0.0;
    pi_solve(p, vin, vfb, 8);
    *a1 = B3_V - PI_R1 * p->i1 - p->vp1_q;
    *a2 = B3_V - PI_R2 * p->i2 - p->vp2_q;
}

/* ---- Coupling to an EL34 grid: 0.1uF, 200k leak to the bias supply, 5k1 stopper ----
 * Grid current on positive peaks charges the capacitor, which shifts the bias on the
 * following cycles (blocking), as in the real amp. AC variables; vbias is absolute. */
#define CPL_C     0.1e-6
#define CPL_LEAK  200e3
#define CPL_STOP  5.1e3

typedef struct {
    double req, ht, veq, vbias, rg;
} Couple;

static void couple_init(Couple* c, double rout, double vbias, double fs)
{
    c->ht = 1.0 / (2.0 * CPL_C * fs);
    c->req = rout + c->ht;
    c->veq = 0.0;
    c->vbias = vbias;
    c->rg = EL34_RGK + CPL_STOP;
}

/* vs: AC plate voltage of the driver. Returns the EL34 grid-cathode voltage. */
static double couple_run(Couple* c, double vs)
{
    const double inj = (vs - c->veq) / c->req;
    double g = 1.0 / c->req + 1.0 / CPL_LEAK;
    double v = inj / g, ig = 0.0, i;
    if (c->vbias + v > 0.0) {
        g += 1.0 / c->rg;
        v = (inj - c->vbias / c->rg) / g;
        ig = (c->vbias + v) / c->rg;
        if (ig < 0.0) { /* boundary: no conduction after all */
            g -= 1.0 / c->rg;
            v = inj / g;
            ig = 0.0;
        }
    }
    i = (vs - v - c->veq) / c->req;
    c->veq += 2.0 * c->ht * i; /* trapezoidal: next history = vc + ht*i */
    return c->vbias + v - CPL_STOP * ig;
}

/* ---- EL34 push-pull into the output transformer ---- */

typedef struct {
    double rh, ratio;      /* quarter of Raa; half-primary to secondary turns ratio */
    double b1, vg2a, vg2b; /* sagging supply, screen voltages */
    double x;              /* plate current difference */
    double iavg, iidle, sag_a;
    double vbias;
} Power;

static double el34_idle(double vbias, double* ig2_out)
{
    double vg2 = B2_V, ig2 = 0.0, e = 0.0, d;
    int k;
    for (k = 0; k < 20; ++k) {
        ig2 = pentode_ig2(&TUBE_EL34, vbias, vg2);
        vg2 = B2_V - 1000.0 * ig2;
    }
    e = pentode_e(&TUBE_EL34, vbias, vg2);
    if (ig2_out) *ig2_out = ig2;
    return pentode_ip(&TUBE_EL34, e, B1_V, &d);
}

static void power_init(Power* p, double fs)
{
    double lo = -120.0, hi = 0.0, ig2 = 0.0;
    int k;
    for (k = 0; k < 60; ++k) { /* bias voltage for the idle current */
        const double m = 0.5 * (lo + hi);
        if (el34_idle(m, NULL) > EL34_IDLE) hi = m; else lo = m;
    }
    p->vbias = 0.5 * (lo + hi);
    el34_idle(p->vbias, &ig2);
    p->rh = RAA / 4.0;
    p->ratio = sqrt(p->rh / R_SPEAKER);
    p->b1 = B1_V;
    p->vg2a = p->vg2b = B2_V - 1000.0 * ig2;
    p->x = 0.0;
    p->iidle = p->iavg = 2.0 * (EL34_IDLE + ig2);
    p->sag_a = 1.0 - exp(-1.0 / (0.06 * fs)); /* reservoir recovery, about 60 ms */
}

/* Grid-cathode voltages in; speaker voltage out (before the transformer's band limits). */
static double power_run(Power* p, double vga, double vgb)
{
    const double ea = pentode_e(&TUBE_EL34, vga, p->vg2a);
    const double eb = pentode_e(&TUBE_EL34, vgb, p->vg2b);
    double x = p->x, ia = 0.0, ib = 0.0, da, db, ig2a, ig2b, b2;
    int k;
    for (k = 0; k < 10; ++k) {
        double f, step;
        ia = pentode_ip(&TUBE_EL34, ea, p->b1 - p->rh * x, &da);
        ib = pentode_ip(&TUBE_EL34, eb, p->b1 + p->rh * x, &db);
        f = x - (ia - ib);
        step = f / (1.0 + p->rh * (da + db));
        x -= step;
        /* currents at the new x, to first order */
        ia += p->rh * da * step;
        ib -= p->rh * db * step;
        if (fabs(step) < 1e-6) break;
    }
    p->x = x;
    /* Screens drop across their 1k resistors; the supply sags with the average current. */
    b2 = p->b1 - (B1_V - B2_V);
    ig2a = pentode_ig2(&TUBE_EL34, vga, p->vg2a);
    ig2b = pentode_ig2(&TUBE_EL34, vgb, p->vg2b);
    p->vg2a = b2 - 1000.0 * ig2a;
    p->vg2b = b2 - 1000.0 * ig2b;
    p->iavg += p->sag_a * (ia + ib + ig2a + ig2b - p->iavg);
    p->b1 = B1_V - R_SAG * (p->iavg - p->iidle);
    if (p->b1 > B1_V + 20.0) p->b1 = B1_V + 20.0;
    /* Polarity chosen so the 4k7 loop is negative feedback, as in the amp. */
    return -p->rh * x / p->ratio;
}

/* ---- The whole amplifier ---- */

typedef struct {
    double volume, treble, middle, bass, master;
    double drive, od_treble, od_middle, od_bass, od_level, od_trim;
    double presence;
    int overdrive;
} AmpControls;

/* Network node numbers, so tests can probe them. */
enum { CL_P = 1, CL_T, CL_A, CL_B, CL_C, CL_M, CL_W, CL_V, CL_G, CL_NODES = CL_G };
enum { MD_P = 1, MD_X, MD_Y, MD_Z, MD_Q, MD_R, MD_G, MD_NODES = MD_G };
enum { DR_P = 1, DR_D1, DR_D2, DR_W, DR_G, DR_NODES = DR_G };
enum { OD_P = 1, OD_T, OD_A, OD_B, OD_C, OD_W, OD_L, OD_Z, OD_NODES = OD_Z };
enum { FB_N = 1, FB_Q, FB_NODES = FB_Q };

typedef struct {
    double fs;
    Stage v1b, v1a, v2a, v2b;
    Net clean, mid, drive, od, fb;
    /* resistor indices of the pots and the relay */
    int cl_mid, cl_tup, cl_tlo, cl_bass, cl_vup, cl_vlo;
    int md_mup, md_mlo, md_on, md_off, md_tup, md_tlo;
    int dr_up, dr_lo;
    int od_tup, od_tlo, od_bass, od_mid, od_lup, od_llo;
    int fb_pres;
    PhaseInv pi;
    Couple ca, cb;
    Power pw;
    double vfb;
    double chan, chan_step;
    int chan_target;
    int od_idle; /* overdrive stages skipped while the clean channel is selected */
    double hp_g, hp_s, lp_g, lp_s; /* transformer band limits (TPT one-poles) */
} Amp;

static void amp_build(Amp* a)
{
    Net* n;
    const double fs = a->fs;

    /* V1B plate -> clean tone stack -> Volume -> 220k || 390pF -> V1A grid */
    n = &a->clean;
    net_init(n, CL_NODES, fs);
    net_src(n, CL_P, a->v1b.rout);
    net_c(n, CL_P, CL_T, 330e-12);
    net_r(n, CL_P, CL_A, 150e3);
    net_c(n, CL_A, CL_B, 0.1e-6);
    net_c(n, CL_A, CL_M, 0.01e-6);
    a->cl_mid = net_r(n, CL_M, 0, 1.0);  /* Middle 250kA, rheostat */
    a->cl_tup = net_r(n, CL_T, CL_W, 1.0); /* Treble 250kB */
    a->cl_tlo = net_r(n, CL_W, CL_B, 1.0);
    a->cl_bass = net_r(n, CL_B, CL_C, 1.0); /* Bass 500kA, rheostat */
    net_c(n, CL_B, CL_C, 0.001e-6);
    net_r(n, CL_C, 0, 9.8e3);
    a->cl_vup = net_r(n, CL_W, CL_V, 1.0); /* Volume 1MA */
    a->cl_vlo = net_r(n, CL_V, 0, 1.0);
    net_r(n, CL_V, CL_G, 220e3);
    net_c(n, CL_V, CL_G, 390e-12);
    net_c(n, CL_G, 0, C_MILLER);
    net_out(n, CL_G);

    /* V1A plate -> .047uF -> Clean Master -> 0.01uF/1M (PI input); relay -> 100k -> OD Trim -> 68k */
    n = &a->mid;
    net_init(n, MD_NODES, fs);
    net_src(n, MD_P, a->v1a.rout);
    net_c(n, MD_P, MD_X, 0.047e-6);
    a->md_mup = net_r(n, MD_X, MD_Y, 1.0); /* Clean Master 1MA */
    a->md_mlo = net_r(n, MD_Y, 0, 1.0);
    net_c(n, MD_Y, MD_Z, 0.01e-6);
    net_r(n, MD_Z, 0, 1e6);
    a->md_on = net_r(n, MD_X, MD_Q, 1.0);  /* relay: 100k fed from V1A ... */
    a->md_off = net_r(n, 0, MD_Q, 1.0);    /* ... or grounded on the clean channel */
    a->md_tup = net_r(n, MD_Q, MD_R, 1.0); /* OD Trim 100kB */
    a->md_tlo = net_r(n, MD_R, 0, 1.0);
    net_r(n, MD_R, MD_G, 68e3);
    net_c(n, MD_G, 0, C_MILLER);
    net_out(n, MD_Z);
    net_out(n, MD_G);

    /* first overdrive stage -> .0022uF -> 100k -> Drive 250kA -> 180k -> second stage */
    n = &a->drive;
    net_init(n, DR_NODES, fs);
    net_src(n, DR_P, a->v2a.rout);
    net_c(n, DR_P, DR_D1, 0.0022e-6);
    net_r(n, DR_D1, DR_D2, 100e3);
    a->dr_up = net_r(n, DR_D2, DR_W, 1.0);
    a->dr_lo = net_r(n, DR_W, 0, 1.0);
    net_r(n, DR_W, DR_G, 180e3);
    net_c(n, DR_G, 0, C_MILLER);
    net_out(n, DR_G);

    /* second overdrive stage -> OD tone stack -> OD Level 1MA -> 0.01uF/1M (PI input) */
    n = &a->od;
    net_init(n, OD_NODES, fs);
    net_src(n, OD_P, a->v2b.rout);
    net_c(n, OD_P, OD_T, 330e-12);
    net_r(n, OD_P, OD_A, 33e3);
    net_c(n, OD_A, OD_B, 0.022e-6);
    net_c(n, OD_A, OD_C, 0.022e-6);
    a->od_tup = net_r(n, OD_T, OD_W, 1.0);  /* Treble 250kB */
    a->od_tlo = net_r(n, OD_W, OD_B, 1.0);
    a->od_bass = net_r(n, OD_B, OD_C, 1.0); /* Bass 1MB, rheostat */
    a->od_mid = net_r(n, OD_C, 0, 1.0);     /* Middle 20kB, rheostat */
    a->od_lup = net_r(n, OD_W, OD_L, 1.0);  /* OD Level 1MA */
    a->od_llo = net_r(n, OD_L, 0, 1.0);
    net_c(n, OD_L, OD_Z, 0.01e-6);
    net_r(n, OD_Z, 0, 1e6);
    net_out(n, OD_Z);

    /* speaker -> 4k7 -> feedback node: 390R || (Presence 2kB + 1uF) */
    n = &a->fb;
    net_init(n, FB_NODES, fs);
    net_src(n, FB_N, 4.7e3);
    net_r(n, FB_N, 0, 390.0);
    a->fb_pres = net_r(n, FB_N, FB_Q, 1.0);
    net_c(n, FB_Q, 0, 1e-6);
    net_out(n, FB_N);
}

static void amp_set(Amp* a, const AmpControls* k)
{
    double t;
    /* Clean channel */
    net_set_r(&a->clean, a->cl_mid, pot_r(250e3 * taper_a(k->middle)));
    net_set_r(&a->clean, a->cl_tup, pot_r(250e3 * (1.0 - clamp01(k->treble))));
    net_set_r(&a->clean, a->cl_tlo, pot_r(250e3 * clamp01(k->treble)));
    net_set_r(&a->clean, a->cl_bass, pot_r(500e3 * taper_a(k->bass)));
    t = taper_a(k->volume);
    net_set_r(&a->clean, a->cl_vup, pot_r(1e6 * (1.0 - t)));
    net_set_r(&a->clean, a->cl_vlo, pot_r(1e6 * t));
    net_factor(&a->clean);

    t = taper_a(k->master);
    net_set_r(&a->mid, a->md_mup, pot_r(1e6 * (1.0 - t)));
    net_set_r(&a->mid, a->md_mlo, pot_r(1e6 * t));
    net_set_r(&a->mid, a->md_on, k->overdrive ? 100e3 : 1e12);
    net_set_r(&a->mid, a->md_off, k->overdrive ? 1e12 : 100e3);
    net_set_r(&a->mid, a->md_tup, pot_r(100e3 * (1.0 - clamp01(k->od_trim))));
    net_set_r(&a->mid, a->md_tlo, pot_r(100e3 * clamp01(k->od_trim)));
    net_factor(&a->mid);

    t = taper_a(k->drive);
    net_set_r(&a->drive, a->dr_up, pot_r(250e3 * (1.0 - t)));
    net_set_r(&a->drive, a->dr_lo, pot_r(250e3 * t));
    net_factor(&a->drive);

    net_set_r(&a->od, a->od_tup, pot_r(250e3 * (1.0 - clamp01(k->od_treble))));
    net_set_r(&a->od, a->od_tlo, pot_r(250e3 * clamp01(k->od_treble)));
    net_set_r(&a->od, a->od_bass, pot_r(1e6 * clamp01(k->od_bass)));
    net_set_r(&a->od, a->od_mid, pot_r(20e3 * clamp01(k->od_middle)));
    t = taper_a(k->od_level);
    net_set_r(&a->od, a->od_lup, pot_r(1e6 * (1.0 - t)));
    net_set_r(&a->od, a->od_llo, pot_r(1e6 * t));
    net_factor(&a->od);

    /* Presence up = less resistance = less treble feedback = brighter. */
    net_set_r(&a->fb, a->fb_pres, pot_r(2e3 * (1.0 - clamp01(k->presence))));
    net_factor(&a->fb);

    a->chan_target = k->overdrive ? 1 : 0;
}

static double tpt_g(double fc, double fs) { return tan(3.14159265358979 * fc / fs); }

static void amp_init(Amp* a, double fs, const AmpControls* k)
{
    memset(a, 0, sizeof *a);
    a->fs = fs;
    /* Values from the schematic: plate load, cathode R || C, supply, grid source R. */
    stage_init(&a->v1b, 220e3, 3.3e3, 5e-6, B5_V, 22e3, fs);
    stage_init(&a->v1a, 150e3, 3.3e3, 5e-6, B5_V, 220e3, fs);
    stage_init(&a->v2a, 180e3, 2.7e3, 5e-6, B4_V, 68e3, fs);
    stage_init(&a->v2b, 120e3, 1.8e3, 1e-6, B4_V, 180e3, fs);
    pi_init(&a->pi);
    power_init(&a->pw, fs);
    couple_init(&a->ca, a->pi.rout1, a->pw.vbias, fs);
    couple_init(&a->cb, a->pi.rout2, a->pw.vbias, fs);
    amp_build(a);
    amp_set(a, k);
    a->chan = a->chan_target;
    a->chan_step = 1.0 / (0.005 * fs); /* relay changeover, 5 ms */
    a->hp_g = tpt_g(OT_LOW_HZ, fs);
    a->lp_g = tpt_g(OT_HIGH_HZ, fs);
}

/* One sample at the model rate. vin: volts at the input jack. Returns speaker volts. */
static double amp_run(Amp* a, double vin)
{
    double v, zc, zo, p1, p2, out;
    /* clean preamp */
    v = stage_run(&a->v1b, vin);
    net_step(&a->clean, &v);
    v = stage_run(&a->v1a, a->clean.v[CL_G]);
    net_step(&a->mid, &v);
    zc = a->mid.v[MD_Z];
    /* overdrive section. On the clean channel the relay grounds its input, so it only
     * idles; it is skipped then and restarted from rest when selected. */
    if (a->chan_target || a->chan > 0.0) {
        if (a->od_idle) {
            stage_reset(&a->v2a);
            stage_reset(&a->v2b);
            net_clear(&a->drive);
            net_clear(&a->od);
            a->od_idle = 0;
        }
        v = stage_run(&a->v2a, a->mid.v[MD_G]);
        net_step(&a->drive, &v);
        v = stage_run(&a->v2b, a->drive.v[DR_G]);
        net_step(&a->od, &v);
        zo = a->od.v[OD_Z];
    } else {
        a->od_idle = 1;
        zo = 0.0;
    }
    /* channel relay: the phase inverter hears the clean master or the OD level */
    if (a->chan < a->chan_target) {
        a->chan += a->chan_step;
        if (a->chan > 1.0) a->chan = 1.0;
    } else if (a->chan > a->chan_target) {
        a->chan -= a->chan_step;
        if (a->chan < 0.0) a->chan = 0.0;
    }
    v = zc + a->chan * (zo - zc);
    /* power amp */
    pi_run(&a->pi, v, a->vfb, &p1, &p2);
    out = power_run(&a->pw, couple_run(&a->ca, p1), couple_run(&a->cb, p2));
    /* transformer: high-pass (primary inductance), then low-pass (leakage) */
    {
        double w = (out - a->hp_s) * (a->hp_g / (1.0 + a->hp_g));
        const double low = w + a->hp_s;
        a->hp_s = low + w;
        out -= low;
        w = (out - a->lp_s) * (a->lp_g / (1.0 + a->lp_g));
        out = w + a->lp_s;
        a->lp_s = out + w;
    }
    /* negative feedback for the next sample */
    net_step(&a->fb, &out);
    a->vfb = a->fb.v[FB_N];
    return out;
}

#endif
