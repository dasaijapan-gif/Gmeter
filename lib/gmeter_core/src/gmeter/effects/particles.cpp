#include "particles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gm {

ParticleEffect::ParticleEffect() {
  params_.add("count", &count_, 1.0f, static_cast<float>(kMaxParticles), "particles");
  params_.add("tank_m", &tankWidthM_, 0.1f, 10.0f, "virtual tank width (m)");
  params_.add("drag", &drag_, 0.0f, 10.0f, "air drag 1/s");
  params_.add("bounce", &restitution_, 0.0f, 1.0f, "wall restitution");
  params_.add("pressure", &pressure_, 0.0f, 80000.0f, "repulsion stiffness");
  params_.add("rho0", &restDensity_, 0.0f, 4.0f, "rest density (particles/cell)");
  params_.add("jitter", &jitter_, 0.0f, 200.0f, "random agitation");
  params_.add("dot", &dotSize_, 1.0f, 2.0f, "particle size px");
  params_.add("substeps", &substeps_, 1.0f, 6.0f, "physics substeps");
  reset();
}

float ParticleEffect::gravityPx() const {
  return kStandardGravity * static_cast<float>(kW) / tankWidthM_;
}

void ParticleEffect::reset() {
  const Vec3 he = VoxelFrame::halfExtent();
  rng_.seed(777u);
  for (int i = 0; i < kMaxParticles; ++i) {
    // Start as a loose cloud in the lower half so it visibly settles.
    pos_[i] = Vec3{rng_.range(-he.x * 0.8f, he.x * 0.8f), rng_.range(-he.y * 0.9f, 0.0f),
                   rng_.range(-he.z * 0.8f, he.z * 0.8f)};
    vel_[i] = Vec3{};
  }
}

// World -> continuous density-grid coordinates (padded grid, cell centres at
// integer + 0.5 offsets handled by the 0.5 shift below).
static inline Vec3 gridCoord(const Vec3& world) {
  const Vec3 v = VoxelFrame::worldToVoxel(world);
  return {v.x / ParticleEffect::CELL + 0.5f, v.y / ParticleEffect::CELL + 0.5f, v.z + 1.0f};
}

void ParticleEffect::splatDensity() {
  std::memset(density_, 0, sizeof(density_));
  const int n = activeCount();
  for (int p = 0; p < n; ++p) {
    const Vec3 g = gridCoord(pos_[p]);
    const int i0 = clampi(static_cast<int>(g.x), 0, GX - 2), j0 = clampi(static_cast<int>(g.y), 0, GY - 2), k0 = clampi(static_cast<int>(g.z), 0, GZ - 2);
    const float fx = clampf(g.x - static_cast<float>(i0), 0.0f, 1.0f), fy = clampf(g.y - static_cast<float>(j0), 0.0f, 1.0f), fz = clampf(g.z - static_cast<float>(k0), 0.0f, 1.0f);
    for (int dk = 0; dk < 2; ++dk) {
      const float wz = dk ? fz : 1.0f - fz;
      for (int dj = 0; dj < 2; ++dj) {
        const float wy = dj ? fy : 1.0f - fy;
        for (int di = 0; di < 2; ++di) {
          const float wx = di ? fx : 1.0f - fx;
          density_[k0 + dk][j0 + dj][i0 + di] += wx * wy * wz;
        }
      }
    }
  }
  // Fold mass that landed in the padding back into the edge cells, then
  // mirror the edge cells into the padding so the density gradient at a wall
  // points into the pile (Neumann boundary) instead of toward the empty
  // padding, which would suck particles into the wall.
  for (int k = 0; k < GZ; ++k)
    for (int j = 0; j < GY; ++j) {
      density_[k][j][1] += density_[k][j][0];           density_[k][j][0] = density_[k][j][1];
      density_[k][j][GX - 2] += density_[k][j][GX - 1]; density_[k][j][GX - 1] = density_[k][j][GX - 2];
    }
  for (int k = 0; k < GZ; ++k)
    for (int i = 0; i < GX; ++i) {
      density_[k][1][i] += density_[k][0][i];           density_[k][0][i] = density_[k][1][i];
      density_[k][GY - 2][i] += density_[k][GY - 1][i]; density_[k][GY - 1][i] = density_[k][GY - 2][i];
    }
  for (int j = 0; j < GY; ++j)
    for (int i = 0; i < GX; ++i) {
      density_[1][j][i] += density_[0][j][i];           density_[0][j][i] = density_[1][j][i];
      density_[GZ - 2][j][i] += density_[GZ - 1][j][i]; density_[GZ - 1][j][i] = density_[GZ - 2][j][i];
    }
  // Pressure only where the pile is denser than rest, so it can settle.
  for (int k = 0; k < GZ; ++k)
    for (int j = 0; j < GY; ++j)
      for (int i = 0; i < GX; ++i) pressureField_[k][j][i] = std::max(density_[k][j][i] - restDensity_, 0.0f);
}

Vec3 ParticleEffect::pressureAccel(const Vec3& world) const {
  const Vec3 g = gridCoord(world);
  // Grid coordinates are non-negative here, so truncation is a floor.
  const int i = clampi(static_cast<int>(g.x + 0.5f), 1, GX - 2);
  const int j = clampi(static_cast<int>(g.y + 0.5f), 1, GY - 2);
  const int k = clampi(static_cast<int>(g.z + 0.5f), 1, GZ - 2);
  const float (*p)[GY][GX] = pressureField_;
  // Central differences in cell units, converted to per-pixel.
  const float dx = (p[k][j][i + 1] - p[k][j][i - 1]) * (0.5f / CELL);
  const float dy = (p[k][j + 1][i] - p[k][j - 1][i]) * (0.5f / CELL);
  const float dz = (p[k + 1][j][i] - p[k - 1][j][i]) * (0.5f / kLayerPitchPx);
  // Grid y points down (screen rows); world y points up. a = -k * grad(p).
  // (Dividing by the local density, as SPH does, over-amplifies the force at
  // the sparse top of the pile and turns it into a fountain.)
  return Vec3{-dx, dy, -dz} * pressure_;
}

void ParticleEffect::substep(float dt, const Vec3& gEff) {
  const float gpx = gravityPx();
  const Vec3 gravity = gEff * gpx;
  const Vec3 he = VoxelFrame::halfExtent();
  const Vec3 lim{he.x - 0.5f, he.y - 0.5f, he.z - 0.5f};
  const float dragK = std::max(0.0f, 1.0f - drag_ * dt);
  // No particle may cross more than 0.8 cell per substep (no tunnelling).
  const float vmax = 0.8f * CELL / dt, vmax2 = vmax * vmax;
  const int n = activeCount();
  for (int p = 0; p < n; ++p) {
    Vec3 a = gravity + pressureAccel(pos_[p]);
    if (jitter_ > 0.0f) a += Vec3{rng_.symmetric(), rng_.symmetric(), rng_.symmetric()} * jitter_;
    Vec3& v = vel_[p];
    Vec3& x = pos_[p];
    v = (v + a * dt) * dragK;
    const float v2 = v.length2();
    if (v2 > vmax2) v *= vmax / std::sqrt(v2);
    x += v * dt;
    // Walls.
    if (x.x < -lim.x) { x.x = -lim.x; if (v.x < 0) v.x = -v.x * restitution_; v.y *= 0.9f; v.z *= 0.9f; }
    if (x.x > lim.x)  { x.x = lim.x;  if (v.x > 0) v.x = -v.x * restitution_; v.y *= 0.9f; v.z *= 0.9f; }
    if (x.y < -lim.y) { x.y = -lim.y; if (v.y < 0) v.y = -v.y * restitution_; v.x *= 0.9f; v.z *= 0.9f; }
    if (x.y > lim.y)  { x.y = lim.y;  if (v.y > 0) v.y = -v.y * restitution_; v.x *= 0.9f; v.z *= 0.9f; }
    if (x.z < -lim.z) { x.z = -lim.z; if (v.z < 0) v.z = -v.z * restitution_; v.x *= 0.9f; v.y *= 0.9f; }
    if (x.z > lim.z)  { x.z = lim.z;  if (v.z > 0) v.z = -v.z * restitution_; v.x *= 0.9f; v.y *= 0.9f; }
  }
}

void ParticleEffect::update(const EffectContext& ctx) {
  const Vec3 gEff = ctx.g.effectiveGravity();
  const int steps = std::max(1, iround(substeps_));
  const float dt = ctx.dt / static_cast<float>(steps);
  for (int s = 0; s < steps; ++s) {
    splatDensity();
    substep(dt, gEff);
  }
}

void ParticleEffect::render(VoxelFrame& f) {
  const int n = activeCount();
  const bool big = dotSize_ >= 1.5f;
  for (int p = 0; p < n; ++p) {
    const Vec3 v = VoxelFrame::worldToVoxel(pos_[p]);
    const int x = iround(v.x), y = iround(v.y), z = iround(v.z);
    f.set(x, y, z);
    if (big) { f.set(x + 1, y, z); f.set(x, y + 1, z); f.set(x + 1, y + 1, z); }
  }
}

}  // namespace gm
