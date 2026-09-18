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
};

template <class T>
Scene *makeScene(void *mem) {
  return new (mem) T();
}

static const CatalogEntry kCatalog[] = {
    {"calib", "3D CALIBRATE", sizeof(CalibScene), &makeScene<CalibScene>},
    {"cube", "3D CUBE", sizeof(CubeScene), &makeScene<CubeScene>},
    {"layers", "3D LAYERS", sizeof(LayersScene), &makeScene<LayersScene>},
    {"stars", "3D STARS", sizeof(StarsScene), &makeScene<StarsScene>},
    {"helix", "3D HELIX", sizeof(HelixScene), &makeScene<HelixScene>},
    {"rings", "3D RINGS", sizeof(RingsScene), &makeScene<RingsScene>},
    {"dial", "3D DIAL", sizeof(DialScene), &makeScene<DialScene>},
    {"torus", "3D TORUS", sizeof(TorusScene), &makeScene<TorusScene>},
    {"vclock", "3D VOXEL CLOCK", sizeof(VoxelClockScene), &makeScene<VoxelClockScene>},
    {"voxel", "3D FLY", sizeof(VoxelScene), &makeScene<VoxelScene>},
    {"tunnel", "3D TUNNEL", sizeof(TunnelScene), &makeScene<TunnelScene>},
    {"blobs", "3D BLOBS", sizeof(BlobsScene), &makeScene<BlobsScene>},
    {"globe", "3D GLOBE", sizeof(GlobeScene), &makeScene<GlobeScene>},
    {"terrain", "3D SOUND HILLS", sizeof(TerrainScene), &makeScene<TerrainScene>},
};
const int kCatalogCount = (int)(sizeof(kCatalog) / sizeof(kCatalog[0]));

inline int catalogFind(const char *id) {
  for (int i = 0; i < kCatalogCount; i++)
    if (!strcmp(kCatalog[i].id, id)) return i;
  return -1;
}

}  // namespace fx3d
