/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Davidian. See LICENSE and NOTICE.md. */
/* A small linear R-C circuit solver. Each passive network of the schematic (tone stacks,
 * volume and drive pots, coupling capacitors, the feedback divider) is entered as a
 * netlist and solved by nodal analysis every sample. Capacitors use the trapezoidal
 * companion model, so the response matches the analog circuit up to the usual frequency
 * warping, which is small at the oversampled rate.
 *
 * Node 0 is ground. Sources are Thevenin voltages (a tube plate) with a series
 * resistance, entered as Norton equivalents. With only R and C the nodal matrix is
 * symmetric positive definite, so it is factored by Cholesky whenever a pot moves, and
 * turned into a discrete state-space system whose states are the capacitor charges. */
#ifndef NETWORK_H
#define NETWORK_H

#include <math.h>
#include <string.h>

#define NET_MAX_NODES 12
#define NET_MAX_R     16
#define NET_MAX_C     8
#define NET_MAX_SRC   2
#define NET_MAX_OUT   2

typedef struct { int a, b; double r; } NetR;
typedef struct { int a, b; double c, g, ieq; } NetC;
typedef struct { int node; double r; } NetSrc;

typedef struct {
    int nodes; /* excluding ground */
    double fs;
    NetR res[NET_MAX_R];
    NetC cap[NET_MAX_C];
    NetSrc src[NET_MAX_SRC];
    int nr, nc, ns;
    double L[NET_MAX_NODES][NET_MAX_NODES]; /* Cholesky factor, lower triangle */
    double v[NET_MAX_NODES + 1];            /* output node voltages after each step */
    /* Discrete state-space form, rebuilt by net_factor: the states are the capacitor
     * history currents, so a step costs a few multiply-adds per capacitor. */
    int no, out[NET_MAX_OUT];
    double A[NET_MAX_C][NET_MAX_C], B[NET_MAX_C][NET_MAX_SRC];
    double C[NET_MAX_OUT][NET_MAX_C], D[NET_MAX_OUT][NET_MAX_SRC];
} Net;

static void net_init(Net* n, int nodes, double fs)
{
    memset(n, 0, sizeof *n);
    n->nodes = nodes;
    n->fs = fs;
}

static int net_r(Net* n, int a, int b, double r)
{
    n->res[n->nr].a = a;
    n->res[n->nr].b = b;
    n->res[n->nr].r = r;
    return n->nr++;
}

static int net_c(Net* n, int a, int b, double c)
{
    NetC* k = &n->cap[n->nc];
    k->a = a;
    k->b = b;
    k->c = c;
    k->g = 2.0 * c * n->fs;
    k->ieq = 0.0;
    return n->nc++;
}

static int net_src(Net* n, int node, double r)
{
    n->src[n->ns].node = node;
    n->src[n->ns].r = r;
    return n->ns++;
}

/* Registers a node whose voltage net_step reports in v[]. */
static int net_out(Net* n, int node)
{
    n->out[n->no] = node;
    return n->no++;
}

static inline void net_set_r(Net* n, int i, double r) { n->res[i].r = r; }

/* Adds a conductance between two nodes to a nodal matrix. */
static void net_stamp(double G[NET_MAX_NODES][NET_MAX_NODES], int a, int b, double g)
{
    if (a) G[a - 1][a - 1] += g;
    if (b) G[b - 1][b - 1] += g;
    if (a && b) {
        G[a - 1][b - 1] -= g;
        G[b - 1][a - 1] -= g;
    }
}

/* Solves G x = b in place with the Cholesky factor. Node numbers are 1-based in b. */
static void net_solve(const Net* n, double* b)
{
    const int N = n->nodes;
    int i, k;
    for (i = 0; i < N; ++i) {
        double s = b[i];
        for (k = 0; k < i; ++k)
            s -= n->L[i][k] * b[k];
        b[i] = s / n->L[i][i];
    }
    for (i = N - 1; i >= 0; --i) {
        double s = b[i];
        for (k = i + 1; k < N; ++k)
            s -= n->L[k][i] * b[k];
        b[i] = s / n->L[i][i];
    }
}

/* Builds the nodal matrix, factors it and derives the state-space form. Call after any
 * resistor changes. */
static void net_factor(Net* n)
{
    double G[NET_MAX_NODES][NET_MAX_NODES];
    double w[NET_MAX_NODES + 1];
    const int N = n->nodes;
    int i, j, k;
    memset(G, 0, sizeof G);
    for (i = 0; i < n->nr; ++i)
        net_stamp(G, n->res[i].a, n->res[i].b, 1.0 / n->res[i].r);
    for (i = 0; i < n->nc; ++i)
        net_stamp(G, n->cap[i].a, n->cap[i].b, n->cap[i].g);
    for (i = 0; i < n->ns; ++i)
        net_stamp(G, n->src[i].node, 0, 1.0 / n->src[i].r);
    for (j = 0; j < N; ++j) {
        double d = G[j][j];
        for (k = 0; k < j; ++k)
            d -= n->L[j][k] * n->L[j][k];
        n->L[j][j] = sqrt(d > 1e-300 ? d : 1e-300);
        for (i = j + 1; i < N; ++i) {
            double s = G[i][j];
            for (k = 0; k < j; ++k)
                s -= n->L[i][k] * n->L[j][k];
            n->L[i][j] = s / n->L[j][j];
        }
    }
    /* Node voltages are v = G^-1 (sum_k s_k vs_k + sum_j p_j ieq_j), where p_j injects a
     * capacitor's history current at its two nodes. Solve for each column once. */
    for (j = 0; j < n->nc + n->ns; ++j) {
        memset(w, 0, sizeof w);
        if (j < n->nc) {
            if (n->cap[j].a) w[n->cap[j].a] += 1.0;
            if (n->cap[j].b) w[n->cap[j].b] -= 1.0;
        } else {
            w[n->src[j - n->nc].node] = 1.0 / n->src[j - n->nc].r;
        }
        net_solve(n, w + 1);
        w[0] = 0.0;
        for (i = 0; i < n->nc; ++i) {
            const NetC* c = &n->cap[i];
            const double dv = 2.0 * c->g * (w[c->a] - w[c->b]);
            if (j < n->nc)
                n->A[i][j] = dv - (i == j ? 1.0 : 0.0);
            else
                n->B[i][j - n->nc] = dv;
        }
        for (i = 0; i < n->no; ++i) {
            if (j < n->nc)
                n->C[i][j] = w[n->out[i]];
            else
                n->D[i][j - n->nc] = w[n->out[i]];
        }
    }
}

/* One time step. vs[i] is the open-circuit voltage of source i. Afterwards n->v holds
 * the voltages of the output nodes registered with net_out. */
static void net_step(Net* n, const double* vs)
{
    double x[NET_MAX_C];
    const int nc = n->nc, ns = n->ns;
    int i, j;
    for (i = 0; i < nc; ++i)
        x[i] = n->cap[i].ieq;
    for (i = 0; i < n->no; ++i) {
        double y = 0.0;
        for (j = 0; j < nc; ++j)
            y += n->C[i][j] * x[j];
        for (j = 0; j < ns; ++j)
            y += n->D[i][j] * vs[j];
        n->v[n->out[i]] = y;
    }
    for (i = 0; i < nc; ++i) {
        double s = 0.0;
        for (j = 0; j < nc; ++j)
            s += n->A[i][j] * x[j];
        for (j = 0; j < ns; ++j)
            s += n->B[i][j] * vs[j];
        n->cap[i].ieq = s;
    }
}

/* Clears the capacitor charges. */
static void net_clear(Net* n)
{
    int i;
    for (i = 0; i < n->nc; ++i)
        n->cap[i].ieq = 0.0;
    memset(n->v, 0, sizeof n->v);
}

#endif
