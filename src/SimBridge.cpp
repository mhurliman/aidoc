#include "SimBridge.h"

#include "SimClient.h"   // from external/sailboat-sim/src/Sim.Net.Client (on the include path)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

// The mirrored enum in the header must not drift from the wire values it stands for.
static_assert(static_cast<int>(SailFlying)  == SIM_PANEL_FLYING,  "panel mode drift");
static_assert(static_cast<int>(SailLuffing) == SIM_PANEL_LUFFING, "panel mode drift");
static_assert(static_cast<int>(SailStalled) == SIM_PANEL_STALLED, "panel mode drift");
static_assert(static_cast<int>(SailBacked)  == SIM_PANEL_BACKED,  "panel mode drift");

namespace {

// The prediction refuses anything above SIM_MAX_STABLE_DT because past that the rig goes chaotic
// rather than failing, so the step is pinned at the ceiling and the accumulator absorbs the rest.
constexpr float kSimStep = SIM_MAX_STABLE_DT;

// A frame that hitches must not be repaid in one go - 200 ms of catch-up at a 2.5 ms step is 80
// physics ticks in one frame, which stalls the display and hitches the next frame too.
constexpr int kMaxStepsPerFrame = 24;

// Past this the render clock did not drift, it MOVED: a reset winding the server clock back, a long
// hitch, or the first sample of the session. Easing would crawl through every second in between.
constexpr float kResyncThresholdS = 0.25f;

// Fraction of the remaining render-clock error taken out per second.
constexpr float kClockEaseRate = 0.1f;

// Sail and mast arrive in the sim's Z-up body frame; the renderer is left-handed Y-up.
void ToRenderFrame(const float in[3], float out[3])
{
    out[0] = in[0];
    out[1] = in[2];
    out[2] = in[1];
}

// Recover a quantized axis: the inverse of Axis below, and the formula SimInputFrame states.
float Unaxis(int16_t q, float minimum, float maximum)
{
    return minimum + (maximum - minimum) * (static_cast<float>(q) + 32768.0f) / 65535.0f;
}

// Quantize one control axis the way SimInputFrame documents: an int16 spanning [min, max] over the
// full -32768..32767 range. Out-of-range values clamp; they never wrap.
int16_t Axis(float value, float minimum, float maximum)
{
    const float t = (value - minimum) / (maximum - minimum);
    const float q = std::round(65535.0f * std::clamp(t, 0.0f, 1.0f)) - 32768.0f;
    return static_cast<int16_t>(std::clamp(q, -32768.0f, 32767.0f));
}

// Smallest-three: 2 bits naming the dropped (largest) component, then three 20-bit components, with
// the dropped one recovered from the unit constraint. Mirrors Quantize.UnpackOrientation in
// Sim.Net.Protocol - the packer negates the whole quaternion when the largest component is negative,
// so the recovered root is unambiguously positive.
void UnpackOrientation(uint32_t lo, uint32_t hi, float outQuat[4])
{
    constexpr int      kBits  = 20;
    constexpr uint64_t kMask  = (1ULL << kBits) - 1ULL;
    constexpr double   kSteps = static_cast<double>((1 << kBits) - 1);
    constexpr double   kRange = 0.70710678;   // 1/sqrt(2)

    const uint64_t packed = (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
    const int largest = static_cast<int>((packed >> (3 * kBits)) & 0x3ULL);

    float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int   shift = 2 * kBits;
    float sumSquares = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        if (i == largest) continue;
        const auto quantized = static_cast<double>((packed >> shift) & kMask);
        c[i] = static_cast<float>((quantized / kSteps - 0.5) * 2.0 * kRange);
        shift -= kBits;
        sumSquares += c[i] * c[i];
    }
    c[largest] = std::sqrt(std::max(0.0f, 1.0f - sumSquares));

    const float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
    const float inv = len > 1e-8f ? 1.0f / len : 1.0f;

    // (x, y, z, w) -> (-x, -z, -y, w): conjugation by the Y/Z basis swap.
    outQuat[0] = -c[0] * inv;
    outQuat[1] = -c[2] * inv;
    outQuat[2] = -c[1] * inv;
    outQuat[3] =  c[3] * inv;
}

}  // namespace

// Defined here, where SimClient.h is in scope. SimBridge::~SimBridge is out of line in this file,
// which is what lets the header hold a unique_ptr to an incomplete type.
struct HostWaveFieldStorage
{
    SimHostWaveField field {};
};

SimBridge::SimBridge() = default;

SimBridge::~SimBridge()
{
    if (m_view)   sim_view_destroy(m_view);
    if (m_client) sim_client_destroy(m_client);
}

bool SimBridge::BeginLocal(int32_t seed, float windSpeedMps, float windFromRad,
                           float significantHeightM, const char* courseId, uint16_t fleetSize,
                           WaveSampleFn waveSample, void* waveUser)
{
    const int abi = sim_abi_version();
    if (abi != SIM_ABI_VERSION)
    {
        // The header this was compiled against and the library it loaded disagree. Checked rather
        // than trusted: the failure mode otherwise is a struct read at the wrong offsets.
        fprintf(stderr, "[sim] ABI mismatch: library %d, header %d\n", abi, SIM_ABI_VERSION);
        return false;
    }

    m_params = { seed, windSpeedMps, windFromRad, significantHeightM,
                 courseId ? courseId : "", fleetSize, waveSample, waveUser };

    SimSessionDesc desc = {};
    desc.seed                 = seed;
    desc.wind_speed_mps       = windSpeedMps;
    desc.wind_from_rad        = windFromRad;
    desc.significant_height_m = significantHeightM;
    desc.fleet_size           = fleetSize;
    desc.client_mode          = SIM_CLIENT_PREDICTING;
    desc.venue_id[0]          = 0;
    std::snprintf(desc.course_id, sizeof(desc.course_id), "%s", courseId ? courseId : "");

    // Hand the sim this renderer's ocean. It replaces the seeded spectrum outright, which is the
    // point: one surface, drawn and sampled, instead of two that merely resemble each other.
    if (waveSample)
    {
        m_hostWaves = std::make_unique<HostWaveFieldStorage>();
        m_hostWaves->field.user   = waveUser;
        m_hostWaves->field.sample = waveSample;
        desc.host_waves           = &m_hostWaves->field;
    }

    const SimResult begun = sim_client_begin_local(&desc, &m_client);
    if (begun != SIM_OK)
    {
        fprintf(stderr, "[sim] begin_local failed (%d): %s\n", begun, sim_last_error());
        return false;
    }

    m_ownBoat = sim_client_get_own_boat(m_client);

    // The view is per boat and resolved by class id, the same key the renderer resolves its hull by.
    const SimResult viewed = sim_view_create("Laser", &m_view);
    if (viewed != SIM_OK)
    {
        fprintf(stderr, "[sim] view_create failed (%d): %s\n", viewed, sim_last_error());
        return false;
    }

    const int panelCount = sim_view_get_panel_count(m_view);
    m_panels.reserve(static_cast<size_t>(std::max(0, panelCount)));
    fprintf(stderr, "[sim] local session ready; own boat %d, %d sail panels, water is %s\n",
            m_ownBoat, panelCount, m_hostWaves ? "the renderer's" : "the sim's own");
    return true;
}

bool SimBridge::Restart()
{
    // Torn down in the order they were made. The view holds per-boat geometry belonging to the
    // client, so it goes first.
    if (m_view)   { sim_view_destroy(m_view);     m_view = nullptr; }
    if (m_client) { sim_client_destroy(m_client); m_client = nullptr; }
    m_hostWaves.reset();

    // Every clock and cached shape belongs to the session that just died. Left behind, the render
    // clock would resync from a large error and the pose would arrive stale for a frame.
    m_accumulator = 0.0f;
    m_renderTime  = 0.0f;
    m_clockStarted = false;
    m_phaseTime   = 0.0f;
    m_havePose    = false;
    m_clientTick  = 0;
    m_sequence    = 0;
    m_boatCount   = 0;
    m_panels.clear();
    m_mast.clear();

    const SessionParams p = m_params;   // BeginLocal overwrites m_params as it goes
    return BeginLocal(p.seed, p.windSpeedMps, p.windFromRad, p.significantHeightM,
                      p.courseId.c_str(), p.fleetSize, p.waveSample, p.waveUser);
}

bool SimBridge::Ready() const
{
    return m_client && sim_client_get_state(m_client) == SIM_CLIENT_READY;
}

void SimBridge::SetControls(float helmRad, float sheetM, float vangM, float outhaulM,
                            float centreboardRetraction, float travelerRad, float hike, float foreAft)
{
    if (!Ready()) return;

    SimInputFrame frame = {};
    frame.client_tick = m_clientTick;
    frame.sequence    = m_sequence++;
    frame.helm        = Axis(helmRad, -1.2f, 1.2f);
    frame.sheet       = Axis(sheetM, 0.0f, 12.0f);
    frame.vang        = Axis(vangM, 0.0f, 12.0f);
    frame.outhaul     = Axis(outhaulM, 0.0f, 12.0f);
    frame.centerboard = Axis(centreboardRetraction, -1.0f, 1.0f);
    frame.traveler    = Axis(travelerRad, 0.0f, 3.14159265f);
    frame.hike        = Axis(hike, -1.0f, 1.0f);
    frame.fore_aft    = Axis(foreAft, -1.0f, 1.0f);

    const SimResult rc = sim_client_input(m_client, static_cast<uint16_t>(m_ownBoat), frame);
    if (rc != SIM_OK)
    {
        fprintf(stderr, "[sim] input rejected (%d): %s\n", rc, sim_last_error());
    }
}

void SimBridge::AdvanceRenderClock(float realDt)
{
    SimClocks clocks = {};
    if (sim_client_get_clocks(m_client, m_renderTime, &clocks) != SIM_OK) return;

    // recommended_render_time is a staircase - it jumps when a snapshot lands and holds still in
    // between - so it is a TARGET, never a per-frame value. Sampling at it directly freezes the
    // fleet between snapshots and then teleports it.
    const float error = clocks.recommended_render_time - m_renderTime;
    if (!m_clockStarted || std::fabs(error) > kResyncThresholdS)
    {
        m_renderTime   = clocks.recommended_render_time;
        m_clockStarted = true;
    }
    else
    {
        m_renderTime += realDt + error * kClockEaseRate * realDt;
    }

    m_phaseTime = clocks.phase_time;
}

void SimBridge::Update(float realDt)
{
    if (!Ready() || m_paused) return;

    m_accumulator += realDt;
    int steps = 0;
    while (m_accumulator >= kSimStep && steps < kMaxStepsPerFrame)
    {
        // On a session authored with begin_local this is also what ticks the authoritative server,
        // so it has to run every frame, not only when a prediction would be visible.
        const SimResult rc = sim_client_predict_step(m_client, kSimStep);
        if (rc != SIM_OK)
        {
            fprintf(stderr, "[sim] predict_step failed (%d): %s\n", rc, sim_last_error());
            break;
        }
        m_accumulator -= kSimStep;
        ++m_clientTick;
        ++steps;
    }
    if (steps == kMaxStepsPerFrame) m_accumulator = 0.0f;   // drop the backlog rather than chase it

    AdvanceRenderClock(realDt);

    SimBoatSnapshot boats[32] = {};
    const int count = sim_client_sample(m_client, m_renderTime, boats,
                                        static_cast<int>(std::size(boats)));
    if (count < 0)
    {
        fprintf(stderr, "[sim] sample failed (%d): %s\n", count, sim_last_error());
        return;
    }
    m_boatCount = count;

    const SimBoatSnapshot* own = nullptr;
    for (int i = 0; i < count; ++i)
    {
        if (boats[i].boat_id == static_cast<uint16_t>(m_ownBoat)) { own = &boats[i]; break; }
    }
    if (!own) return;

    const float simPos[3] = {own->pose.x_mm / 1000.0f,
                             own->pose.y_mm / 1000.0f,
                             own->pose.z_mm / 1000.0f};
    ToRenderFrame(simPos, m_ownPos);
    UnpackOrientation(own->pose.orientation_lo, own->pose.orientation_hi, m_ownQuat);

    // Bounds are the ones SimRigPoseQ documents; they are not interchangeable, and reading one
    // axis with another's range is a bug that looks like a physics problem.
    m_boomYaw   = Unaxis(own->rig.boom_yaw, -2.0f, 2.0f);
    m_boomPitch = Unaxis(own->rig.boom_pitch, -1.5707963f, 1.5707963f);
    m_rudder    = Unaxis(own->rig.rudder_stock, -1.2f, 1.2f);

    const float simVel[3] = { Unaxis(own->velocity.lx, -50.0f, 50.0f),
                              Unaxis(own->velocity.ly, -50.0f, 50.0f),
                              Unaxis(own->velocity.lz, -50.0f, 50.0f) };
    ToRenderFrame(simVel, m_ownVel);
    m_havePose = true;

    // Once per FRAME per boat, not once per snapshot: the sample is a blend of the two snapshots
    // bracketing the render time, so the geometry moves every frame even while the tick does not.
    if (sim_view_update(m_view, own) != SIM_OK) return;

    const int panelCount = sim_view_get_panel_count(m_view);
    std::vector<SimPanel> raw(static_cast<size_t>(std::max(0, panelCount)));
    const int written =
        raw.empty() ? 0 : sim_view_get_panels(m_view, raw.data(), static_cast<int>(raw.size()));

    // Zero is not an error: a boat below NEAR detail carries no sail on the wire.
    m_panels.clear();
    for (int i = 0; i < std::max(0, written); ++i)
    {
        SailPanel p = {};
        ToRenderFrame(raw[i].luff_lower,  p.luffLower);
        ToRenderFrame(raw[i].luff_upper,  p.luffUpper);
        ToRenderFrame(raw[i].leech_lower, p.leechLower);
        ToRenderFrame(raw[i].leech_upper, p.leechUpper);
        ToRenderFrame(raw[i].normal,      p.normal);
        p.draftDepth    = raw[i].draft_depth;
        p.draftPosition = raw[i].draft_position;
        p.mode          = raw[i].mode;
        m_panels.push_back(p);
    }

    constexpr int kMastPoints = 16;
    float mastXyz[3 * kMastPoints] = {};
    const int pts = sim_view_get_mast(m_view, mastXyz, kMastPoints);
    m_mast.clear();
    for (int i = 0; i < std::max(0, pts); ++i)
    {
        float out[3];
        ToRenderFrame(&mastXyz[i * 3], out);
        m_mast.push_back(out[0]);
        m_mast.push_back(out[1]);
        m_mast.push_back(out[2]);
    }
}

void SimBridge::GetVelocity(float outVel[3]) const
{
    std::memcpy(outVel, m_ownVel, sizeof(m_ownVel));
}

bool SimBridge::GetMeanWind(float outVelXZ[2]) const
{
    if (!m_client) return false;

    // The sim's horizontal plane is (x, y); this renderer's is (x, z). They map straight through,
    // so the pair needs no reordering - only the name of the second axis changes.
    return sim_client_get_mean_wind(m_client, outVelXZ) == SIM_OK;
}

bool SimBridge::SampleWind(const float* xz, int count, float* outVel) const
{
    if (!m_client || count <= 0) return false;

    // render_time, deliberately, NOT the phase clock. A pause holds the swell while the weather
    // keeps running, so phase time would place the puffs wrong while they still looked plausible.
    // The horizontal plane maps straight through: a renderer xz is a sim xy.
    return sim_client_sample_wind(m_client, m_renderTime, xz, count, outVel) >= 0;
}

bool SimBridge::GetOwnPose(float outPos[3], float outQuat[4]) const
{
    if (!m_havePose) return false;
    std::memcpy(outPos, m_ownPos, sizeof(m_ownPos));
    std::memcpy(outQuat, m_ownQuat, sizeof(m_ownQuat));
    return true;
}
