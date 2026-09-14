// The firmware's PCA1 validator on the host (tools/oscmusic/test_clip_maker.py).
//   validate MAX FILE...   one line per file: "<1|0> <frames> FILE"
// MAX 0 is the flash store's animValidatePca; any other MAX goes to
// animValidatePcaMax, the card's rule.
#include <cstdlib>

#include <LittleFS.h>

#include "anim_store.h"

SerialStub Serial;
LittleFSStub LittleFS;

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const unsigned long max = strtoul(argv[1], nullptr, 10);
  for (int i = 2; i < argc; i++) {
    File f(argv[i]);
    PcaHeader h{};
    const bool ok = max ? animValidatePcaMax(f, &h, (uint32_t)max) : animValidatePca(f, &h);
    printf("%d %u %s\n", ok ? 1 : 0, (unsigned)h.frameCount, argv[i]);
  }
  return 0;
}
