#pragma once
// A 3D effect for the panel. Behind -DFX3D_ENABLED; a build without the flag must not change.
// See src/fx3d/README.md.

#if defined(FX3D_ENABLED)

void fx3dBegin();      // setup(), after the display is up
void fx3dTick();       // every render tick, cheap when it has nothing to draw

#endif // FX3D_ENABLED
