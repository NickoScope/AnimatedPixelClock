/*
 * Engine: the state and buffers of styles 7-14, reset, frame updates and the
 * render dispatch. Wow.__init__, update() and step() of effects_wow.py.
 */
#if defined(VIZ_WOW_ENABLED) || !defined(ARDUINO)

#include <cstring>
#include <new>

#include "wow_internal.h"

namespace wow {

const char *const kNames[kEffects] = {"Prism EQ",       "Neon Mirror+",    "Spectrogram", "Radial Bloom",
                                      "Beat Particles", "Scope Afterglow", "Twin VU",     "Synthwave Grid"};

bool Engine::begin(AllocFn alloc) {
  if (s_) return true;
  void *mem = alloc(sizeof(State));
  if (!mem) return false;
  State *s = new (mem) State();
  s->cols = static_cast<uint8_t *>(alloc((size_t)kW * kH));
  s->rgb = static_cast<float *>(alloc((size_t)kW * kH * 3 * sizeof(float)));
  s->parts = static_cast<Particle *>(alloc((size_t)kMaxParts * sizeof(Particle)));
  s->glow = static_cast<float *>(alloc((size_t)kW * kH * sizeof(float)));
  if (!s->cols || !s->rgb || !s->parts || !s->glow) return false;
  for (int i = 0; i < 64; i++) s->lut[i] = col(inferno(i / R(63.0)));
  s_ = s;
  reset(PRISM_EQ, R(1.0));
  return true;
}

int Engine::effect() const { return s_ ? s_->effect : -1; }

void Engine::setReact(real react) {
  if (s_) s_->react = react;
}

void Engine::reset(int effect, real react) {
  if (!s_) return;
  State &s = *s_;
  s.effect = (effect >= 0 && effect < kEffects) ? effect : PRISM_EQ;
  s.react = react;
  s.have = false;
  s.beatEnv = s.hue = s.hueTarget = R(0.0);
  s.rng = XorShift32();
  switch (s.effect) {
    case SPECTROGRAM:
      std::memset(s.cols, 0, (size_t)kW * kH);
      std::memset(s.ticks, 0, sizeof(s.ticks));
      s.head = 0;
      break;
    case RADIAL_BLOOM:
      s.rot = R(0.0);
      s.nRings = 0;
      break;
    case BEAT_PARTICLES:
      for (size_t i = 0; i < (size_t)kW * kH * 3; i++) s.rgb[i] = 0.0f;
      s.nParts = 0;
      // Python builds the list left to right: x, y, speed per star.
      for (Star &st : s.stars) {
        st.x = s.rng.uniform(R(0), R(kW));
        st.y = s.rng.uniform(R(0), R(kH));
        st.speed = s.rng.uniform(R(0.2), R(1.0));
      }
      break;
    case SCOPE_AFTERGLOW:
      for (size_t i = 0; i < (size_t)kW * kH; i++) s.glow[i] = 0.0f;
      break;
    case TWIN_VU:
      for (int m = 0; m < 2; m++) {
        s.pos[m] = s.vel[m] = R(0.0);
        s.nGhosts[m] = 0;
        s.norm[m] = R(0.3);
      }
      break;
    case SYNTHWAVE:
      s.offset = R(0.0);
      break;
    default:
      break;
  }
}

void Engine::update(const VizFrame &f) {
  if (!s_) return;
  State &s = *s_;
  s.f = f;
  s.have = true;
  if (f.beat && s.react > 0) {
    s.beatEnv = std::max(s.beatEnv, (R(0.55) + R(0.45) * R(f.strength)) * s.react);
    s.hueTarget += R(0.07) * s.react;
    if (s.hueTarget >= R(64.0)) {
      s.hueTarget -= R(64.0);
      s.hue -= R(64.0);
    }
    if (s.effect == RADIAL_BLOOM) beatRadialBloom(s);
    if (s.effect == BEAT_PARTICLES) beatParticles(s);
  }
  if (s.effect == SPECTROGRAM) updateSpectrogram(s);
}

void Engine::render(Canvas &cv, real dt) {
  if (!s_ || !s_->have) return;
  State &s = *s_;
  switch (s.effect) {
    case PRISM_EQ: renderPrismEq(s, cv, dt); break;
    case NEON_MIRROR_PLUS: renderNeonMirrorPlus(s, cv, dt); break;
    case SPECTROGRAM: renderSpectrogram(s, cv, dt); break;
    case RADIAL_BLOOM: renderRadialBloom(s, cv, dt); break;
    case BEAT_PARTICLES: renderBeatParticles(s, cv, dt); break;
    case SCOPE_AFTERGLOW: renderScopeAfterglow(s, cv, dt); break;
    case TWIN_VU: renderTwinVu(s, cv, dt); break;
    default: renderSynthwave(s, cv, dt); break;
  }
}

void step(State &s, real dt) {
  s.beatEnv *= std::exp(-dt / R(0.18));
  s.hue += (s.hueTarget - s.hue) * (R(1.0) - std::exp(-dt / R(0.25)));
}

}  // namespace wow

#endif
