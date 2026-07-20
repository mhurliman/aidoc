#include "SimBridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>

#include <cstdio>

namespace
{
    hostfxr_initialize_for_runtime_config_fn s_init     = nullptr;
    hostfxr_get_runtime_delegate_fn          s_getDelegate = nullptr;
    hostfxr_close_fn                         s_close    = nullptr;

    // Locate hostfxr.dll (via nethost) and bind the three entry points we need.
    bool LoadHostfxr()
    {
        if (s_init && s_getDelegate && s_close) return true;

        char_t path[MAX_PATH];
        size_t size = sizeof(path) / sizeof(char_t);
        if (get_hostfxr_path(path, &size, nullptr) != 0)
        {
            fprintf(stderr, "[sim] get_hostfxr_path failed — is the .NET runtime installed?\n");
            return false;
        }

        HMODULE lib = ::LoadLibraryW(path);
        if (!lib)
        {
            fprintf(stderr, "[sim] LoadLibrary(hostfxr) failed\n");
            return false;
        }

        s_init = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
            ::GetProcAddress(lib, "hostfxr_initialize_for_runtime_config"));
        s_getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
            ::GetProcAddress(lib, "hostfxr_get_runtime_delegate"));
        s_close = reinterpret_cast<hostfxr_close_fn>(
            ::GetProcAddress(lib, "hostfxr_close"));

        return s_init && s_getDelegate && s_close;
    }

    // Initialize the runtime from a .runtimeconfig.json and fetch the
    // load_assembly_and_get_function_pointer delegate.
    load_assembly_and_get_function_pointer_fn GetLoadAssembly(const char_t* configPath)
    {
        hostfxr_handle ctx = nullptr;
        int rc = s_init(configPath, nullptr, &ctx);
        if (rc != 0 || ctx == nullptr)
        {
            fprintf(stderr, "[sim] hostfxr_initialize_for_runtime_config failed (0x%x)\n", rc);
            if (ctx) s_close(ctx);
            return nullptr;
        }

        void* fn = nullptr;
        rc = s_getDelegate(ctx, hdt_load_assembly_and_get_function_pointer, &fn);
        s_close(ctx);
        if (rc != 0 || fn == nullptr)
        {
            fprintf(stderr, "[sim] hostfxr_get_runtime_delegate failed (0x%x)\n", rc);
            return nullptr;
        }
        return reinterpret_cast<load_assembly_and_get_function_pointer_fn>(fn);
    }
}

SimBridge::~SimBridge()
{
    if (m_handle && m_destroy)
    {
        m_destroy(m_handle);
        m_handle = 0;
    }
}

bool SimBridge::Init(const std::wstring& exeDir)
{
    if (!LoadHostfxr()) return false;

    std::wstring configPath = exeDir + L"Sim.Host.runtimeconfig.json";
    std::wstring asmPath    = exeDir + L"Sim.Host.dll";

    auto loadAssembly = GetLoadAssembly(configPath.c_str());
    if (!loadAssembly) return false;

    const char_t* type = L"Sim.Host.NativeExports, Sim.Host";

    auto resolve = [&](const char_t* method, void** out) -> bool
    {
        int rc = loadAssembly(asmPath.c_str(), type, method,
                              UNMANAGEDCALLERSONLY_METHOD, nullptr, out);
        if (rc != 0 || *out == nullptr)
        {
            fprintf(stderr, "[sim] failed to resolve export (0x%x)\n", rc);
            return false;
        }
        return true;
    };

    bool ok = resolve(L"SimCreate",           reinterpret_cast<void**>(&m_create))
           && resolve(L"SimStart",            reinterpret_cast<void**>(&m_start))
           && resolve(L"SimStop",             reinterpret_cast<void**>(&m_stop))
           && resolve(L"SimGetBodyPose",      reinterpret_cast<void**>(&m_getPose))
           && resolve(L"SimGetHullMeshCounts",reinterpret_cast<void**>(&m_hullCounts))
           && resolve(L"SimGetHullMesh",      reinterpret_cast<void**>(&m_hullMesh))
           && resolve(L"SimSetControls",      reinterpret_cast<void**>(&m_setControls))
           && resolve(L"SimSetWind",          reinterpret_cast<void**>(&m_setWind))
           && resolve(L"SimResetUpright",     reinterpret_cast<void**>(&m_resetUpright))
           && resolve(L"SimSetPaused",        reinterpret_cast<void**>(&m_setPaused))
           && resolve(L"SimGetTelemetry",     reinterpret_cast<void**>(&m_getTelemetry))
           && resolve(L"SimGetRigInfo",       reinterpret_cast<void**>(&m_rigInfo))
           && resolve(L"SimGetRigIndices",    reinterpret_cast<void**>(&m_rigIndices))
           && resolve(L"SimGetRigVertices",   reinterpret_cast<void**>(&m_rigVertices))
           && resolve(L"SimDestroy",          reinterpret_cast<void**>(&m_destroy));

    if (!ok)
    {
        m_create = nullptr;
        return false;
    }

    fprintf(stderr, "[sim] Sim.Host loaded; runtime ready.\n");
    return true;
}

bool SimBridge::Create(WaveHeightFn waveFn)
{
    if (!m_create) return false;
    m_handle = m_create(waveFn);
    if (m_handle == 0)
    {
        fprintf(stderr, "[sim] SimCreate returned null handle.\n");
        return false;
    }
    return true;
}

void SimBridge::Start()
{
    if (m_handle && m_start) m_start(m_handle);
}

void SimBridge::Stop()
{
    if (m_handle && m_stop) m_stop(m_handle);
}

void SimBridge::GetPose(float pos[3], float quat[4], float* time) const
{
    if (m_handle && m_getPose)
        m_getPose(m_handle, pos, quat, time);
}

void SimBridge::SetControls(float sheet, float vang, float outhaul, float helm,
                            float centreboard, float hike, float foreAft) const
{
    if (m_handle && m_setControls)
        m_setControls(m_handle, sheet, vang, outhaul, helm, centreboard, hike, foreAft);
}

void SimBridge::SetWind(float speed, float directionRad) const
{
    if (m_handle && m_setWind)
        m_setWind(m_handle, speed, directionRad);
}

void SimBridge::ResetUpright() const
{
    if (m_handle && m_resetUpright)
        m_resetUpright(m_handle);
}

void SimBridge::SetPaused(bool paused) const
{
    if (m_handle && m_setPaused)
        m_setPaused(m_handle, paused ? 1 : 0);
}

void SimBridge::GetTelemetry(float outTelemetry[12]) const
{
    if (m_handle && m_getTelemetry)
        m_getTelemetry(m_handle, outTelemetry);
}

void SimBridge::GetRigInfo(int& vertexCount, int& sailIdx, int& sparIdx, int& lineIdx, int& foilIdx, int& markerIdx, int& telltaleIdx) const
{
    vertexCount = sailIdx = sparIdx = lineIdx = foilIdx = markerIdx = telltaleIdx = 0;
    if (m_handle && m_rigInfo)
        m_rigInfo(m_handle, &vertexCount, &sailIdx, &sparIdx, &lineIdx, &foilIdx, &markerIdx, &telltaleIdx);
}

void SimBridge::GetRigIndices(std::vector<uint32_t>& indices) const
{
    if (m_handle && m_rigIndices && !indices.empty())
        m_rigIndices(m_handle, reinterpret_cast<int*>(indices.data()));
}

void SimBridge::GetRigVertices(float dt, std::vector<float>& positions, std::vector<float>& normals) const
{
    if (m_handle && m_rigVertices && !positions.empty())
        m_rigVertices(m_handle, dt, positions.data(), normals.data());
}

bool SimBridge::GetHullMesh(std::vector<float>& positions, std::vector<float>& normals,
                            std::vector<uint32_t>& indices) const
{
    if (!m_handle || !m_hullCounts || !m_hullMesh)
        return false;

    int vertexCount = 0, indexCount = 0;
    m_hullCounts(m_handle, &vertexCount, &indexCount);
    if (vertexCount <= 0 || indexCount <= 0)
        return false;

    positions.resize(static_cast<size_t>(vertexCount) * 3);
    normals.resize(static_cast<size_t>(vertexCount) * 3);
    indices.resize(static_cast<size_t>(indexCount));
    m_hullMesh(m_handle, positions.data(), normals.data(),
               reinterpret_cast<int*>(indices.data()));
    return true;
}
