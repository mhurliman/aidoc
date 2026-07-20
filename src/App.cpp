#include "App.h"
#include "Renderer.h"
#include "IRenderer.h"
#include "assets/Mesh.h"
#include "assets/Material.h"
#include "util/Assert.h"
#include "water/FFTWaterSurface.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <windowsx.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

using namespace DirectX;

App::App() = default;
App::~App() = default;

// Resolve a relative path against the directory containing the executable.
static std::string ResolveExePath(const std::string& relativePath)
{
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    auto pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
    {
        dir = dir.substr(0, pos + 1);
    }
    return dir + relativePath;
}

// Directory containing the executable, as a wide string ending in a separator.
static std::wstring ResolveExeDirW()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        dir = dir.substr(0, pos + 1);
    return dir;
}

// -------------------------------------------------------------------------------------------------
// Buoyancy wave query.
//
// The managed sim calls App_SampleWaveHeight ~thousands of times per 500 Hz tick (once per hull
// mesh vertex). SampleHeightCPU is far too expensive to run per-call at that rate, so instead we
// keep a small heightfield around the boat, refreshed once per render frame (FillWaveCache), and
// bilinear-sample it here. A lock-free double buffer lets the sim thread read while the render
// thread fills. Points outside the grid clamp to the edge (the boat stays well inside it).
// -------------------------------------------------------------------------------------------------
namespace
{
    struct WaveCache
    {
        static constexpr int G = 32;      // grid samples per side
        float side = 16.0f;               // world-space span (m), centred on the boat
        float heights[G * G] = {};
        float originX = 0.0f, originZ = 0.0f;  // world XZ of cell (0,0)
        float cell = 16.0f / (G - 1);
    };

    // Debug wave-match overlay: an independent grid + a HIGH mode count so it reproduces the exact
    // surface the GPU renders (the physics cache above uses modes=8, height-only). 64 modes ≈ full
    // spectrum given the Phillips 1/k⁴ falloff; placed at displaced positions like the GPU VS.
    constexpr int   kDebugGrid  = 16;      // tiles per side over kDebugSide metres
    constexpr int   kDebugModes = 32;
    constexpr float kDebugSide  = 16.0f;

    // Bilinear-sample one of the cache grids at world (wx,wz), clamping to the edge.
    float SampleGrid(const WaveCache* wc, const float* grid, float wx, float wz)
    {
        constexpr int G = WaveCache::G;
        float fx = (wx - wc->originX) / wc->cell;
        float fz = (wz - wc->originZ) / wc->cell;
        if (fx < 0.0f) fx = 0.0f; else if (fx > G - 1) fx = (float)(G - 1);
        if (fz < 0.0f) fz = 0.0f; else if (fz > G - 1) fz = (float)(G - 1);
        int x0 = (int)fx, z0 = (int)fz;
        int x1 = x0 < G - 1 ? x0 + 1 : x0;
        int z1 = z0 < G - 1 ? z0 + 1 : z0;
        float tx = fx - x0, tz = fz - z0;
        float h00 = grid[z0 * G + x0], h10 = grid[z0 * G + x1];
        float h01 = grid[z1 * G + x0], h11 = grid[z1 * G + x1];
        float h0 = h00 + (h10 - h00) * tx;
        float h1 = h01 + (h11 - h01) * tx;
        return h0 + (h1 - h0) * tz;
    }

    FFTWaterSurface*          g_waveSurface = nullptr;
    WaveCache                 g_waveCacheA, g_waveCacheB;
    std::atomic<WaveCache*>   g_waveFront{ &g_waveCacheA };
    WaveCache*                g_waveBack = &g_waveCacheB;

    // Refresh the back buffer around (cx,cz) and publish it. Called on the render thread.
    void FillWaveCache(float cx, float cz, float elapsed)
    {
        FFTWaterSurface* w = g_waveSurface;
        if (!w) return;

        WaveCache* wc = g_waveBack;
        wc->cell = wc->side / (WaveCache::G - 1);
        wc->originX = cx - wc->side * 0.5f;
        wc->originZ = cz - wc->side * 0.5f;
        // Batched fill over the whole grid. modes=8 already spans the full 512→1 m band (the cascade
        // tiles are 8× apart, so each reaches the next one's size); modes=16 adds the decorrelated
        // cross-cascade mid-band detail (each cascade down to tile/16) that the GPU's full 256-mode
        // spectrum has but modes=8 dropped — narrowing the physics-vs-render gap. Cost scales as
        // modes²: 16 is ~4× modes=8 (≈5 ms Release / ~33 ms Debug); 32 measured ~128 ms in Debug, too
        // much for marginal gain. The 0.5 m cache grid caps representable detail at ~1 m anyway, and
        // the dominant remaining gap to the render is the choppiness displacement, not modes.
        constexpr int kWaveCacheModes = 16;
        w->SampleHeightGrid(wc->originX, wc->originZ, wc->cell, WaveCache::G,
                            elapsed, kWaveCacheModes, wc->heights);

        // Publish: swap front/back (single producer, single consumer).
        g_waveBack = g_waveFront.exchange(wc, std::memory_order_acq_rel);
    }
}

// Wave-height callback handed to the managed sim (cdecl). worldX/worldZ are the sim's horizontal
// (x, y) mapped straight through (sim Z-up → aidoc Y-up shares the same horizontal plane); the
// returned scalar is the surface elevation. The sim's own time arg is ignored — the surface is
// locked to the render clock captured at fill time so physics and visuals share one wave phase.
extern "C" float __cdecl App_SampleWaveHeight(float worldX, float worldZ, float /*t*/)
{
    const WaveCache* wc = g_waveFront.load(std::memory_order_acquire);
    if (!wc) return 0.0f;

    // Eulerian height at the query point. Choppiness only moves water horizontally (volume-conserving),
    // so the mean level here matches the rendered surface — the boat floats at the right height; only
    // the crest *phase* is subtly shifted. (Inverting the displacement to chase choppy crests inflated
    // the sampled height near crests and popped the boat into the air, so we don't.)
    return SampleGrid(wc, wc->heights, worldX, worldZ);
}

void App::Init(HWND hwnd)
{
    InitDevice(hwnd);
    InitSwapChain(hwnd);
    InitBuffers();
    InitFence();
    InitViewport();

    m_renderer = std::make_unique<Renderer>();
    m_renderer->Init(m_device.Get(), m_commandQueue.Get(), FrameCount);

    LoadScene();
    InitImGui(hwnd);
    InitSim();

    float aspectRatio = static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight);
    m_camera.SetPerspective(XM_PIDIV4, aspectRatio, 0.1f, 1000.0f);
    m_camera.SetPosition({0.0f, 3.0f, -8.0f});
}

void App::InitSim()
{
    if (!m_sim.Init(ResolveExeDirW()))
    {
        fprintf(stderr, "[sim] runtime init failed; boat physics disabled.\n");
        return;
    }
    if (!m_sim.Create(&App_SampleWaveHeight))
    {
        fprintf(stderr, "[sim] create failed; boat physics disabled.\n");
        return;
    }

    // Build the boat entity from the sim's actual hull mesh (already in metres, renderer Y-up body
    // frame). This is the same geometry the buoyancy uses, so the visible hull matches the physics.
    std::vector<float> hullPos, hullNrm;
    std::vector<uint32_t> hullIdx;
    if (m_sim.GetHullMesh(hullPos, hullNrm, hullIdx) && !hullIdx.empty())
    {
        m_hullMat = m_renderer->GetResourceManager().CreateMaterial("boat_hull");
        m_hullMat->baseColorFactor = { 0.85f, 0.86f, 0.90f, 1.0f };  // light hull grey (editable)
        m_hullMat->roughness  = 0.45f;
        m_hullMat->metallic   = 0.0f;
        m_hullMat->doubleSided = true;   // closed hull; skip winding concerns from the frame conversion
        m_hullMat->shadingModel = ShadingModel::PBR;
        m_hullMat->CreatePSO(m_device.Get());

        auto hullMesh = std::make_shared<Mesh>();
        hullMesh->CreateFromArrays(hullPos.data(), hullNrm.data(),
                                   static_cast<uint32_t>(hullPos.size() / 3),
                                   hullIdx.data(), static_cast<uint32_t>(hullIdx.size()),
                                   m_hullMat, m_device.Get());

        Entity boat;
        boat.mesh = hullMesh;
        boat.visible = true;
        XMStoreFloat4x4(&boat.worldTransform, XMMatrixIdentity());
        m_scene.GetEntities().push_back(boat);
        m_boatEntityIndex = static_cast<int>(m_scene.GetEntities().size()) - 1;
        m_boatScale = 1.0f;   // hull mesh is already in metres
        fprintf(stderr, "[sim] hull mesh: %zu verts, %zu tris\n",
                hullPos.size() / 3, hullIdx.size() / 3);
    }
    else
    {
        fprintf(stderr, "[sim] no hull mesh available; boat not shown.\n");
    }

    // Rig mesh (sail + spars + lines + foils): fixed topology, vertices rebuilt each frame.
    int rigVerts = 0, sailIdx = 0, sparIdx = 0, lineIdx = 0, foilIdx = 0, markerIdx = 0, telltaleIdx = 0;
    m_sim.GetRigInfo(rigVerts, sailIdx, sparIdx, lineIdx, foilIdx, markerIdx, telltaleIdx);
    if (rigVerts > 0 && (sailIdx + sparIdx + lineIdx + foilIdx + markerIdx + telltaleIdx) > 0)
    {
        std::vector<uint32_t> indices(static_cast<size_t>(sailIdx + sparIdx + lineIdx + foilIdx + markerIdx + telltaleIdx));
        m_sim.GetRigIndices(indices);

        auto makeMat = [&](const char* key, XMFLOAT4 color, float rough, bool dbl, bool blend)
        {
            auto m = m_renderer->GetResourceManager().CreateMaterial(key);
            m->baseColorFactor = color;
            m->roughness = rough;
            m->metallic = 0.0f;
            m->doubleSided = dbl;    // avoids winding fixes from the sim→render reflection
            m->alphaBlend = blend;
            m->shadingModel = ShadingModel::PBR;
            m->CreatePSO(m_device.Get());
            return m;
        };
        m_sailMat    = makeMat("rig_sail", { 0.95f, 0.95f, 0.93f, 0.82f }, 0.75f, true, true);  // translucent
        auto sparMat = makeMat("rig_spar", { 0.26f, 0.26f, 0.29f, 1.0f },  0.4f,  true, false); // dark grey
        auto lineMat = makeMat("rig_line", { 0.92f, 0.80f, 0.30f, 1.0f },  0.6f,  true, false); // yellow rope
        m_foilMat    = makeMat("rig_foil", { 0.18f, 0.42f, 0.34f, 1.0f },  0.5f,  true, false); // teal foils
        m_markerMat  = makeMat("rig_marker", { 0.95f, 0.35f, 0.08f, 1.0f }, 0.5f, true, false); // orange gizmos
        m_telltaleMat= makeMat("rig_telltale", { 0.90f, 0.10f, 0.10f, 1.0f }, 0.7f, true, false); // red leech telltales

        const uint32_t sailStart     = 0u;
        const uint32_t sparStart     = static_cast<uint32_t>(sailIdx);
        const uint32_t lineStart     = static_cast<uint32_t>(sailIdx + sparIdx);
        const uint32_t foilStart     = static_cast<uint32_t>(sailIdx + sparIdx + lineIdx);
        const uint32_t markerStart   = static_cast<uint32_t>(sailIdx + sparIdx + lineIdx + foilIdx);
        const uint32_t telltaleStart = static_cast<uint32_t>(sailIdx + sparIdx + lineIdx + foilIdx + markerIdx);

        auto& rig = m_scene.GetRig();
        rig.indices = std::move(indices);
        // Opaque parts first; the translucent sail is drawn last so it blends over them.
        rig.submeshes = {
            { sparStart,     static_cast<uint32_t>(sparIdx),     sparMat },
            { lineStart,     static_cast<uint32_t>(lineIdx),     lineMat },
            { foilStart,     static_cast<uint32_t>(foilIdx),     m_foilMat },
            { markerStart,   static_cast<uint32_t>(markerIdx),   m_markerMat },
            { telltaleStart, static_cast<uint32_t>(telltaleIdx), m_telltaleMat },
            { sailStart,     static_cast<uint32_t>(sailIdx),     m_sailMat },
        };
        rig.vertices.resize(static_cast<size_t>(rigVerts));
        rig.visible = true;
        m_rigVertexCount = rigVerts;
        m_rigPos.resize(static_cast<size_t>(rigVerts) * 3);
        m_rigNrm.resize(static_cast<size_t>(rigVerts) * 3);
        fprintf(stderr, "[sim] rig mesh: %d verts (sail %d spar %d line %d foil %d marker %d telltale %d)\n",
                rigVerts, sailIdx, sparIdx, lineIdx, foilIdx, markerIdx, telltaleIdx);

        // Debug overlay: a grid of small tiles at the CPU-sampled buoyancy heights, to compare with
        // the GPU-rendered surface. Fixed quad-tile topology; positions stream each frame (hidden
        // until toggled). WaveCache::G source samples, decimated by 2 → (G/2)² tiles.
        // Two overlaid grids: GREEN = the physics surface (exactly what buoyancy samples), MAGENTA =
        // the GPU/render surface. The boat should sit on GREEN; the green→magenta gap is the disparity.
        m_waveDebugMat = makeMat("wave_gpu",  { 1.0f, 0.10f, 0.90f, 1.0f }, 0.6f, true, false); // magenta
        m_physSurfMat  = makeMat("wave_phys", { 0.15f, 0.95f, 0.25f, 1.0f }, 0.6f, true, false); // green
        {
            const int tiles = kDebugGrid * kDebugGrid;
            m_dbgH.resize(tiles); m_dbgDx.resize(tiles); m_dbgDz.resize(tiles);
            auto& dbg = m_scene.GetDebug();
            dbg.vertices.resize(static_cast<size_t>(tiles) * 4 * 2);   // [physics tiles][gpu tiles]
            dbg.indices.clear();
            dbg.indices.reserve(static_cast<size_t>(tiles) * 6 * 2);
            for (int t = 0; t < tiles * 2; ++t)
            {
                uint32_t b = static_cast<uint32_t>(t) * 4;
                dbg.indices.push_back(b + 0); dbg.indices.push_back(b + 1); dbg.indices.push_back(b + 2);
                dbg.indices.push_back(b + 0); dbg.indices.push_back(b + 2); dbg.indices.push_back(b + 3);
            }
            uint32_t perSurf = static_cast<uint32_t>(tiles) * 6;
            dbg.submeshes = {
                { 0u,      perSurf, m_physSurfMat },   // physics (green): verts [0, tiles*4)
                { perSurf, perSurf, m_waveDebugMat },  // gpu (magenta): verts [tiles*4, 2*tiles*4)
            };
            dbg.visible = false;
        }
    }

    // Prime the wave cache once so the opening ticks float on a real surface, not flat water.
    FillWaveCache(0.0f, 0.0f, m_elapsedTime);
    m_sim.Start();
    fprintf(stderr, "[sim] started (500 Hz thread).\n");
}

void App::UpdateBoatFromSim()
{
    if (!m_sim.SimReady())
        return;
    auto& entities = m_scene.GetEntities();
    if (m_boatEntityIndex < 0 || m_boatEntityIndex >= static_cast<int>(entities.size()))
        return;

    float t = 0.0f;
    m_sim.GetPose(m_boatPos, m_boatQuat, &t);
    const float* p = m_boatPos;
    const float* q = m_boatQuat;

    // Refresh the local wave cache centred on the boat (sim x,y → aidoc x,z) at the render clock.
    FillWaveCache(p[0], p[1], m_elapsedTime);

    // Map sim (right-handed, Z-up) → render (left-handed, Y-up):
    //   position:    (x, y, z) → (x, z, y)
    //   orientation: q(x,y,z,w) → (-x, -z, -y, w)   [conjugation by the Y↔Z basis swap]
    XMFLOAT3 renderPos = { p[0], p[2], p[1] };
    XMVECTOR qRender = XMVectorSet(-q[0], -q[2], -q[1], q[3]);
    XMMATRIX scale = XMMatrixScaling(m_boatScale, m_boatScale, m_boatScale);
    XMMATRIX rot   = XMMatrixRotationQuaternion(qRender);
    XMMATRIX trans = XMMatrixTranslation(renderPos.x, renderPos.y, renderPos.z);
    XMStoreFloat4x4(&entities[m_boatEntityIndex].worldTransform, scale * rot * trans);
}

// Rebuild the dynamic rig mesh (sail + spars + lines) from the sim each frame, interleaving the
// streamed positions/normals into the Scene's dynamic vertex buffer.
void App::UpdateRigMesh(float dt)
{
    if (m_rigVertexCount <= 0 || !m_sim.SimReady())
        return;
    m_sim.GetRigVertices(dt, m_rigPos, m_rigNrm);
    auto& verts = m_scene.GetRig().vertices;
    for (int i = 0; i < m_rigVertexCount; ++i)
    {
        PbrVertex& v = verts[i];
        v.position[0] = m_rigPos[3 * i + 0];
        v.position[1] = m_rigPos[3 * i + 1];
        v.position[2] = m_rigPos[3 * i + 2];
        v.normal[0] = m_rigNrm[3 * i + 0];
        v.normal[1] = m_rigNrm[3 * i + 1];
        v.normal[2] = m_rigNrm[3 * i + 2];
        v.uv[0] = v.uv[1] = 0.0f;
    }
}

// Fill the debug overlay with small magenta tiles that reproduce the EXACT surface the GPU renders:
// a high-mode height + horizontal choppiness displacement, with each tile placed at the displaced
// world position (grid + disp) just like the water vertex shader. If these tiles lie exactly on the
// GPU water, the CPU sampling math is confirmed 1:1 (and any physics disparity is purely from the
// cheaper modes=8/no-displacement approximation the buoyancy path deliberately uses).
void App::UpdateDebugWaveMesh()
{
    auto& dbg = m_scene.GetDebug();
    dbg.visible = m_showWaveDebug;
    if (!m_showWaveDebug || dbg.vertices.empty())
        return;

    constexpr int   DG    = kDebugGrid;
    const int       tiles = DG * DG;
    const float     cell  = kDebugSide / (DG - 1);
    const float     ox    = m_boatPos[0] - kDebugSide * 0.5f;   // sim x
    const float     oz    = m_boatPos[1] - kDebugSide * 0.5f;   // sim y

    // GPU/render surface: high-mode height + choppiness displacement.
    m_scene.GetWaterSurface().SampleSurfaceGrid(ox, oz, cell, DG, m_elapsedTime, kDebugModes,
                                                m_dbgH.data(), m_dbgDx.data(), m_dbgDz.data());

    constexpr float half = 0.20f;   // tile half-size (m)
    // Render frame is Y-up: sim (x, y, elevation) → (x, elevation, y).
    auto writeTile = [&](int base, float wx, float h, float wz)
    {
        auto set = [&](int k, float dx, float dz)
        {
            PbrVertex& v = dbg.vertices[base + k];
            v.position[0] = wx + dx; v.position[1] = h; v.position[2] = wz + dz;
            v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
            v.uv[0] = v.uv[1] = 0.0f;
        };
        set(0, -half, -half); set(1, half, -half); set(2, half, half); set(3, -half, half);
    };

    for (int j = 0; j < DG; ++j)
        for (int i = 0; i < DG; ++i)
        {
            int idx = j * DG + i;
            float gx = ox + i * cell, gz = oz + j * cell;
            // GREEN — physics surface: EXACTLY what buoyancy samples (Eulerian, modes=8, un-displaced).
            float hp = App_SampleWaveHeight(gx, gz, 0.0f);
            writeTile(idx * 4, gx, hp, gz);
            // MAGENTA — GPU/render surface: same grid, but displaced like the water vertex shader.
            writeTile((tiles + idx) * 4, gx + m_dbgDx[idx], m_dbgH[idx], gz + m_dbgDz[idx]);
        }
}

// Keyboard sailing controls (active in follow mode). Rates approximate the Godot eased-slack feel;
// the load-dependent gearing is a later refinement. Held keys ramp the control at a fixed rate.
void App::UpdateSailControls(float dt)
{
    auto& in = m_inputManager;
    const bool shift = in.IsKeyDown(VK_SHIFT);

    // Mainsheet: S trims in (smaller), Shift+S eases (larger).
    if (in.IsKeyDown('S'))
        m_sheet = shift ? std::min(4.0f, m_sheet + 1.0f * dt)
                        : std::max(0.09f, m_sheet - 1.0f * dt);
    // Vang: V trims (smaller), Shift+V eases.
    if (in.IsKeyDown('V'))
        m_vang = shift ? std::min(0.866f, m_vang + 0.3f * dt)
                       : std::max(0.20f, m_vang - 0.3f * dt);
    // Outhaul: O hauls flatter (smaller), Shift+O eases.
    if (in.IsKeyDown('O'))
        m_outhaul = shift ? std::min(0.25f, m_outhaul + 0.05f * dt)
                          : std::max(0.0f, m_outhaul - 0.05f * dt);

    // Helm: Left = port (+), Right = starboard (−), Space recenters.
    if (in.IsKeyDown(VK_LEFT))  m_helm = std::min(0.5f, m_helm + 1.0f * dt);
    if (in.IsKeyDown(VK_RIGHT)) m_helm = std::max(-0.5f, m_helm - 1.0f * dt);
    if (in.IsKeyDown(VK_SPACE)) m_helm = 0.0f;

    // Crew hike: , = port (+), . = starboard (−); persists when released.
    if (in.IsKeyDown(VK_OEM_COMMA))  m_hike = std::min(1.0f, m_hike + 1.5f * dt);
    if (in.IsKeyDown(VK_OEM_PERIOD)) m_hike = std::max(-1.0f, m_hike - 1.5f * dt);
    // Crew fore/aft: Up = forward (+), Down = aft (−).
    if (in.IsKeyDown(VK_UP))   m_foreAft = std::min(1.0f, m_foreAft + 1.0f * dt);
    if (in.IsKeyDown(VK_DOWN)) m_foreAft = std::max(-1.0f, m_foreAft - 1.0f * dt);

    // X rights the boat (capsize recovery): pose reset on the sim side, keeping heading + position.
    if (in.IsKeyPressed('X')) m_sim.ResetUpright();

    // B blows (dumps sheet + vang to fully eased); R resets everything.
    if (in.IsKeyPressed('B')) { m_sheet = 4.0f; m_vang = 0.866f; }
    if (in.IsKeyPressed('R'))
    {
        m_sheet = 4.0f; m_vang = 0.866f; m_outhaul = 0.09253f;
        m_helm = 0.0f; m_centreboard = 0.0f; m_hike = 0.0f; m_foreAft = 0.0f;
        m_camOrbitYaw = 0.0f; m_camOrbitPitch = 0.0f; m_camZoom = 1.0f; m_cameraInitialised = false;
    }
}

// Right-mouse orbit, wheel zoom, C recenter — layered on top of the chase follow.
void App::UpdateCameraInput()
{
    int wheel = m_inputManager.GetMouseWheelDelta();
    if (wheel != 0)
    {
        float notches = static_cast<float>(wheel) / 120.0f;   // WHEEL_DELTA
        m_camZoom *= std::pow(1.1f, -notches);                // wheel up → closer
        m_camZoom = std::clamp(m_camZoom, 0.3f, 3.0f);
    }
    if (m_inputManager.IsMouseButtonDown(1))                  // right button held = orbit
    {
        m_camOrbitYaw   -= static_cast<float>(m_inputManager.GetMouseDeltaX()) * 0.005f;
        m_camOrbitPitch  = std::clamp(
            m_camOrbitPitch + static_cast<float>(m_inputManager.GetMouseDeltaY()) * 0.005f,
            -1.4f, 1.4f);
    }
    if (m_inputManager.IsKeyPressed('C')) { m_camOrbitYaw = 0.0f; m_camOrbitPitch = 0.0f; }
}

// Spring-arm chase camera (port of Godot's UpdateChaseCamera): follows the boat's heading only,
// with orbit yaw/pitch and zoom layered on, then exponential position smoothing. All math is in the
// sim's Z-up frame, then eye/target are mapped to the renderer's Y-up frame (swap y↔z).
void App::UpdateChaseCamera(float dt)
{
    XMVECTOR pos = XMVectorSet(m_boatPos[0], m_boatPos[1], m_boatPos[2], 0.0f);
    XMVECTOR q   = XMVectorSet(m_boatQuat[0], m_boatQuat[1], m_boatQuat[2], m_boatQuat[3]);

    XMVECTOR bodyX = XMVector3Rotate(XMVectorSet(1, 0, 0, 0), q);          // boat forward (sim world)
    float yaw = atan2f(XMVectorGetY(bodyX), XMVectorGetX(bodyX));           // heading, ignores heel/pitch

    XMVECTOR yawQuat   = XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), yaw + m_camOrbitYaw);
    XMVECTOR rightAxis = XMVector3Rotate(XMVectorSet(0, 1, 0, 0), yawQuat);
    XMVECTOR pitchQuat = XMQuaternionRotationAxis(rightAxis, m_camOrbitPitch);

    XMVECTOR baseOffset = XMVectorSet(-12.0f * m_camZoom, 0.0f, 4.0f * m_camZoom, 0.0f);  // 12 astern, 4 up
    XMVECTOR off = XMVector3Rotate(XMVector3Rotate(baseOffset, yawQuat), pitchQuat);
    XMVECTOR desired = XMVectorAdd(pos, off);
    XMVECTOR lookAt  = XMVectorAdd(pos, XMVectorSet(0, 0, 3, 0));           // 3 m above boat

    XMFLOAT3 eyeR = { XMVectorGetX(desired), XMVectorGetZ(desired), XMVectorGetY(desired) };
    XMFLOAT3 tgtR = { XMVectorGetX(lookAt),  XMVectorGetZ(lookAt),  XMVectorGetY(lookAt) };

    if (!m_cameraInitialised) { m_smoothedCamPos = eyeR; m_cameraInitialised = true; }
    else
    {
        float a = 1.0f - std::exp(-1.5f * dt);   // framerate-independent smoothing (~0.67 s)
        m_smoothedCamPos.x += (eyeR.x - m_smoothedCamPos.x) * a;
        m_smoothedCamPos.y += (eyeR.y - m_smoothedCamPos.y) * a;
        m_smoothedCamPos.z += (eyeR.z - m_smoothedCamPos.z) * a;
    }
    m_camera.SetLookAt(m_smoothedCamPos, tgtR);
}

void App::InitDevice(HWND hwnd)
{
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debugController.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(TRUE);
            }
        }
    }
#endif

    ASSERT_SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ASSERT_SUCCEEDED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
}

void App::InitSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory4> factory;
    ASSERT_SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = WindowWidth;
    swapChainDesc.Height = WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    ASSERT_SUCCEEDED(factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hwnd, &swapChainDesc,
                                                     nullptr, nullptr, &swapChain1));

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ASSERT_SUCCEEDED(swapChain1.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void App::InitBuffers()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ASSERT_SUCCEEDED(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; i++)
    {
        m_displayBuffers[i].InitFromSwapChain(m_device.Get(), m_swapChain.Get(), i, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    for (UINT i = 0; i < FrameCount; i++)
    {
        ASSERT_SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&m_commandAllocators[i])));
    }

    m_depthBuffer.Create(m_device.Get(), WindowWidth, WindowHeight);
}

void App::InitFence()
{
    ASSERT_SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void App::InitViewport()
{
    m_viewport = {0.0f, 0.0f, static_cast<float>(WindowWidth), static_cast<float>(WindowHeight),
                  0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(WindowWidth), static_cast<LONG>(WindowHeight)};
}

void App::Resize(UINT width, UINT height)
{
    // Ignore pre-init messages, minimize (0×0), and no-op resizes.
    if (!m_swapChain || width == 0 || height == 0)
        return;
    if (width == m_width && height == m_height)
        return;

    // All in-flight GPU work must finish before back buffers / depth are released.
    WaitForGpu();

    // Drop references to the old swap-chain back buffers (required by ResizeBuffers).
    for (UINT i = 0; i < FrameCount; ++i)
        m_displayBuffers[i].Reset();

    ASSERT_SUCCEEDED(m_swapChain->ResizeBuffers(
        FrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));

    // Recreate RTVs into the existing heap and the depth buffer at the new size.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; ++i)
    {
        m_displayBuffers[i].InitFromSwapChain(m_device.Get(), m_swapChain.Get(), i, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    m_depthBuffer.Create(m_device.Get(), width, height);

    m_width  = width;
    m_height = height;
    m_viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

    m_camera.SetPerspective(XM_PIDIV4,
        static_cast<float>(width) / static_cast<float>(height), 0.1f, 1000.0f);

    // Size-dependent render targets (scene-color copy) are rebuilt on the next BeginFrame.
    m_renderer->OnResize();
}

void App::LoadScene()
{
    m_scene.Load(ResolveExePath("scenes/testscene.json"), m_device.Get(),
                 m_renderer->GetResourceManager());

    WaterDesc waterDesc = {};
    waterDesc.N = waterDesc.M = 256;
    waterDesc.TileSize = 512.0f;  // mesh tile span = largest cascade; cascades sized internally
    m_scene.CreateWater(m_device.Get(), waterDesc, m_commandQueue.Get());

    m_scene.CreateAtmosphere(m_device.Get());

    // Prime the directional light direction from the initial GUI sun angles so that
    // frame 0 uses the same direction the sliders show rather than whatever came from JSON.
    if (!m_scene.GetDirectionalLights().empty())
    {
        float elevRad = m_sunElevation * 3.14159265f / 180.0f;
        float azimRad = m_sunAzimuth   * 3.14159265f / 180.0f;
        auto& dir = m_scene.GetDirectionalLights()[0].direction;
        dir.x = -cosf(elevRad) * sinf(azimRad);
        dir.y = -sinf(elevRad);
        dir.z = -cosf(elevRad) * cosf(azimRad);
    }

    // Wire the atmosphere env cubemap into the water reflection slot.
    // This replaces the static JPG cubemap; the atmosphere updates it every frame.
    m_scene.GetWaterSurface().SetEnvCubemapFromResource(
        m_device.Get(), m_scene.GetAtmosphere().GetEnvCubeResource());

    // The buoyancy wave query samples this surface (via the per-frame local cache).
    g_waveSurface = &m_scene.GetWaterSurface();

    // The sim drives the first entity's transform (the boat). See scenes/testscene.json.
    if (!m_scene.GetEntities().empty())
        m_boatEntityIndex = 0;
}

void App::InitImGui(HWND hwnd)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_commandQueue.Get();
    initInfo.NumFramesInFlight = FrameCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.SrvDescriptorHeap = m_renderer->GetTransientHeap().GetHeap();
    initInfo.LegacySingleSrvCpuDescriptor = m_renderer->GetTransientHeap().GetCPUHandle(0);
    initInfo.LegacySingleSrvGpuDescriptor = m_renderer->GetTransientHeap().GetGPUHandle(0);
    ImGui_ImplDX12_Init(&initInfo);
}

void App::BeginInputFrame()
{
    m_inputManager.BeginFrame();
}

void App::OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
        // wParam == SIZE_MINIMIZED yields 0×0 via LOWORD/HIWORD, which Resize() ignores.
        Resize(LOWORD(lParam), HIWORD(lParam));
        break;
    case WM_KEYDOWN:
        m_inputManager.OnKeyDown(static_cast<int>(wParam));
        break;
    case WM_KEYUP:
        m_inputManager.OnKeyUp(static_cast<int>(wParam));
        break;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        m_inputManager.OnMouseButtonDown(0);
        break;
    case WM_LBUTTONUP:
        ReleaseCapture();
        m_inputManager.OnMouseButtonUp(0);
        break;
    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        m_inputManager.OnMouseButtonDown(1);
        break;
    case WM_RBUTTONUP:
        ReleaseCapture();
        m_inputManager.OnMouseButtonUp(1);
        break;
    case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        m_inputManager.OnMouseButtonDown(2);
        break;
    case WM_MBUTTONUP:
        ReleaseCapture();
        m_inputManager.OnMouseButtonUp(2);
        break;
    case WM_MOUSEMOVE:
        m_inputManager.OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        break;
    case WM_MOUSEWHEEL:
        m_inputManager.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        break;
    }
}

void App::Update(float dt)
{
    // ImGui gets first priority on input consumption
    if (ImGui::GetCurrentContext())
    {
        if (ImGui::GetIO().WantCaptureKeyboard)
        {
            m_inputManager.ConsumeAllKeys();
        }
        if (ImGui::GetIO().WantCaptureMouse)
        {
            m_inputManager.ConsumeAllMouse();
        }
    }

    // Pause toggle (P): freezes the sim thread + the ocean-wave clock; render/camera stay live.
    if (m_inputManager.IsKeyPressed('P'))
    {
        m_paused = !m_paused;
        m_sim.SetPaused(m_paused);
    }

    // Advance the ocean clock only when running — freezing it stops the FFT waves (render + the
    // buoyancy wave cache both read this time), so the whole sea holds still with the sim.
    if (!m_paused)
        m_elapsedTime += dt;

    // Drive the sim: player controls (keyboard when following) + wind, then read telemetry.
    if (m_sim.SimReady())
    {
        if (m_followBoat)
            UpdateSailControls(dt);
        m_sim.SetControls(m_sheet, m_vang, m_outhaul, m_helm, m_centreboard, m_hike, m_foreAft);
        m_sim.SetWind(m_windSpeed, XMConvertToRadians(m_windDirDeg));
        m_sim.GetTelemetry(m_telemetry);
    }

    // Pull the latest boat pose and refresh the buoyancy wave cache around it.
    UpdateBoatFromSim();
    UpdateRigMesh(dt);
    UpdateDebugWaveMesh();

    // Chase/orbit camera when following the boat; free WASD/mouse fly otherwise.
    if (m_followBoat)
    {
        UpdateCameraInput();
        UpdateChaseCamera(dt);
    }
    else
    {
        m_camera.Update(dt, m_inputManager);
    }

    m_fpsFrameCount++;
    m_fpsAccumulator += dt;
    if (m_fpsAccumulator >= 1.0f)
    {
        m_fps = static_cast<float>(m_fpsFrameCount) / m_fpsAccumulator;
        m_fpsFrameCount = 0;
        m_fpsAccumulator = 0.0f;
    }
}

void App::WaitForGpu()
{
    UINT64 fenceValue = m_nextFenceValue++;
    ASSERT_SUCCEEDED(m_commandQueue->Signal(m_fence.Get(), fenceValue));
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        ASSERT_SUCCEEDED(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    for (UINT i = 0; i < FrameCount; i++)
    {
        m_frameFenceValues[i] = fenceValue;
    }
}

void App::WaitForFrame(UINT frameIndex)
{
    if (m_fence->GetCompletedValue() < m_frameFenceValues[frameIndex])
    {
        ASSERT_SUCCEEDED(
            m_fence->SetEventOnCompletion(m_frameFenceValues[frameIndex], m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void App::Render()
{
    WaitForFrame(m_frameIndex);

    // Mesh rebuild releases the old vertex/index buffers — must wait for all frame slots,
    // not just this one, since FrameCount=3 means two other slots may still be in flight.
    if (m_scene.GetWaterSurface().tweaks.meshResolution != m_scene.GetWaterSurface().GetDesc().MeshResolution)
        WaitForGpu();
    m_scene.GetWaterSurface().RebuildMeshIfNeeded();

    auto& rt = m_displayBuffers[m_frameIndex];

    m_renderer->BeginFrame(m_commandAllocators[m_frameIndex].Get(), rt, m_depthBuffer, m_viewport,
                           m_scissorRect, m_frameIndex, m_fence->GetCompletedValue());

    // Build view
    XMMATRIX viewMat = m_camera.GetViewMatrix();
    XMMATRIX projMat = m_camera.GetProjectionMatrix();

    View view = {};
    XMStoreFloat4x4(&view.viewMatrix, XMMatrixTranspose(viewMat));
    XMStoreFloat4x4(&view.projMatrix, XMMatrixTranspose(projMat));
    XMStoreFloat4x4(&view.viewProjMatrix, XMMatrixTranspose(viewMat * projMat));
    view.position = m_camera.GetPosition();
    view.nearZ    = 0.1f;
    view.farZ     = 1000.0f;

    // Build per-frame constants
    FrameConstants frameConstants = {};
    frameConstants.viewProj = view.viewProjMatrix;
    frameConstants.cameraPos = view.position;

    m_renderer->RenderScene(m_scene, view, frameConstants, m_elapsedTime);

    // ImGui
    m_renderer->GetProfiler().BeginScope("ImGui", m_renderer->GetCommandList());

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Scene Inspector");
    ImGui::Text("%.1f FPS (%.2f ms)", m_fps, m_fps > 0.0f ? 1000.0f / m_fps : 0.0f);
    XMFLOAT3 pos = m_camera.GetPosition();
    ImGui::Text("Camera: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
    ImGui::SliderFloat("Move Speed", &m_camera.GetMoveSpeed(), 1.0f, 50.0f);
    ImGui::Checkbox("VSync", &m_vsync);
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Boat / Sim", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Follow camera", &m_followBoat);
        ImGui::SameLine();
        ImGui::TextDisabled("(off = free fly)");
        ImGui::SameLine();
        ImGui::TextDisabled("| Sim: %s", m_sim.SimReady() ? "500 Hz" : "off");

        ImGui::SeparatorText("Controls");
        ImGui::SliderFloat("Mainsheet (m)", &m_sheet,       0.09f, 4.0f,  "%.2f");
        ImGui::SliderFloat("Vang (m)",      &m_vang,        0.20f, 0.866f, "%.3f");
        ImGui::SliderFloat("Outhaul (m)",   &m_outhaul,     0.0f,  0.25f,  "%.3f");
        ImGui::SliderAngle("Helm",          &m_helm,       -28.6f, 28.6f);   // ±0.5 rad
        ImGui::SliderFloat("Centreboard",   &m_centreboard, 0.0f,  1.0f,   "%.2f");
        ImGui::SliderFloat("Crew hike",     &m_hike,       -1.0f,  1.0f,   "%.2f");
        ImGui::SliderFloat("Crew fore/aft", &m_foreAft,    -1.0f,  1.0f,   "%.2f");
        if (ImGui::Button("Blow (B)")) { m_sheet = 4.0f; m_vang = 0.866f; }
        ImGui::SameLine();
        if (ImGui::Button("Reset (R)"))
        {
            m_sheet = 4.0f; m_vang = 0.866f; m_outhaul = 0.09253f;
            m_helm = 0.0f; m_centreboard = 0.0f; m_hike = 0.0f; m_foreAft = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Right boat (X)")) m_sim.ResetUpright();
        if (ImGui::Button(m_paused ? "Resume (P)" : "Pause (P)"))
        {
            m_paused = !m_paused;
            m_sim.SetPaused(m_paused);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(m_paused ? "sim + ocean paused" : "running");
        ImGui::Checkbox("Wave debug: physics=green, GPU=magenta", &m_showWaveDebug);
        ImGui::SameLine();
        ImGui::TextDisabled("(boat should sit on GREEN)");

        ImGui::SeparatorText("Wind");
        ImGui::SliderFloat("Speed (m/s)", &m_windSpeed,  0.0f, 15.0f,  "%.1f");
        ImGui::SliderFloat("Dir (deg)",   &m_windDirDeg, 0.0f, 360.0f, "%.0f");

        ImGui::SeparatorText("Telemetry");
        constexpr float R2D = 57.29578f;
        float hdg = m_telemetry[1] * R2D; if (hdg < 0.0f) hdg += 360.0f;
        ImGui::Text("SOG  %.2f m/s (%.1f kn)", m_telemetry[0], m_telemetry[0] * 1.94384f);
        ImGui::Text("HDG  %.0f deg    Boom %.0f deg", hdg, m_telemetry[10] * R2D);
        ImGui::Text("Heel %.1f deg    Pitch %.1f deg", m_telemetry[2] * R2D, m_telemetry[3] * R2D);
        ImGui::Text("TWS  %.1f m/s   TWA %.0f deg", m_telemetry[4], m_telemetry[5] * R2D);
        ImGui::Text("AWS  %.1f m/s   AWA %.0f deg", m_telemetry[6], m_telemetry[7] * R2D);
        ImGui::Text("Sheet %.0f N    Vang %.0f N", m_telemetry[8], m_telemetry[9]);

        ImGui::TextDisabled("S/V/O trim (Shift=ease) | arrows=helm | Space=center");
        ImGui::TextDisabled(",/.=hike | Up/Dn=fore-aft | B=blow R=reset X=right boat | P=pause");
        ImGui::TextDisabled("RMB=orbit | wheel=zoom | C=recenter cam");
    }
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Appearance"))
    {
        if (m_hullMat) ImGui::ColorEdit3("Hull",           &m_hullMat->baseColorFactor.x);
        if (m_sailMat) ImGui::ColorEdit4("Sail (a=opacity)", &m_sailMat->baseColorFactor.x);
        if (m_foilMat) ImGui::ColorEdit3("Foils",          &m_foilMat->baseColorFactor.x);
        if (m_markerMat) ImGui::ColorEdit3("Markers",      &m_markerMat->baseColorFactor.x);
        if (m_telltaleMat) ImGui::ColorEdit3("Telltales",  &m_telltaleMat->baseColorFactor.x);
    }
    ImGui::Separator();

    int entityIdx = 0;
    for (auto& entity : m_scene.GetEntities())
    {
        ImGui::PushID(entityIdx);
        char entityLabel[128];
        snprintf(entityLabel, sizeof(entityLabel), "Entity %d", entityIdx++);
        if (ImGui::CollapsingHeader(entityLabel, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Visible", &entity.visible);
            const auto& subMeshes = entity.mesh->GetSubMeshes();
            auto& editMaterials = entity.mesh->GetMaterials();

            if (ImGui::TreeNode("Materials"))
            {
                for (size_t i = 0; i < editMaterials.size(); i++)
                {
                    auto& mat = editMaterials[i];
                    char label[128];
                    if (!mat->name.empty())
                    {
                        snprintf(label, sizeof(label), "[%d] %s", (int)i, mat->name.c_str());
                    }
                    else
                    {
                        snprintf(label, sizeof(label), "Material %d", (int)i);
                    }

                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::TreeNode(label))
                    {
                        ImGui::SliderFloat("Roughness", &mat->roughness, 0.0f, 1.0f);
                        ImGui::SliderFloat("Metallic", &mat->metallic, 0.0f, 1.0f);
                        ImGui::ColorEdit4("Base Color", &mat->baseColorFactor.x);
                        ImGui::ColorEdit3("Emissive", &mat->emissiveFactor.x);
                        ImGui::Text("Base Texture: %s",
                                    mat->HasBaseColorTexture() ? "Yes" : "None");
                        ImGui::Text("Emissive Texture: %s",
                                    mat->HasEmissiveTexture() ? "Yes" : "None");
                        ImGui::Checkbox("Double Sided", &mat->doubleSided);
                        ImGui::Checkbox("Alpha Blend", &mat->alphaBlend);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("SubMeshes"))
            {
                for (size_t i = 0; i < subMeshes.size(); i++)
                {
                    const auto& sub = subMeshes[i];
                    char label[128];
                    if (!sub.name.empty())
                    {
                        snprintf(label, sizeof(label), "[%d] %s", (int)i, sub.name.c_str());
                    }
                    else
                    {
                        snprintf(label, sizeof(label), "SubMesh %d", (int)i);
                    }
                    if (ImGui::TreeNode(label))
                    {
                        ImGui::Text("Material: %d", sub.materialIndex);
                        ImGui::Text("Indices: %u (start: %u)", sub.indexCount, sub.startIndex);
                        ImGui::Text("Base Vertex: %d", sub.baseVertex);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& water  = m_scene.GetWaterSurface();
        auto& tweaks = water.tweaks;
        const auto& desc = water.GetDesc();

        ImGui::Checkbox("Visible##water", &tweaks.visible);
        ImGui::SameLine();
        ImGui::Checkbox("Tessellation", &tweaks.tessellation);
        ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &tweaks.wireframe);
        ImGui::SliderInt("Tile Count",      &tweaks.tileCount,      1,  9);
        ImGui::SliderInt("Mesh Resolution", &tweaks.meshResolution, 4, 64);
        ImGui::SliderFloat("Wind Speed",        &tweaks.windSpeed,       0.1f, 100.0f);
        ImGui::SliderAngle("Wind Direction",    &tweaks.windTheta,       -180.0f, 180.0f);
        ImGui::SliderFloat("Amplitude",         &tweaks.amplitude,       0.0f, 0.25f, "%.3f");
        ImGui::SliderFloat("Small Wave Cutoff", &tweaks.smallWaveCutoff, 0.001f, 0.1f, "%.4f");
        ImGui::SliderFloat("Choppiness",        &tweaks.choppiness,      0.0f,   1.0f);
        ImGui::SliderFloat("Time Scale",        &tweaks.timeScale,       0.0f,   2.0f);
        ImGui::SliderFloat("Max Tess",          &tweaks.maxTessellation, 1.0f,   64.0f);
        ImGui::SliderFloat("Tess Distance",     &tweaks.tessDistance,    0.0f, 1000.0f);
        ImGui::ColorEdit3 ("Color",             &tweaks.color.x);

        const auto& specPlot = water.GetSpectrumPlot();
        if (!specPlot.empty())
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Wave Spectrum  (|k| = 0..N/2,  total |h0| per ring)");
            ImGui::PlotLines("##spectrum", specPlot.data(), (int)specPlot.size(),
                             0, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 80));
        }

        ImGui::TextDisabled("FFT: %dx%d  Tile: %.1fm", desc.N, desc.M, desc.TileSize);
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto& atm = m_scene.GetAtmosphere();
        auto& ap  = atm.params;
        ImGui::Checkbox("Visible##atm", &atm.visible);

        if (ImGui::TreeNode("Sun"))
        {
            // Drive the first directional light's direction from elevation/azimuth.
            bool changed = false;
            changed |= ImGui::SliderFloat("Elevation (deg)", &m_sunElevation, -10.0f, 90.0f);
            changed |= ImGui::SliderFloat("Azimuth (deg)",   &m_sunAzimuth,    0.0f, 360.0f);
            ImGui::SliderFloat("Intensity", &ap.sunIntensity, 1.0f, 50.0f);

            if (changed && !m_scene.GetDirectionalLights().empty())
            {
                float elevRad = m_sunElevation * 3.14159265f / 180.0f;
                float azimRad = m_sunAzimuth   * 3.14159265f / 180.0f;
                // Direction light travels (toward scene): opposite of "toward sun"
                auto& dir = m_scene.GetDirectionalLights()[0].direction;
                dir.x = -cosf(elevRad) * sinf(azimRad);
                dir.y = -sinf(elevRad);
                dir.z = -cosf(elevRad) * cosf(azimRad);
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Rayleigh"))
        {
            ImGui::SliderFloat3("Beta (km^-1)", &ap.rayleighBeta.x, 0.0f, 0.1f, "%.5f");
            ImGui::SliderFloat("Scale Height (km)", &ap.rayleighScaleHeight, 1.0f, 20.0f);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Mie"))
        {
            ImGui::SliderFloat("Beta (km^-1)",    &ap.mieBeta.x,      0.0f, 0.1f, "%.4f");
            ap.mieBeta.y = ap.mieBeta.z = ap.mieBeta.x;
            ap.mieBetaExt.x = ap.mieBetaExt.y = ap.mieBetaExt.z = ap.mieBeta.x * 1.1f;
            ImGui::SliderFloat("Scale Height (km)", &ap.mieScaleHeight, 0.1f, 5.0f);
            ImGui::SliderFloat("G (asymmetry)",  &ap.mieG,           0.0f, 0.999f);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Ozone"))
        {
            ImGui::SliderFloat3("Beta Abs (km^-1)",   &ap.ozoneBetaAbs.x,      0.0f, 0.005f, "%.5f");
            ImGui::SliderFloat("Center Height (km)", &ap.ozoneCenterHeight, 10.0f, 40.0f);
            ImGui::SliderFloat("Width (km)",          &ap.ozoneWidth,         5.0f, 30.0f);
            ImGui::TreePop();
        }
        if (ImGui::Button("Recompute Transmittance LUT"))
            atm.MarkDirty();
    }

    ImGui::End();

    // GPU Timings window
    ImGui::Begin("GPU Timings");
    const auto& gpuResults = m_renderer->GetProfiler().GetResults();
    if (gpuResults.empty())
    {
        ImGui::TextDisabled("Waiting for GPU data...");
    }
    else
    {
        float totalMs = 1.0f;
        for (const auto& r : gpuResults)
            if (strcmp(r.name, "Total Frame") == 0) { totalMs = r.ms; break; }

        if (ImGui::BeginTable("gpu_timings", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("ms",    ImGuiTableColumnFlags_WidthFixed,  52.0f);
            ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& r : gpuResults)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", r.ms);
                ImGui::TableSetColumnIndex(2);
                ImGui::ProgressBar(r.ms / totalMs, ImVec2(-1.0f, 0.0f), "");
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_renderer->GetCommandList());
    m_renderer->GetProfiler().EndScope(m_renderer->GetCommandList()); // ImGui

    m_frameFenceValues[m_frameIndex] = m_renderer->EndFrame(rt, m_fence.Get(), m_nextFenceValue);

    ASSERT_SUCCEEDED(m_swapChain->Present(m_vsync ? 1 : 0, 0));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void App::Cleanup()
{
    // Stop the sim thread first so it no longer invokes the wave callback while we tear down.
    m_sim.Stop();
    WaitForGpu();
    m_renderer->Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
    }
}
