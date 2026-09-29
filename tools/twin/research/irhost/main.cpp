// Host check: does IRremoteESP8266 2.9.0 (UNIT_TEST build) decode a TSOP-like
// demodulated NEC waveform, as the firmware's gpio_intr() would record it
// (rawbuf[0]=1, then alternating mark/space durations in 2-us ticks)?
#include <cstdio>
#include <vector>
#include "IRrecv.h"
#include "IRutils.h"
static std::vector<uint32_t> necUs(uint32_t v, bool repeat) {
  std::vector<uint32_t> d;  // mark, space, mark, ... (ends with the footer mark)
  if (repeat) { d = {8960, 2240, 560}; return d; }
  d.push_back(8960); d.push_back(4480);
  for (int i = 31; i >= 0; i--) { d.push_back(560); d.push_back(((v >> i) & 1) ? 1680 : 560); }
  d.push_back(560);
  return d;
}
static void run(IRrecv &rx, uint32_t v, bool rep, int stretch, int latency_jitter) {
  std::vector<uint32_t> d = necUs(v, rep);
  static uint16_t raw[300];
  raw[0] = 1;
  for (size_t i = 0; i < d.size(); i++) {
    int us = (int)d[i] + ((i % 2 == 0) ? stretch : -stretch) + ((i % 3) ? latency_jitter : -latency_jitter);
    raw[i + 1] = (uint16_t)(us / kRawTick);
  }
  decode_results r;
  r.rawbuf = raw; r.rawlen = d.size() + 1; r.overflow = false;
  raw[r.rawlen] = 0;
  bool ok = rx.decode(&r);
  printf("value=0x%06X repeat=%d stretch=%+4d us jitter=%3d -> ok=%d type=%s value=0x%llX repeat=%d bits=%u cmd=0x%X addr=0x%X\n",
         v, rep, stretch, latency_jitter, ok, typeToString(r.decode_type).c_str(),
         (unsigned long long)r.value, r.repeat, r.bits, r.command, r.address);
}
int main() {
  IRrecv rx(0, 256, 15, false);
  rx.setUnknownThreshold(12);
  const uint32_t codes[] = {0xFF708F, 0xFF58A7, 0xFFE01F, 0xFF41BE, 0xFF28D7, 0xFFC03F, 0xFF19E6, 0xFF12ED, 0xFF40BF, 0xFFC936};
  for (uint32_t c : codes) run(rx, c, false, 50, 0);
  run(rx, 0xFF708F, true, 50, 0);
  // TSOP 82460 Fig.1 bounds at 38 kHz: tpo in [tpi - 3.0/f0, tpi + 3.5/f0] = [-79, +92] us
  for (int s : {-79, 0, 92, 150, 200}) run(rx, 0xFF708F, false, s, 0);
  run(rx, 0xFF708F, false, 92, 40);
  return 0;
}
