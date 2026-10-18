#include "smoke.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gm {

SmokeEffect::SmokeEffect() {
  params_.add("wind", &wind_, 0.0f, 200.0f, "cells/s^2 per g");
  params_.add("buoyancy", &buoyancy_, 0.0f, 50.0f, "rise per unit density");
  params_.add("emit", &emit_, 0.0f, 200.0f, "emitter density/s");
  params_.add("emit_r", &emitRadius_, 0.75f, 4.0f, "emitter radius cells");
  params_.add("emit_d", &emitDepth_, 0.5f, 4.0f, "emitter half-depth layers");
  params_.add("dissipate", &dissipation_, 0.0f, 5.0f, "density decay 1/s");
  params_.add("iters", &iters_, 1.0f, 30.0f, "pressure solver iterations");
  params_.add("rate", &rateHz_, 20.0f, 120.0f, "physics steps per second (20 minimum)");
  params_.add("gain", &gain_, 0.1f, 10.0f, "density to brightness");
  params_.add("swirl", &swirl_, 0.0f, 20.0f, "emitter turbulence");
}

SmokeEffect::~SmokeEffect() { release(); }

void SmokeEffect::allocate() {
  if (d_) return;
  u_ = new float[N]; v_ = new float[N]; w_ = new float[N];
  u0_ = new float[N]; v0_ = new float[N]; w0_ = new float[N];
  d_ = new float[N]; d0_ = new float[N];
}

void SmokeEffect::release() {
  delete[] u_; delete[] v_; delete[] w_;
  delete[] u0_; delete[] v0_; delete[] w0_;
  delete[] d_; delete[] d0_;
  u_ = v_ = w_ = u0_ = v0_ = w0_ = d_ = d0_ = nullptr;
}

void SmokeEffect::activate() { allocate(); reset(); }
void SmokeEffect::deactivate() { release(); }

void SmokeEffect::reset() {
  if (!d_) return;
  const size_t bytes = sizeof(float) * N;
  std::memset(u_, 0, bytes); std::memset(v_, 0, bytes); std::memset(w_, 0, bytes);
  std::memset(u0_, 0, bytes); std::memset(v0_, 0, bytes); std::memset(w0_, 0, bytes);
  std::memset(d_, 0, bytes); std::memset(d0_, 0, bytes);
  accum_ = 0.0f;
  sinceStep_ = 0.0f;
}

float SmokeEffect::totalDensity() const {
  if (!d_) return 0.0f;
  float s = 0.0f;
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) s += d_[idx(i, j, k)];
  return s;
}

// Boundary conditions. b = 1/2/3 flips the normal velocity component at the
// x/y/z walls (free-slip, no penetration); b = 0 copies (Neumann) for scalars.
void SmokeEffect::setBnd(int b, float* x) {
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j) {
      x[idx(0, j, k)] = b == 1 ? -x[idx(1, j, k)] : x[idx(1, j, k)];
      x[idx(NX + 1, j, k)] = b == 1 ? -x[idx(NX, j, k)] : x[idx(NX, j, k)];
    }
  for (int k = 1; k <= NZ; ++k)
    for (int i = 1; i <= NX; ++i) {
      x[idx(i, 0, k)] = b == 2 ? -x[idx(i, 1, k)] : x[idx(i, 1, k)];
      x[idx(i, NY + 1, k)] = b == 2 ? -x[idx(i, NY, k)] : x[idx(i, NY, k)];
    }
  for (int j = 1; j <= NY; ++j)
    for (int i = 1; i <= NX; ++i) {
      x[idx(i, j, 0)] = b == 3 ? -x[idx(i, j, 1)] : x[idx(i, j, 1)];
      x[idx(i, j, NZ + 1)] = b == 3 ? -x[idx(i, j, NZ)] : x[idx(i, j, NZ)];
    }
  // Edges and corners: average of the inward neighbours along each boundary axis.
  for (int k = 0; k < SZ; ++k) {
    const bool bk = (k == 0 || k == SZ - 1);
    for (int j = 0; j < SY; ++j) {
      const bool bj = (j == 0 || j == SY - 1);
      for (int i = 0; i < SX; ++i) {
        const bool bi = (i == 0 || i == SX - 1);
        const int nb = (bi ? 1 : 0) + (bj ? 1 : 0) + (bk ? 1 : 0);
        if (nb < 2) continue;
        float s = 0.0f;
        if (bi) s += x[idx(i == 0 ? 1 : SX - 2, j, k)];
        if (bj) s += x[idx(i, j == 0 ? 1 : SY - 2, k)];
        if (bk) s += x[idx(i, j, k == 0 ? 1 : SZ - 2)];
        x[idx(i, j, k)] = s / static_cast<float>(nb);
      }
    }
  }
}

void SmokeEffect::linSolve(int b, float* x, const float* x0, float a, float c, int iters) {
  const float invC = 1.0f / c;
  for (int it = 0; it < iters; ++it) {
    for (int k = 1; k <= NZ; ++k)
      for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
          const int id = idx(i, j, k);
          x[id] = (x0[id] + a * (x[id - 1] + x[id + 1] + x[id - SX] + x[id + SX] + x[id - SX * SY] + x[id + SX * SY])) * invC;
        }
    setBnd(b, x);
  }
}

void SmokeEffect::advect(int b, float* d, const float* d0, const float* u, const float* v, const float* w, float dt) {
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        float x = static_cast<float>(i) - dt * u[id];
        float y = static_cast<float>(j) - dt * v[id];
        float z = static_cast<float>(k) - dt * w[id];
        x = clampf(x, 0.5f, NX + 0.5f);
        y = clampf(y, 0.5f, NY + 0.5f);
        z = clampf(z, 0.5f, NZ + 0.5f);
        const int i0 = static_cast<int>(x), j0 = static_cast<int>(y), k0 = static_cast<int>(z);
        const float s1 = x - i0, s0 = 1.0f - s1;
        const float t1 = y - j0, t0 = 1.0f - t1;
        const float r1 = z - k0, r0 = 1.0f - r1;
        const int i1 = i0 + 1, j1 = j0 + 1, k1 = k0 + 1;
        d[id] = r0 * (s0 * (t0 * d0[idx(i0, j0, k0)] + t1 * d0[idx(i0, j1, k0)]) +
                      s1 * (t0 * d0[idx(i1, j0, k0)] + t1 * d0[idx(i1, j1, k0)])) +
                r1 * (s0 * (t0 * d0[idx(i0, j0, k1)] + t1 * d0[idx(i0, j1, k1)]) +
                      s1 * (t0 * d0[idx(i1, j0, k1)] + t1 * d0[idx(i1, j1, k1)]));
      }
  setBnd(b, d);
}

void SmokeEffect::project(float* u, float* v, float* w, float* p, float* div) {
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        div[id] = -0.5f * (u[id + 1] - u[id - 1] + v[id + SX] - v[id - SX] + w[id + SX * SY] - w[id - SX * SY]);
        p[id] = 0.0f;
      }
  setBnd(0, div);
  setBnd(0, p);
  linSolve(0, p, div, 1.0f, 6.0f, std::max(1, iround(iters_)));
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        u[id] -= 0.5f * (p[id + 1] - p[id - 1]);
        v[id] -= 0.5f * (p[id + SX] - p[id - SX]);
        w[id] -= 0.5f * (p[id + SX * SY] - p[id - SX * SY]);
      }
  setBnd(1, u);
  setBnd(2, v);
  setBnd(3, w);
}

void SmokeEffect::update(const EffectContext& ctx) {
  if (!d_) allocate(), reset();
  // Run the solver at rateHz_ regardless of the render rate (the dithered
  // rasterisation hides the lower physics rate). The schedule keeps its
  // remainder so the step rate is exact; each step advances by the real time
  // since the previous one, capped at 1/20 s for stability.
  accum_ += ctx.dt;
  sinceStep_ += ctx.dt;
  const float period = 1.0f / rateHz_;
  if (accum_ < period) return;
  accum_ -= period;
  if (accum_ > period) accum_ = 0.0f;  // drop backlog after a stall
  const float dt = std::min(sinceStep_, 1.0f / 20.0f);
  sinceStep_ = 0.0f;
  const Vec3 inertial = ctx.g.inertial();

  // Forces. A uniform body force on a closed box is a pure pressure gradient
  // and the projection step would cancel it, so the inertial force is
  // weighted by density: the smoke behaves like a heavy gas that is thrown
  // around while the clear air stays put. Buoyancy lifts dense cells.
  // Grid j grows downward, so "up" is -v.
  const float fx = wind_ * inertial.x * dt;
  const float fy = -wind_ * inertial.y * dt;
  const float fz = wind_ * inertial.z * dt;
  for (int k = 1; k <= NZ; ++k)
    for (int j = 1; j <= NY; ++j)
      for (int i = 1; i <= NX; ++i) {
        const int id = idx(i, j, k);
        const float dens = d_[id];
        u_[id] += fx * dens;
        v_[id] += (fy - buoyancy_ * dt) * dens;
        w_[id] += fz * dens;
      }
  {
    // Emitter: a short cylinder centred in the volume, emitRadius_ cells wide
    // on a layer and emitDepth_ layers deep, so the plume starts compact and
    // longitudinal g visibly moves it through the stack.
    const float ci = (NX + 1) * 0.5f, cj = NY * 0.62f + 0.5f, ck = (NZ + 1) * 0.5f;
    const float r2 = emitRadius_ * emitRadius_;
    for (int k = 1; k <= NZ; ++k)
      for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
          const float dx = static_cast<float>(i) - ci, dy = static_cast<float>(j) - cj;
          const float dz = (static_cast<float>(k) - ck) / emitDepth_;
          if (dx * dx + dy * dy > r2 || dz * dz > 1.0f) continue;
          const int id = idx(i, j, k);
          d_[id] += emit_ * dt;
          u_[id] += swirl_ * rng_.symmetric() * dt * 10.0f;
          w_[id] += swirl_ * rng_.symmetric() * dt * 10.0f;
        }
  }

  // Velocity step: project, advect, project.
  project(u_, v_, w_, u0_, v0_);
  std::swap(u_, u0_); std::swap(v_, v0_); std::swap(w_, w0_);
  advect(1, u_, u0_, u0_, v0_, w0_, dt);
  advect(2, v_, v0_, u0_, v0_, w0_, dt);
  advect(3, w_, w0_, u0_, v0_, w0_, dt);
  project(u_, v_, w_, u0_, v0_);

  // Density step.
  std::swap(d_, d0_);
  advect(0, d_, d0_, u_, v_, w_, dt);
  const float keep = std::max(0.0f, 1.0f - dissipation_ * dt);
  for (int n = 0; n < N; ++n) d_[n] *= keep;
}

void SmokeEffect::render(VoxelFrame& f) {
  if (!d_) return;
  const float threshold = 1.0f / 64.0f;
  for (int k = 1; k <= NZ; ++k) {
    const int layer = k - 1;
    for (int j = 1; j <= NY; ++j) {
      for (int i = 1; i <= NX; ++i) {
        const float val = d_[idx(i, j, k)] * gain_;
        if (val < threshold) continue;
        const int x0 = (i - 1) * CX, y0 = (j - 1) * CY;
        if (val >= 1.0f) { f.fillRect(layer, x0, y0, x0 + CX - 1, y0 + CY - 1); continue; }
        for (int y = y0; y < y0 + CY; ++y)
          for (int x = x0; x < x0 + CX; ++x) f.plot(x, y, layer, val);
      }
    }
  }
}

}  // namespace gm
