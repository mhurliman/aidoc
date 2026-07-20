#pragma once

#include <string>
#include <cstdint>
#include <vector>

// Wave-height callback the managed sim invokes for buoyancy: returns the water surface height (m)
// at a world XZ position and time. cdecl to match the managed [UnmanagedCallersOnly] cdecl exports.
using WaveHeightFn = float(__cdecl*)(float worldX, float worldZ, float t);

// Loads the C# physics sim (Sim.Host.dll) via the .NET runtime host (nethost + hostfxr) and exposes
// its C-ABI controls. The managed sim runs on its own fixed-timestep (500 Hz) thread; the render
// thread reads the latest pose via GetPose (a lock-free snapshot on the managed side).
//
// See plan §2. Pose is reported in the sim's Z-up world frame; the caller maps it to the renderer's
// Y-up frame (see App.cpp).
class SimBridge
{
public:
    ~SimBridge();

    // Load Sim.Host.dll + start the runtime from <exeDir> (which must contain Sim.Host.dll and
    // Sim.Host.runtimeconfig.json). Returns false and logs on failure.
    bool Init(const std::wstring& exeDir);

    // Create the managed sim, wiring in the native ocean height query. Must be called after Init.
    bool Create(WaveHeightFn waveFn);

    void Start();  // launch the 500 Hz sim thread
    void Stop();   // stop + join

    // Latest pose in the sim's Z-up world frame: pos[3]=xyz, quat[4]=xyzw, *time=sim seconds.
    void GetPose(float pos[3], float quat[4], float* time) const;

    // Fetch the boat hull render mesh (renderer Y-up body frame). Returns false if unavailable.
    // positions/normals: 3 floats per vertex; indices: 3 per triangle.
    bool GetHullMesh(std::vector<float>& positions, std::vector<float>& normals,
                     std::vector<uint32_t>& indices) const;

    // Player controls: sheet/vang/outhaul (m), helm (rad), centreboard/hike/foreAft (dimensionless).
    void SetControls(float sheet, float vang, float outhaul, float helm,
                     float centreboard, float hike, float foreAft) const;

    // Uniform wind: speed (m/s), direction (rad, compass bearing wind blows toward).
    void SetWind(float speed, float directionRad) const;

    // Reset the boat upright (capsize recovery): keeps position + heading, zeroes heel/velocity.
    void ResetUpright() const;

    // Pause/resume physics stepping (the sim thread keeps running but stops advancing).
    void SetPaused(bool paused) const;

    // Fill a 12-float telemetry snapshot (see SimHost.GetTelemetry for the layout).
    void GetTelemetry(float outTelemetry[12]) const;

    // Rig mesh (sail + spars + lines). Topology is fixed; call GetRigInfo/GetRigIndices once, then
    // GetRigVertices each frame. Vertices are render Y-up frame; 3 floats per pos/normal.
    void GetRigInfo(int& vertexCount, int& sailIdx, int& sparIdx, int& lineIdx, int& foilIdx, int& markerIdx, int& telltaleIdx) const;
    void GetRigIndices(std::vector<uint32_t>& indices) const;   // sized by caller to total index count
    void GetRigVertices(float dt, std::vector<float>& positions, std::vector<float>& normals) const;

    bool RuntimeReady() const { return m_create != nullptr; }
    bool SimReady() const { return m_handle != 0; }

private:
    using SimCreateFn  = std::intptr_t(__cdecl*)(WaveHeightFn);
    using SimStartFn   = void(__cdecl*)(std::intptr_t);
    using SimStopFn    = void(__cdecl*)(std::intptr_t);
    using SimGetPoseFn = void(__cdecl*)(std::intptr_t, float*, float*, float*);
    using SimDestroyFn = void(__cdecl*)(std::intptr_t);
    using SimHullCountsFn = void(__cdecl*)(std::intptr_t, int*, int*);
    using SimHullMeshFn = void(__cdecl*)(std::intptr_t, float*, float*, int*);
    using SimSetControlsFn = void(__cdecl*)(std::intptr_t, float, float, float, float, float, float, float);
    using SimSetWindFn = void(__cdecl*)(std::intptr_t, float, float);
    using SimResetUprightFn = void(__cdecl*)(std::intptr_t);
    using SimSetPausedFn = void(__cdecl*)(std::intptr_t, int);
    using SimGetTelemetryFn = void(__cdecl*)(std::intptr_t, float*);
    using SimRigInfoFn = void(__cdecl*)(std::intptr_t, int*, int*, int*, int*, int*, int*, int*);
    using SimRigIndicesFn = void(__cdecl*)(std::intptr_t, int*);
    using SimRigVerticesFn = void(__cdecl*)(std::intptr_t, float, float*, float*);

    SimCreateFn       m_create      = nullptr;
    SimStartFn        m_start       = nullptr;
    SimStopFn         m_stop        = nullptr;
    SimGetPoseFn      m_getPose     = nullptr;
    SimDestroyFn      m_destroy     = nullptr;
    SimHullCountsFn   m_hullCounts  = nullptr;
    SimHullMeshFn     m_hullMesh    = nullptr;
    SimSetControlsFn  m_setControls = nullptr;
    SimSetWindFn      m_setWind     = nullptr;
    SimResetUprightFn m_resetUpright = nullptr;
    SimSetPausedFn    m_setPaused    = nullptr;
    SimGetTelemetryFn m_getTelemetry = nullptr;
    SimRigInfoFn      m_rigInfo     = nullptr;
    SimRigIndicesFn   m_rigIndices  = nullptr;
    SimRigVerticesFn  m_rigVertices = nullptr;
    std::intptr_t m_handle = 0;
};
