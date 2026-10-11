// Death Stranding's hooks into the Astra foundation's AstraPassthrough.fx (the list is at the top of that effect).
// It goes into DS's reshade-shaders\Shaders\ in place of the foundation's empty AstraGame.fxh.

// DS draws its UI into a target of its own and composites it with the scene in one pass: the shared ui code
// (extern/minecraft-in-death-stranding/ds/src/ui.cpp, compiled into astra-ds.addon64) finds it every frame, binds it
// to every effect's DSUI texture and sets every effect's DsUiActive - so DS's UI goes back over her.
#define ASTRA_GAME_UI DSUI
#define ASTRA_GAME_UI_ACTIVE DsUiActive

// Minecraft in Death Stranding (MCPassthrough.fx), when it is installed too, leaves its blocks' coverage and depth in
// McInfoTex: she hides behind them (astra-ds.addon64 sets AstraOccluderActive while it composites).
#define ASTRA_OCCLUDER_TEX McInfoTex
