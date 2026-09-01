#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Opaque ABI handles, forward-declared so SimClient.h stays out of the renderer's headers.
struct SimClient;
struct SimView;

// The ABI struct is an anonymous typedef, so it cannot be forward-declared: naming it here would
// declare a DIFFERENT type. Held behind one of ours instead, which keeps SimClient.h out of every
// translation unit that merely includes this header.
struct HostWaveFieldStorage;

// Answers the sim's water queries. Batched: (user, xy, count, t, out_elevation_m, out_slope),
// with xy 2 floats per point, elevation 1, slope 2 - (dz/dx, dz/dy). Sim world frame, +z up.
//
// Called from INSIDE Update, many times per tick, so it has to be a lookup rather than a fresh
// evaluation of a spectrum. It must not call back into SimBridge.
using WaveSampleFn = void (*)(void* user, const float* xy, int count, float t,
                              float* outElevation, float* outSlope);

// How a panel is flying. Mirrors SimPanelMode from SimProtocol.h, kept in step by the static_assert
// in SimBridge.cpp - the values are on the wire, so they cannot drift silently.
enum SailPanelMode : uint8_t
{
    SailFlying  = 0,   // streaming clean
    SailLuffing = 1,   // unloaded, not separated - still streams
    SailStalled = 2,   // flow separated: the leech telltale hooks
    SailBacked  = 3,   // wrapped forward
};

// One strip of sail cloth, already mapped into the renderer's Y-up body frame. Mirrors SimPanel from
// the sim's ABI rather than re-exporting it, so nothing above this line depends on the C header.
struct SailPanel
{
    float   luffLower[3],  luffUpper[3];
    float   leechLower[3], leechUpper[3];
    float   normal[3];        // unit, pointing to leeward; all-zero means "rebuild this one yourself"
    float   draftDepth;       // max camber / chord
    float   draftPosition;    // chord fraction of the camber peak
    uint8_t mode;             // a SimPanelMode value - drives the panel's tint
};

// Drives the C# sailing sim through its native client ABI (Sim.Net.Client, published under NativeAOT
// from the external/sailboat-sim submodule). The contract is one-way by design: this class consumes
// state and produces an input frame, and never reaches into the simulation.
//
// Everything handed out is in the RENDERER's frame. The sim is right-handed Z-up (+x bow, +y port,
// +z up); this renderer is left-handed Y-up. That basis swap happens here and only here:
//   position    (x, y, z)    -> (x, z, y)
//   orientation (x, y, z, w) -> (-x, -z, -y, w)
class SimBridge
{
public:
    ~SimBridge();

    // Both out of line, not defaulted here: a defaulted constructor in the header has to be able to
    // destroy its members if it throws, which instantiates the unique_ptr deleter at a point where
    // HostWaveFieldStorage is still incomplete.
    SimBridge();
    SimBridge(const SimBridge&)            = delete;
    SimBridge& operator=(const SimBridge&) = delete;

    // Author a local single-player session. windFromRad is METEOROLOGICAL (the direction it blows
    // from). significantHeightM of 0 asks for calm water, which publishes an empty wave component
    // list - flat water, not an uninitialised one. Returns false and logs on failure.
    // waveSample is the HOST's water, and supplying it makes this renderer the single source of
    // truth for the surface: the sim samples it instead of building a seeded spectrum, so the hull
    // floats on the ocean actually being drawn. Pass nullptr to fall back to the sim's own sea, in
    // which case significantHeightM still means something - with a host field it is ignored.
    bool BeginLocal(int32_t seed, float windSpeedMps, float windFromRad,
                    float significantHeightM, const char* courseId, uint16_t fleetSize,
                    WaveSampleFn waveSample = nullptr, void* waveUser = nullptr);

    // Put the boat back as it began. The ABI has no reset export - there is no way to ask a
    // running session to rewind - so this authors a NEW one from the parameters the last
    // BeginLocal was given. Same seed, so the same sea and the same start.
    //
    // The panel and mast counts can differ afterwards if the class ever changes, so a caller must
    // re-read them and rebuild any topology sized from them.
    bool Restart();

    // True once there is something to draw. A locally authored session is ready immediately; the
    // check exists because a received session is not, and the frame loop should not care which.
    bool Ready() const;

    // Advance one display frame by the real frame delta. Runs the prediction on a fixed-step
    // accumulator, steers the render clock, then samples the fleet and re-seats the sail geometry.
    void Update(float realDt);

    // Controls for the next predicted step, in sim units. Bounds are clamped, not wrapped:
    //   helm [-1.2, 1.2] rad   sheet/vang/outhaul [0, 12] m   centreboard/hike/foreAft [-1, 1]
    //   traveler [0, pi] rad - an ANGLE on the traveler arc, centred at pi/2, NOT a slider
    void SetControls(float helmRad, float sheetM, float vangM, float outhaulM,
                     float centreboardRetraction, float travelerRad, float hike, float foreAft);

    // Own boat pose in the renderer's Y-up world frame. False before the first successful sample.
    bool GetOwnPose(float outPos[3], float outQuat[4]) const;

    // This frame's sail, in the renderer's Y-up BODY frame - apply the boat's world transform to it.
    const std::vector<SailPanel>& Panels() const { return m_panels; }

    // Mast centreline, deck to masthead, 3 floats per point, renderer Y-up body frame.
    const std::vector<float>& MastPoints() const { return m_mast; }

    // This frame's rig angles, dequantized from the snapshot. The boom's are what the spars and
    // every line hanging off them are posed by; the rudder angle is the blade's ACHIEVED angle,
    // not the helm command, so a blade that has not caught up is drawn where it actually is.
    float BoomYaw() const   { return m_boomYaw; }     // rad, boom-end toward port positive
    float BoomPitch() const { return m_boomPitch; }   // rad, boom-end DOWN positive
    float RudderAngle() const { return m_rudder; }    // rad

    // Own boat's linear velocity, renderer Y-up world frame. Needed for apparent wind, which is
    // what a masthead fly actually points into - a fly keyed to true wind ignores the boat.
    void GetVelocity(float outVel[3]) const;

    // The un-perturbed mean breeze: one vector for the whole course, in the renderer's world XZ.
    // This is the one that holds still while gusts and shifts move over it, which is what makes it
    // safe to key a wave spectrum to - the sampled field would rebuild the spectrum every frame.
    // False if unavailable.
    bool GetMeanWind(float outVelXZ[2]) const;

    // Ambient wind at world XZ points, renderer frame, 2 floats out per point (the horizontal
    // velocity the air is MOVING at, not the meteorological "from"). False if unavailable.
    bool SampleWind(const float* xz, int count, float* outVel) const;

    // What the WAVE FIELD must be evaluated at, and the only thing that may use it. A pause freezes
    // this while the render clock keeps running, which is the whole reason it is a separate clock.
    float PhaseTime() const { return m_phaseTime; }

    // Pausing is the renderer's call in this API: stop stepping and the world holds still. There is
    // no pause export to call, and phase time stops advancing with the prediction.
    void SetPaused(bool paused) { m_paused = paused; }
    bool Paused() const { return m_paused; }

    int  BoatCount() const { return m_boatCount; }

private:
    void AdvanceRenderClock(float realDt);

    SimClient* m_client = nullptr;
    SimView*   m_view   = nullptr;

    // Held for the life of the session: the sim keeps the pointer it was handed at begin_local,
    // so a temporary would leave it reading freed stack on the first tick.
    std::unique_ptr<HostWaveFieldStorage> m_hostWaves;

    // Everything BeginLocal was called with, so Restart can call it again identically.
    struct SessionParams
    {
        int32_t      seed = 0;
        float        windSpeedMps = 0.0f;
        float        windFromRad = 0.0f;
        float        significantHeightM = 0.0f;
        std::string  courseId;
        uint16_t     fleetSize = 1;
        WaveSampleFn waveSample = nullptr;
        void*        waveUser = nullptr;
    };
    SessionParams m_params;

    int      m_ownBoat   = 0;
    int      m_boatCount = 0;
    uint32_t m_clientTick = 0;
    uint16_t m_sequence   = 0;

    float m_accumulator = 0.0f;   // unspent real time, drained in fixed sim steps
    float m_renderTime  = 0.0f;   // our own clock, eased toward the sim's recommendation
    bool  m_clockStarted = false;
    float m_phaseTime   = 0.0f;
    bool  m_paused      = false;
    bool  m_havePose    = false;

    float m_ownPos[3]  = {0.0f, 0.0f, 0.0f};
    float m_ownQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float m_ownVel[3]  = {0.0f, 0.0f, 0.0f};
    float m_boomYaw    = 0.0f;
    float m_boomPitch  = 0.0f;
    float m_rudder     = 0.0f;

    std::vector<SailPanel> m_panels;
    std::vector<float>     m_mast;
};
