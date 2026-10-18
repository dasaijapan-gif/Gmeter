#include "slosh.hpp"

#include <algorithm>
#include <cmath>

namespace gm {

SloshEffect::SloshEffect() {
  params_.add("fill", &fillFraction_, 0.1f, 0.8f, "rest depth / panel height");
  params_.add("tank_m", &tankWidthM_, 0.1f, 10.0f, "virtual tank width (m); bigger = slower slosh");
  params_.add("slope_gain", &slopeGain_, 0.1f, 2.0f, "tilt per g (1 = physical)");
  params_.add("damping", &damping_, 0.0f, 10.0f, "velocity damping 1/s");
  params_.add("viscosity", &viscosity_, 0.0f, 60.0f, "surface diffusion px^2/s");
  params_.add("ripple", &ripple_, 0.0f, 50.0f, "idle ripple strength");
  params_.add("bob", &bobPxPerG_, 0.0f, 30.0f, "vertical bounce px per g");
  params_.add("bob_hz", &bobHz_, 0.5f, 10.0f, "vertical bounce frequency");
  params_.add("splash", &splashGain_, 0.0f, 300.0f, "splash on vertical jerk");
  params_.add("style", &style_, 0.0f, 2.0f, "0 surface, 1 surface+body, 2 solid");
  params_.add("body_i", &bodyIntensity_, 0.0f, 1.0f, "body dither density");
  params_.add("surface_px", &surfacePx_, 1.0f, 4.0f, "surface thickness px");
  params_.add("substeps", &maxSubsteps_, 1.0f, 16.0f, "max physics substeps per frame");
  reset();
}

float SloshEffect::gravityPx() const {
  // 1 g in pixels/s^2 given the virtual tank width.
  const float pxPerMetre = static_cast<float>(kW) / tankWidthM_;
  return kStandardGravity * pxPerMetre;
}

void SloshEffect::reset() {
  const float h0 = fillFraction_ * static_cast<float>(kH);
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x < NX; ++x) h_[z][x] = h0;
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x <= NX; ++x) u_[z][x] = 0.0f;
  for (int z = 0; z <= NZ; ++z)
    for (int x = 0; x < NX; ++x) w_[z][x] = 0.0f;
  targetVolume_ = h0 * NX * NZ;
  bob_.snap(0.0f);
  lastVertG_ = 0.0f;
  gvFiltered_ = 1.0f;
}

float SloshEffect::totalVolume() const {
  float v = 0.0f;
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x < NX; ++x) v += h_[z][x];
  return v;
}

void SloshEffect::substep(float dt, float gv, const Vec3& gEff) {
  const float gpx = gravityPx();
  const float ax = slopeGain_ * gEff.x * gpx;   // lateral body force (px/s^2)
  const float az = slopeGain_ * gEff.z * gpx;   // longitudinal body force
  const float damp = std::max(0.0f, 1.0f - damping_ * dt);
  constexpr float kDry = 0.05f;
  constexpr float invDX = 1.0f / DX, invDZ = 1.0f / DZ;

  // Momentum on interior faces (forward step). Walls stay at zero velocity
  // (reflecting); faces between two dry cells carry nothing.
  for (int z = 0; z < NZ; ++z) {
    u_[z][0] = 0.0f;
    u_[z][NX] = 0.0f;
    for (int x = 1; x < NX; ++x) {
      if (h_[z][x] < kDry && h_[z][x - 1] < kDry) { u_[z][x] = 0.0f; continue; }
      const float dhdx = (h_[z][x] - h_[z][x - 1]) * invDX;
      u_[z][x] = (u_[z][x] + dt * (-gv * dhdx + ax)) * damp;
    }
  }
  for (int x = 0; x < NX; ++x) { w_[0][x] = 0.0f; w_[NZ][x] = 0.0f; }
  for (int z = 1; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      if (h_[z][x] < kDry && h_[z - 1][x] < kDry) { w_[z][x] = 0.0f; continue; }
      const float dhdz = (h_[z][x] - h_[z - 1][x]) * invDZ;
      w_[z][x] = (w_[z][x] + dt * (-gv * dhdz + az)) * damp;
    }
  }

  // Mass fluxes with upwind depth.
  for (int z = 0; z < NZ; ++z) {
    fluxX_[z][0] = fluxX_[z][NX] = 0.0f;
    for (int x = 1; x < NX; ++x) {
      const float u = u_[z][x];
      fluxX_[z][x] = u * (u > 0.0f ? h_[z][x - 1] : h_[z][x]);
    }
  }
  for (int x = 0; x < NX; ++x) fluxZ_[0][x] = fluxZ_[NZ][x] = 0.0f;
  for (int z = 1; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      const float w = w_[z][x];
      fluxZ_[z][x] = w * (w > 0.0f ? h_[z - 1][x] : h_[z][x]);
    }
  }
  // Outflow limiter: a cell may not export more volume in one step than it
  // holds, so heights never go negative and mass is conserved exactly.
  for (int z = 0; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      float out = 0.0f;
      if (fluxX_[z][x + 1] > 0.0f) out += fluxX_[z][x + 1] * DZ;
      if (fluxX_[z][x] < 0.0f) out -= fluxX_[z][x] * DZ;
      if (fluxZ_[z + 1][x] > 0.0f) out += fluxZ_[z + 1][x] * DX;
      if (fluxZ_[z][x] < 0.0f) out -= fluxZ_[z][x] * DX;
      const float avail = h_[z][x] * DX * DZ;
      if (out * dt > avail && out > 0.0f) {
        const float k = avail / (out * dt);
        if (fluxX_[z][x + 1] > 0.0f) fluxX_[z][x + 1] *= k;
        if (fluxX_[z][x] < 0.0f) fluxX_[z][x] *= k;
        if (fluxZ_[z + 1][x] > 0.0f) fluxZ_[z + 1][x] *= k;
        if (fluxZ_[z][x] < 0.0f) fluxZ_[z][x] *= k;
      }
    }
  }
  // Continuity (backward step with the new fluxes).
  for (int z = 0; z < NZ; ++z) {
    for (int x = 0; x < NX; ++x) {
      const float div = (fluxX_[z][x + 1] - fluxX_[z][x]) * invDX + (fluxZ_[z + 1][x] - fluxZ_[z][x]) * invDZ;
      h_[z][x] = std::max(0.0f, h_[z][x] - dt * div);
    }
  }
}

void SloshEffect::smooth(float amount) {
  // Conservative diffusion along x (the fine axis): flux form with zero-flux
  // walls, so the total volume is untouched. amount = nu*dt/dx^2 <= 0.25.
  if (amount <= 0.0f) return;
  amount = std::min(amount, 0.25f);
  for (int z = 0; z < NZ; ++z) {
    float carry = 0.0f;  // flux into cell x from cell x-1
    for (int x = 0; x < NX; ++x) {
      const float next = x + 1 < NX ? amount * (h_[z][x + 1] - h_[z][x]) : 0.0f;
      h_[z][x] += next - carry;
      carry = next;
    }
  }
}

void SloshEffect::renormalize() {
  const float v = totalVolume();
  if (v > 1e-3f) {
    const float s = targetVolume_ / v;
    for (int z = 0; z < NZ; ++z)
      for (int x = 0; x < NX; ++x) h_[z][x] *= s;
  }
}

void SloshEffect::update(const EffectContext& ctx) {
  const Vec3 gEff = ctx.g.effectiveGravity();
  // Vertical jolts: whole-body bob plus a splash proportional to jerk, both
  // driven by the raw vertical g so bumps stay crisp...
  const float vertG = ctx.g.verticalG();
  bob_.step(-vertG * bobPxPerG_, ctx.dt, bobHz_, 0.35f);
  const float jerk = ctx.dt > 0.0f ? std::fabs(vertG - lastVertG_) / ctx.dt : 0.0f;
  lastVertG_ = vertG;
  if (jerk > 2.0f && splashGain_ > 0.0f) {
    const float kick = std::min(jerk, 30.0f) * splashGain_ * ctx.dt;
    for (int z = 0; z < NZ; ++z)
      for (int x = 1; x < NX; ++x) u_[z][x] += kick * rng_.symmetric();
    for (int z = 1; z < NZ; ++z)
      for (int x = 0; x < NX; ++x) w_[z][x] += kick * 0.5f * rng_.symmetric();
  }
  if (ripple_ > 0.0f) {
    for (int i = 0; i < 4; ++i) {
      const int x = 1 + static_cast<int>(rng_.uniform() * (NX - 1));
      const int z = static_cast<int>(rng_.uniform() * NZ);
      u_[z][x] += ripple_ * ctx.dt * rng_.symmetric();
    }
  }
  // ...while the pressure term sees a ~1 Hz low-passed, clamped vertical g so a
  // pothole does not triple the surface slope for one frame.
  gvFiltered_ = lerpf(gvFiltered_, clampf(-gEff.y, 0.2f, 3.0f), ctx.dt / (ctx.dt + 0.16f));
  const float gv = gvFiltered_ * gravityPx();

  // CFL from the current state: c = sqrt(gv * hmax), Courant target 0.7.
  float hmax = 0.0f;
  for (int z = 0; z < NZ; ++z)
    for (int x = 0; x < NX; ++x) hmax = std::max(hmax, h_[z][x]);
  const float c = std::sqrt(gv * std::max(hmax, 1.0f));
  const float dtMax = 0.7f * DX / std::max(c, 1e-3f);
  int n = static_cast<int>(std::ceil(ctx.dt / dtMax));
  n = clampi(n, 1, static_cast<int>(maxSubsteps_));
  lastSubsteps_ = n;
  const float dt = ctx.dt / static_cast<float>(n);
  for (int i = 0; i < n; ++i) {
    substep(dt, gv, gEff);
    smooth(viscosity_ * dt / (DX * DX));
  }
  renormalize();  // only corrects float round-off now that the limiter keeps h >= 0
}

void SloshEffect::render(VoxelFrame& f) {
  const float bottom = static_cast<float>(kH) - 1.0f;
  const float bob = bob_.pos;
  const int thick = std::max(1, iround(surfacePx_));
  const int style = iround(style_);
  for (int z = 0; z < NZ; ++z) {
    for (int px = 0; px < kW; ++px) {
      // Linear interpolation between cell centres along x.
      const float cx = (static_cast<float>(px) + 0.5f) / DX - 0.5f;
      int i0 = ifloor(cx);
      const float t = cx - static_cast<float>(i0);
      i0 = clampi(i0, 0, NX - 1);
      const int i1 = clampi(i0 + 1, 0, NX - 1);
      const float h = lerpf(h_[z][i0], h_[z][i1], t) + bob;
      const int ySurf = iround(bottom - h);
      if (h < 0.5f) continue;
      for (int k = 0; k < thick; ++k) f.set(px, ySurf + k, z);
      if (style == 1) {
        for (int y = ySurf + thick; y <= kH - 1; ++y) f.plot(px, y, z, bodyIntensity_);
      } else if (style == 2) {
        f.vline(z, px, ySurf + thick, kH - 1);
      }
    }
  }
}

}  // namespace gm
