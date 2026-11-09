/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Davidian. See LICENSE and NOTICE.md. */
/* Vacuum tube plate-current models after Norman Koren, "Improved vacuum tube models for SPICE
 * simulations" (Glass Audio, 1996). Parameters are Koren's published fits:
 *   12AX7A: mu 101.24, ex 1.267, kg1 1002.9, kp 699.73, kvb 300 (fit by Tom Mitchell, 2001,
 *           listed on Koren's tube parameter page)
 *   EL34:  mu 11, ex 1.35, kg1 650, kg2 4200, kp 60, kvb 24
 * Voltages in volts, currents in amps. */
#ifndef TUBE_H
#define TUBE_H

#include <math.h>

typedef struct {
    double mu, ex, kg1, kp, kvb;
} Triode;

typedef struct {
    double mu, ex, kg1, kg2, kp, kvb;
} Pentode;

static const Triode TUBE_12AX7 = { 101.24, 1.267, 1002.9, 699.73, 300.0 };
static const Pentode TUBE_EL34 = { 11.0, 1.35, 650.0, 4200.0, 60.0, 24.0 };

/* ln(1 + e^u) without overflow. */
static inline double softplus(double u)
{
    return u > 30.0 ? u : log1p(exp(u));
}

/* Triode plate current, with partial derivatives by plate-cathode and grid-cathode voltage. */
#ifdef HO_PROFILE
static long long g_triode_evals;
#endif
static inline double triode_ip(const Triode* t, double vpk, double vgk, double* d_vpk, double* d_vgk)
{
#ifdef HO_PROFILE
    ++g_triode_evals;
#endif
    double s, u, sp, sg, e1, p;
    if (vpk <= 0.0) {
        *d_vpk = *d_vgk = 0.0;
        return 0.0;
    }
    s = sqrt(t->kvb + vpk * vpk);
    u = t->kp * (1.0 / t->mu + vgk / s);
    /* softplus and sigmoid from one exponential */
    if (u > 30.0) {
        sp = u;
        sg = 1.0;
    } else {
        const double eu = exp(u);
        sp = log1p(eu);
        sg = eu / (1.0 + eu);
    }
    e1 = vpk / t->kp * sp;
    if (e1 <= 1e-12) {
        *d_vpk = *d_vgk = 0.0;
        return 0.0;
    }
    p = exp((t->ex - 1.0) * log(e1));
    {
        const double k = t->ex * p / t->kg1;
        *d_vgk = k * vpk * sg / s;
        *d_vpk = k * (sp / t->kp - vpk * vpk * vgk * sg / (s * s * s));
    }
    return p * e1 / t->kg1;
}

/* Grid current of a small-signal triode once the grid goes positive of the cathode:
 * roughly a 1.5-power law, about 0.4 mA at +1 V. Estimate, not from Koren. */
static inline double triode_ig(double vgk, double* d_vgk)
{
    if (vgk <= 0.0) {
        *d_vgk = 0.0;
        return 0.0;
    }
    *d_vgk = 1.5 * 4e-4 * sqrt(vgk);
    return 4e-4 * vgk * sqrt(vgk);
}

/* Pentode plate current. The grid-1 term is computed once per sample (pentode_e) because
 * it does not depend on the plate voltage; pentode_ip then applies the plate knee. */
static inline double pentode_e(const Pentode* t, double vg1k, double vg2k)
{
    double e1;
    if (vg2k <= 1.0)
        return 0.0;
    e1 = vg2k / t->kp * softplus(t->kp * (1.0 / t->mu + vg1k / vg2k));
    return e1 <= 0.0 ? 0.0 : pow(e1, t->ex) / t->kg1;
}

static inline double pentode_ip(const Pentode* t, double e, double vpk, double* d_vpk)
{
    const double x = vpk / t->kvb;
    if (vpk <= 0.0) {
        *d_vpk = e / t->kvb; /* keeps Newton moving back into the valid region */
        return 0.0;
    }
    *d_vpk = e / (t->kvb * (1.0 + x * x));
    return e * atan(x);
}

/* Screen current (Koren). */
static inline double pentode_ig2(const Pentode* t, double vg1k, double vg2k)
{
    const double v = vg2k / t->mu + vg1k;
    return v <= 0.0 ? 0.0 : pow(v, t->ex) / t->kg2;
}

/* Control-grid current of a power tube driven positive, through its internal grid-cathode
 * resistance. About 1.5k for an EL34; estimate, not from Koren. */
#define EL34_RGK 1500.0

#endif
