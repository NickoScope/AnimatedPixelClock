/*
 * AnimatedPixelClock - Clip stream
 *
 * A PCA1 clip played from a file too long to cache: the reader side reads
 * frames ahead into a ring, the render side takes the next one when it is
 * due. Written against an Arduino File and micros() only, so the host test
 * (tools/oscmusic/test_clip_maker.py) runs both sides on threads; the card,
 * the FreeRTOS task and the request handshake live in clip_sd.cpp.
 *
 * Single producer, single consumer. `head` is written by the reader only and
 * `tail` by the render side only; each is a 32-bit load or store, one
 * instruction, so neither side takes a lock. begin() resets both and runs
 * only while the render side is not taking - clip_sd.cpp gates that.
 */

#ifndef CLIP_STREAM_H
#define CLIP_STREAM_H

#include <Arduino.h>
#include <FS.h>

#include "../ambient/anim_store.h"

#if defined(CLIP_HOST_TEST)
#include <atomic>
#define CLIP_INDEX std::atomic<uint32_t>
#else
#define CLIP_INDEX volatile uint32_t
#endif

// One second in hand at 25 fps. Measured on the panel 2026-09-14: 4096-byte
// reads from the card took 2.59 ms on average and 6.44 ms at worst over 2048
// reads, and a frame is due every 40 ms, so a full ring rides out a stall of
// about 150 worst reads. The ring holds one slot more than this (an empty
// slot tells full from empty): (25 + 1) x 4096 B = 104 KB of PSRAM.
#define CLIP_RING_FRAMES 25

class ClipStream {
 public:
  // Caller-owned buffers: `ring` (CLIP_RING_FRAMES + 1) * PCA_FRAME_BYTES,
  // `delays` maxFrames entries.
  void init(uint8_t* ring, uint16_t* delays, uint32_t maxFrames);

  // Reader side. begin() takes an open file whose header animValidatePcaMax
  // accepted, reads the palette and the delay table and empties the ring.
  // fill() reads up to `budget` frames while the ring has room, going back to
  // frame 0 after the last. Both return false on a short read or a failed seek.
  bool begin(File& f, const PcaHeader& hdr);
  bool fill(File& f, int budget);

  // Render side: the next frame and its delay, or false when the ring is
  // empty - an underrun, and the caller keeps the frame it has.
  bool take(uint8_t* frame, uint16_t* delayMs);
  void missed() { underruns++; }

  const uint16_t* palette() const { return pal; }
  uint16_t frames() const { return hdr.frameCount; }
  uint32_t queued() const;

  // Since begin(): frame reads, their total and worst time in microseconds,
  // underruns (counted by the render side), passes back to frame 0.
  uint32_t reads = 0, readUsMax = 0, underruns = 0, loops = 0;
  uint64_t readUsTotal = 0;

 private:
  uint8_t* ring = nullptr;
  uint16_t* delays = nullptr;
  uint32_t maxFrames = 0;
  uint16_t ringDelay[CLIP_RING_FRAMES + 1];
  PcaHeader hdr{};
  uint16_t pal[PCA_MAX_PALETTE];
  uint32_t frame0 = 0;   // file offset of frame 0
  uint32_t next = 0;     // reader: the frame the next read returns
  bool inPlace = false;  // reader: the file position is at frame `next`
  CLIP_INDEX head{0}, tail{0};
};

#endif  // CLIP_STREAM_H
