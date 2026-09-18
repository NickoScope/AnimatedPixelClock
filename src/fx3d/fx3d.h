#pragma once
// 3D on the panel, mono and anaglyph (docs/27-fx3d.md in the knowledge base):
// scenes, which own the screen while they show, and looks, which show any
// page's own frame in 3D. Behind -DFX3D_ENABLED; a build without the flag must
// not change. See src/fx3d/README.md.

#if defined(FX3D_ENABLED)

void fx3dBegin();                // setup(): the PSRAM frame and /api/fx3d; the bench build arms its bench
void fx3dLoop();                 // every loop() pass: the glasses profile's deferred NVS write, and the bench's schedule
bool fx3dOwnsScreen();           // a scene is showing: it draws every pixel
int fx3dRefreshHz(int pageHz);   // the render tick's rate: 30 for a scene; with a look, at most 30 (30 if it moves)
void fx3dRender();               // the scene, in the render tick after the clear and before the flip
void fx3dStop();                 // the knob was touched: a scene gives the panel back (a look stays)

#endif // FX3D_ENABLED
