/*
 * AnimatedPixelClock - Clip stream
 *
 * See clip_stream.h.
 */

#include "clip_stream.h"

static const uint32_t SLOTS = CLIP_RING_FRAMES + 1;  // head == tail means empty

void ClipStream::init(uint8_t* r, uint16_t* d, uint32_t m) {
  ring = r;
  delays = d;
  maxFrames = m;
}

bool ClipStream::begin(File& f, const PcaHeader& h) {
  head = 0;
  tail = 0;
  reads = readUsMax = underruns = loops = 0;
  readUsTotal = 0;
  if (!ring || !delays || h.frameCount < 1 || h.frameCount > maxFrames) return false;
  hdr = h;
  uint8_t raw[PCA_MAX_PALETTE * 2];
  const size_t palBytes = (size_t)h.paletteLen * 2;
  const size_t tableBytes = (size_t)h.frameCount * 2;
  if (!f.seek(PCA_HEADER_BYTES) || f.read(raw, palBytes) != palBytes ||
      f.read((uint8_t*)delays, tableBytes) != tableBytes) {
    return false;
  }
  for (int i = 0; i < PCA_MAX_PALETTE; i++)
    pal[i] = i < h.paletteLen ? (uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8) : 0;
  // The flash player's clamp, so a clip plays at the same pace from either store.
  for (uint32_t i = 0; i < h.frameCount; i++) {
    if (delays[i] < 34) delays[i] = 34;
    if (delays[i] > 5000) delays[i] = 5000;
  }
  frame0 = PCA_HEADER_BYTES + palBytes + tableBytes;
  next = 0;
  inPlace = true;  // the delay table ends where frame 0 begins
  return true;
}

uint32_t ClipStream::queued() const {
  const uint32_t h = head, t = tail;
  return (h + SLOTS - t) % SLOTS;
}

bool ClipStream::fill(File& f, int budget) {
  while (budget-- > 0) {
    const uint32_t h = head;
    if ((h + 1) % SLOTS == tail) return true;  // full
    const unsigned long t0 = micros();
    // Frames are read in order, so the only seek is the one back to frame 0:
    // no FAT cluster walk per frame (fast seek is off in this IDF's FATFS).
    if (!inPlace && !f.seek(frame0 + next * PCA_FRAME_BYTES)) return false;
    if (f.read(ring + h * PCA_FRAME_BYTES, PCA_FRAME_BYTES) != PCA_FRAME_BYTES) return false;
    const uint32_t us = (uint32_t)(micros() - t0);
    reads++;
    readUsTotal += us;
    if (us > readUsMax) readUsMax = us;
    ringDelay[h] = delays[next];
    if (++next == hdr.frameCount) {
      next = 0;
      loops++;
      inPlace = false;
    } else {
      inPlace = true;
    }
    head = (h + 1) % SLOTS;  // publish last, when the slot is complete
  }
  return true;
}

bool ClipStream::take(uint8_t* frame, uint16_t* delayMs) {
  const uint32_t t = tail;
  if (t == head) return false;
  memcpy(frame, ring + t * PCA_FRAME_BYTES, PCA_FRAME_BYTES);
  *delayMs = ringDelay[t];
  tail = (t + 1) % SLOTS;
  return true;
}
