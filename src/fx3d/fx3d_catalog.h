#pragma once
// Every scene, in the order the knob and the previews walk them. A scene is
// built in memory the caller hands in (PSRAM on the panel), so no scene costs
// internal RAM while another one is showing. Plain C++11.

#include <new>

#include "fx3d_scenes_music.h"

namespace fx3d {

struct CatalogEntry {
  const char *id;        // stable: logs, the API, preview files
  const char *name;      // what the knob's banner says
  size_t bytes;          // sizeof the scene
  Scene *(*make)(void *mem);
  uint8_t mode;          // the mode it is shown in first
};

template <class T>
Scene *makeScene(void *mem) {
  return new (mem) T();
}

static const CatalogEntry kCatalog[] = {
    {"calib", "3D CALIBRATE", sizeof(CalibScene), &makeScene<CalibScene>, MODE_RED_BLUE},
    {"cube", "3D CUBE", sizeof(CubeScene), &makeScene<CubeScene>, MODE_RED_BLUE},
    {"layers", "3D LAYERS", sizeof(LayersScene), &makeScene<LayersScene>, MODE_RED_BLUE},
    {"stars", "3D STARS", sizeof(StarsScene), &makeScene<StarsScene>, MODE_RED_BLUE},
    {"helix", "3D HELIX", sizeof(HelixScene), &makeScene<HelixScene>, MODE_RED_BLUE},
    {"rings", "3D RINGS", sizeof(RingsScene), &makeScene<RingsScene>, MODE_RED_BLUE},
    {"dial", "3D DIAL", sizeof(DialScene), &makeScene<DialScene>, MODE_RED_BLUE},
    {"torus", "3D TORUS", sizeof(TorusScene), &makeScene<TorusScene>, MODE_MONO},
    {"vclock", "3D VOXEL CLOCK", sizeof(VoxelClockScene), &makeScene<VoxelClockScene>, MODE_MONO},
    {"voxel", "3D FLY", sizeof(VoxelScene), &makeScene<VoxelScene>, MODE_MONO},
    {"tunnel", "3D TUNNEL", sizeof(TunnelScene), &makeScene<TunnelScene>, MODE_MONO},
    {"blobs", "3D BLOBS", sizeof(BlobsScene), &makeScene<BlobsScene>, MODE_MONO},
    {"globe", "3D GLOBE", sizeof(GlobeScene), &makeScene<GlobeScene>, MODE_MONO},
    {"terrain", "3D SOUND HILLS", sizeof(TerrainScene), &makeScene<TerrainScene>, MODE_MONO},
};
const int kCatalogCount = (int)(sizeof(kCatalog) / sizeof(kCatalog[0]));

inline int catalogFind(const char *id) {
  for (int i = 0; i < kCatalogCount; i++)
    if (!strcmp(kCatalog[i].id, id)) return i;
  return -1;
}

}  // namespace fx3d
