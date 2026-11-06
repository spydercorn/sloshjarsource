#include "PCH.h"

#include <RE/B/BSVisit.h>
#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShader.h>
#include <RE/B/BSGeometry.h>
#include <RE/B/BSTriShape.h>
#include <RE/Offsets_VTABLE.h>
#include <REL/Relocation.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiMatrix3.h>
#include <RE/N/NiIntegerExtraData.h>
#include <RE/N/NiColor.h>
#include <RE/R/Renderer.h>
#include <RE/S/ScriptEventSourceHolder.h>
#include <RE/T/TES.h>
#include <RE/T/TESCellAttachDetachEvent.h>
#include <RE/T/TESObjectREFR.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>  // DirectX SDK compiler interface, called through a late-bound system DLL
#include <wrl/client.h>


// Milestone G: per-jar reactive movement slosh in the proven SECOND draw in BSLightingShader::RestoreGeometry,
// after Skyrim draws tagged Jar03 normally. A separate PSGravityPlane tints
// RTV0 RGB via blending; RTV1/RTV2 and original alpha should be preserved.
// No Data/Shaders/Lighting.hlsl override and no replacement of Open Shaders.
// Earlier red/clip replacement modes remain as opt-in debugging fallbacks.
// THIS IS NOT PRODUCTION-READY: other renderers may change geometry, targets,
// or the timing/ownership of the immediate-context render state.
namespace
{
    const RE::BSFixedString kMarkerName{ "FilledJarLiquid" };

    enum class VisualMode { kOff, kGreen, kHide };
    VisualMode g_mode = VisualMode::kOff;

    // Resolve the INI next to THIS DLL (not relative to PowerShell's or Skyrim's working directory).
    // This also works when a mod manager loads the DLL from its own mod directory.
    VisualMode ReadVisualMode()
    {
        auto module = ::GetModuleHandleW(L"FilledJarMarkerDetector.dll");
        if (!module) {
            return VisualMode::kOff;
        }
        wchar_t modulePath[MAX_PATH]{};
        if (!::GetModuleFileNameW(module, modulePath, MAX_PATH)) {
            return VisualMode::kOff;
        }
        const auto iniPath = std::filesystem::path(modulePath).parent_path() / L"FilledJarVisualTest.ini";
        wchar_t modeText[32]{};
        ::GetPrivateProfileStringW(L"Visual", L"Mode", L"Off", modeText, 32, iniPath.c_str());
        std::wstring mode{ modeText };
        for (auto& ch : mode) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        if (mode == L"hide") {
            return VisualMode::kHide;
        }
        if (mode == L"off") {
            return VisualMode::kOff;
        }
        return mode == L"green" ? VisualMode::kGreen : VisualMode::kOff;
    }

    const char* ModeName(VisualMode mode)
    {
        switch (mode) {
        case VisualMode::kHide: return "Hide";
        case VisualMode::kGreen: return "Green";
        default: return "Off";
        }
    }

    void ApplyVisualTest(RE::NiAVObject* glass, RE::FormID refID, const char* origin)
    {
        if (!glass) {
            return;
        }
        switch (g_mode) {
        case VisualMode::kGreen:
            // Vanilla's tint/effect-data path may be ignored by some shader/material types.
            // Log that the tint was REQUESTED, not that the jar necessarily turned green.
            glass->TintScenegraph(RE::NiColorA{ 0.0f, 1.0f, 0.0f, 1.0f });
            spdlog::info("VISUAL GREEN REQUESTED [{}]: ref {:08X}, mesh '{}'", origin, refID, glass->name.c_str());
            break;
        case VisualMode::kHide:
            // Reliable isolation test: only the glass shape is application-culled; cork remains.
            // This is transient game memory state, not a saved NIF or ESP mutation.
            glass->SetAppCulled(true);
            spdlog::info("VISUAL HIDE [{}]: ref {:08X}, mesh '{}', appCulled={}",
                origin, refID, glass->name.c_str(), glass->GetAppCulled());
            break;
        case VisualMode::kOff:
            spdlog::info("VISUAL OFF [{}]: ref {:08X}, mesh '{}'", origin, refID, glass->name.c_str());
            break;
        }
    }

    // The original VSMain and PSMain are still probe-only. Separately compile
    // PSDiagnosticRed, keeping its GPU object for an optional per-jar red swap.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> g_redDiagnosticPS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> g_gravityPlanePS;
    // Overlay-only D3D states, created lazily on the render thread.
    Microsoft::WRL::ComPtr<ID3D11BlendState> g_overlayBlend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> g_overlayDepth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> g_overlayDepthSource;
    Microsoft::WRL::ComPtr<ID3D11Buffer> g_sloshPerJarBuffer;



    // This remains independent of Data/Shaders and does not modify CS files.
    // D3DCompiler_47 is a Windows system component; no new vcpkg packages or
    // changes to the CommunityShaders shader directory are needed.
    void ProbeShaderCompilation()
    {
        HMODULE ourModule = ::GetModuleHandleW(L"FilledJarMarkerDetector.dll");
        if (!ourModule) {
            spdlog::error("LIQUID SHADER PROBE: could not locate this plugin DLL");
            return;
        }
        wchar_t modulePath[MAX_PATH]{};
        if (!::GetModuleFileNameW(ourModule, modulePath, MAX_PATH)) {
            spdlog::error("LIQUID SHADER PROBE: GetModuleFileNameW failed");
            return;
        }
        const auto shader = std::filesystem::path(modulePath).parent_path() /
                            L"FilledJar" / L"FilledJarLiquid.hlsl";
        if (!std::filesystem::is_regular_file(shader)) {
            spdlog::error("LIQUID SHADER PROBE: source file missing: {}", shader.string());
            return;
        }

        auto* compiler = ::LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!compiler) {
            spdlog::error("LIQUID SHADER PROBE: Windows d3dcompiler_47.dll unavailable, error={}",
                          ::GetLastError());
            return;
        }
        // Do not link to d3dcompiler.lib: this remains optional, and all errors
        // are logged without preventing Skyrim from starting.
        using CompileFn = decltype(&D3DCompileFromFile);
        auto compile = reinterpret_cast<CompileFn>(::GetProcAddress(compiler, "D3DCompileFromFile"));
        if (!compile) {
            spdlog::error("LIQUID SHADER PROBE: D3DCompileFromFile entry point missing");
            ::FreeLibrary(compiler);
            return;
        }

        auto compileStage = [&](const char* entry, const char* target) {
            Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
            Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
            auto hr = compile(shader.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                              entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                              bytecode.GetAddressOf(), diagnostics.GetAddressOf());
            if (FAILED(hr)) {
                if (diagnostics && diagnostics->GetBufferPointer()) {
                    spdlog::error("LIQUID SHADER PROBE: {} / {} failed: {}", entry, target,
                        std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                                    diagnostics->GetBufferSize()));
                } else {
                    spdlog::error("LIQUID SHADER PROBE: {} / {} failed, HRESULT={:08X}",
                        entry, target, static_cast<std::uint32_t>(hr));
                }
                return false;
            }
            spdlog::info("LIQUID SHADER PROBE: COMPILED {} / {}, {} byte(s)",
                         entry, target, bytecode->GetBufferSize());
            // A GPU creation smoke test on Skyrim's real device. CreateShader
            // does NOT bind anything or modify render state. The temporary
            // GPU object is released immediately afterward.
            // Newer CommonLibSSE-NG keeps renderer data behind runtime-aware
            // accessors; Renderer::data is not a public member. Obtain the
            // D3D11 device through its supported static accessor instead.
            // CommonLib's REX::W32 interface mirrors the Windows COM ABI.
            auto* gameDevice = RE::BSGraphics::Renderer::GetDevice();
            auto* device = reinterpret_cast<ID3D11Device*>(gameDevice);
            if (!device) {
                spdlog::warn("LIQUID SHADER PROBE: game D3D11 device not available; GPU creation skipped");
                return true;
            }
            HRESULT createHr = E_FAIL;
            if (target[0] == 'v') {
                Microsoft::WRL::ComPtr<ID3D11VertexShader> vsObject;
                createHr = device->CreateVertexShader(bytecode->GetBufferPointer(),
                    bytecode->GetBufferSize(), nullptr, vsObject.GetAddressOf());
            } else {
                Microsoft::WRL::ComPtr<ID3D11PixelShader> psObject;
                createHr = device->CreatePixelShader(bytecode->GetBufferPointer(),
                    bytecode->GetBufferSize(), nullptr, psObject.GetAddressOf());
                if (SUCCEEDED(createHr) && std::strcmp(entry, "PSDiagnosticRed") == 0) {
                    g_redDiagnosticPS = psObject; // retain across draws
                }
                if (SUCCEEDED(createHr) && std::strcmp(entry, "PSGravityPlane") == 0) {
                    g_gravityPlanePS = psObject; // pixel-stage-only gravity test
                }
            }
            if (SUCCEEDED(createHr)) {
                spdlog::info("LIQUID SHADER PROBE: GPU CREATE {} OK (not bound to a draw)", target);
            } else {
                spdlog::error("LIQUID SHADER PROBE: GPU CREATE {} failed HRESULT={:08X}",
                              target, static_cast<std::uint32_t>(createHr));
            }
            return true;
        };

        const bool vs = compileStage("VSMain", "vs_5_0");
        const bool ps = compileStage("PSMain", "ps_5_0");
        const bool red = compileStage("PSDiagnosticRed", "ps_5_0");
        const bool gravity = compileStage("PSGravityPlane", "ps_5_0");
        spdlog::info("LIQUID SHADER PROBE: vertex={}, originalLiquid={}, diagnosticRed={}, gravity={}, redGPU={}, gravityGPU={}",
                     vs ? "OK" : "FAILED", ps ? "OK" : "FAILED",
                     red ? "OK" : "FAILED", gravity ? "OK" : "FAILED",
                     g_redDiagnosticPS ? "READY" : "MISSING", g_gravityPlanePS ? "READY" : "MISSING");
        ::FreeLibrary(compiler);
    }


    // Paired BSLightingShader geometry methods are a smaller hook surface than
    // intercepting ALL D3D11 draws. Both need to be hooked for restoration.
    // Do not assume the first set of matching passes works for every camera or
    // every shader permutation: this intentionally uses a known test signature.
    using GeometryFn = void (*)(RE::BSShader*, RE::BSRenderPass*, std::uint32_t);
    GeometryFn g_originalSetupGeometry = nullptr;
    GeometryFn g_originalRestoreGeometry = nullptr;
    bool g_traceRequested = false;
    bool g_redRequested = false;
    bool g_gravityRequested = false;
    bool g_overlayRequested = false;
    bool g_movementSloshRequested = false;
    float g_sloshFrequency = 1.05f; // rad/sec: ~6 sec full natural oscillation
    float g_sloshDamping = 0.24f; // damping ratio
    float g_sloshTranslation = 0.0013f; // change in horizontal velocity -> impulse
    float g_sloshRotation = 0.90f; // change of jar axis -> impulse
    float g_sloshMaxSlope = 0.28f; // tan(15.6 degrees)
    std::atomic<std::uint32_t> g_sloshKickLogs{ 0 };
    std::atomic<std::uint32_t> g_overlayAttempts{ 0 };
    std::atomic<std::uint32_t> g_overlaySkipped{ 0 };
    std::uint32_t g_maxTraceLines = 12;
    std::atomic<std::uint32_t> g_traceMatches{ 0 };
    std::atomic<std::uint32_t> g_redAttempts{ 0 };
    std::atomic<std::uint32_t> g_redRestores{ 0 };
    std::atomic<std::uint32_t> g_redSkipped{ 0 };
    std::atomic<bool> g_firstPassSeen{ false };
    bool g_hooksInstalled = false;

    // The exact signature observed in the user's successful jar render trace.
    // As a fail-closed diagnostic restriction, this is not a general jar shader ID.
    constexpr std::uint32_t kObservedJarPass = 0x4900022D;
    constexpr std::uint32_t kObservedJarFlags = 0x45;

    // Render-thread-only, transient slosh state. No save-game changes or global shader overrides.
    // Keyed by the tagged glass geometry pointer; inactive entries are aged out so
    // unloaded cells don't retain state indefinitely. Not accessed by game-thread scans.
    struct JarSloshState {
        std::chrono::steady_clock::time_point lastSample{};
        std::array<float, 3> lastPos{ 0, 0, 0 };
        std::array<float, 3> lastAxis{ 0, 0, 1 };
        std::array<float, 3> lastVelocity{ 0, 0, 0 };
        std::array<float, 2> slope{ 0, 0 }; // world-space free-surface slope XY
        std::array<float, 2> slopeVelocity{ 0, 0 }; // slope change / second
        bool initialized = false;
    };
    std::unordered_map<const RE::BSGeometry*, JarSloshState> g_jarSloshStates;
    std::chrono::steady_clock::time_point g_lastSloshCleanup{};

    // D3D11 exposes PS constant-buffer slots 0..13 (14 total). Never use b14.
    constexpr UINT kSloshConstantSlot = 12;
    static_assert(kSloshConstantSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);

    struct alignas(16) JarSloshConstantData {
        float slopeX;
        float slopeY;
        float reserved0;
        float reserved1;
    };
    static_assert(sizeof(JarSloshConstantData) == 16);

    // A slow, underdamped spring is excited only by changes in jar motion.
    // This is visual inertia rather than an actual fluid simulation.
    JarSloshConstantData AdvanceJarSlosh(const RE::BSGeometry* glass)
    {
        JarSloshConstantData output{};
        if (!g_movementSloshRequested || !glass) return output;

        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        auto& state = g_jarSloshStates[glass];
        const auto& pose = glass->world;
        const std::array<float, 3> pos{ pose.translate.x, pose.translate.y, pose.translate.z };
        // NiMatrix3 uses entry[row][column]. The third column is the jar's
        // local +Z axis expressed in WORLD coordinates; rotation impulses
        // use its change, not the camera position or the Open Shaders matrix.
        const std::array<float, 3> axis{
            pose.rotate.entry[0][2], pose.rotate.entry[1][2], pose.rotate.entry[2][2]
        };
        if (!state.initialized) {
            state.lastPos = pos;
            state.lastAxis = axis;
            state.lastSample = now;
            state.initialized = true;
            return output;
        }

        const float rawDt = std::chrono::duration<float>(now - state.lastSample).count();
        // Same jar can be reached multiple times within one frame. Reuse the
        // existing state rather than applying an impulse for each draw pass.
        if (rawDt < 1.0f / 180.0f) {
            return { state.slope[0], state.slope[1], 0, 0 };
        }
        if (rawDt > 0.40f) {
            // Cell reload, pause, or teleport: do not turn it into a giant kick.
            state.lastPos = pos;
            state.lastAxis = axis;
            state.lastVelocity = { 0, 0, 0 };
            state.slope = { 0, 0 };
            state.slopeVelocity = { 0, 0 };
            state.lastSample = now;
            return output;
        }
        const float dt = std::clamp(rawDt, 1.0f / 180.0f, 0.08f);

        std::array<float, 3> dPos{};
        for (int i = 0; i < 3; ++i) dPos[i] = pos[i] - state.lastPos[i];
        const float displacement = std::sqrt(dPos[0]*dPos[0] + dPos[1]*dPos[1] + dPos[2]*dPos[2]);
        if (displacement > 220.0f) {
            state.lastVelocity = { 0, 0, 0 };
            state.lastPos = pos;
            state.lastAxis = axis;
            state.lastSample = now;
            return { state.slope[0], state.slope[1], 0, 0 };
        }

        std::array<float, 3> vel{};
        for (int i = 0; i < 3; ++i) vel[i] = dPos[i] / rawDt;
        // Ignore almost-zero movement from scene graph floating-point jitter.
        for (int i = 0; i < 2; ++i) {
            float dv = vel[i] - state.lastVelocity[i];
            if (std::abs(dv) < 2.0f) dv = 0.0f;
            const float tiltChange = axis[i] - state.lastAxis[i];
            const float translationKick = -g_sloshTranslation * std::clamp(dv, -500.0f, 500.0f);
            const float rotationKick = -g_sloshRotation * tiltChange;
            float impulse = std::clamp(translationKick + rotationKick, -0.65f, 0.65f);
            state.slopeVelocity[i] += impulse;
            if (std::abs(impulse) > 0.09f) {
                const auto n = g_sloshKickLogs.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 12) {
                    spdlog::info("MOVEMENT SLOSH: kick #{} axis={} magnitude={:.3f} velocityChange={:.1f} axisChange={:.3f}",
                        n, i, impulse, dv, tiltChange);
                }
            }
        }

        // Semi-implicit Euler on d2x/dt2 + 2*zeta*omega*dx/dt + omega^2*x=0.
        // Use up to four substeps when a frame is late, keeping the slow motion
        // stable without changing the target liquid volume.
        const int steps = std::clamp(static_cast<int>(std::ceil(dt / 0.018f)), 1, 5);
        const float h = dt / static_cast<float>(steps);
        for (int step = 0; step < steps; ++step) {
            for (int i = 0; i < 2; ++i) {
                state.slopeVelocity[i] += (-g_sloshFrequency * g_sloshFrequency * state.slope[i]
                    - 2.0f * g_sloshDamping * g_sloshFrequency * state.slopeVelocity[i]) * h;
                state.slope[i] += state.slopeVelocity[i] * h;
            }
        }
        const float slopeMagnitude = std::hypot(state.slope[0], state.slope[1]);
        if (slopeMagnitude > g_sloshMaxSlope && slopeMagnitude > 1e-5f) {
            const float scale = g_sloshMaxSlope / slopeMagnitude;
            state.slope[0] *= scale;
            state.slope[1] *= scale;
            state.slopeVelocity[0] *= 0.55f;
            state.slopeVelocity[1] *= 0.55f;
        }
        state.lastPos = pos;
        state.lastAxis = axis;
        state.lastVelocity = vel;
        state.lastSample = now;

        // Retain states only for recently rendered objects, and bound memory.
        if (g_jarSloshStates.size() > 96 && now - g_lastSloshCleanup > std::chrono::seconds(5)) {
            g_lastSloshCleanup = now;
            for (auto it = g_jarSloshStates.begin(); it != g_jarSloshStates.end();) {
                if (now - it->second.lastSample > std::chrono::seconds(20))
                    it = g_jarSloshStates.erase(it);
                else ++it;
            }
        }
        output.slopeX = state.slope[0];
        output.slopeY = state.slope[1];
        return output;
    }

    // Thread-local because Skyrim invokes the per-geometry methods on the render
    // thread; avoid letting state from one thread restore another thread's PS.
    struct PixelSwapState
    {
        RE::BSRenderPass* pass = nullptr;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> previousPS;
        // Only used in Gravity mode. We mirror VS b2 -> PS b13 for the original
        // world matrix; restore PS b13 before the next object renders.
        Microsoft::WRL::ComPtr<ID3D11Buffer> previousPSB13;
        bool changedB13 = false;
        bool active = false;
    };
    thread_local PixelSwapState t_pixelSwap;

    float ReadIniFloat(const std::filesystem::path& ini, const wchar_t* key, float fallback, float min, float max)
    {
        wchar_t result[64]{};
        ::GetPrivateProfileStringW(L"MovementSlosh", key, L"", result, 64, ini.c_str());
        if (!result[0]) return fallback;
        wchar_t* end = nullptr;
        const float parsed = std::wcstof(result, &end);
        return (end == result || !std::isfinite(parsed)) ? fallback : std::clamp(parsed, min, max);
    }

    void ReadTraceSettings()
    {
        const HMODULE module = ::GetModuleHandleW(L"FilledJarMarkerDetector.dll");
        wchar_t modulePath[MAX_PATH]{};
        if (!module || !::GetModuleFileNameW(module, modulePath, MAX_PATH)) {
            spdlog::warn("RENDER TRACE: unable to find INI path; tracing and red swap disabled");
            return;
        }
        const auto iniPath = std::filesystem::path(modulePath).parent_path() / L"FilledJarVisualTest.ini";
        g_traceRequested = ::GetPrivateProfileIntW(L"RenderTrace", L"Enabled", 0, iniPath.c_str()) == 1;
        const auto limit = ::GetPrivateProfileIntW(L"RenderTrace", L"MaxMatches", 12, iniPath.c_str());
        g_maxTraceLines = static_cast<std::uint32_t>(std::clamp(limit, 1u, 32u));
        g_redRequested = ::GetPrivateProfileIntW(L"RedPixelTest", L"Enabled", 0, iniPath.c_str()) == 1;
        g_gravityRequested = ::GetPrivateProfileIntW(L"GravityPlaneTest", L"Enabled", 0, iniPath.c_str()) == 1;
        g_overlayRequested = ::GetPrivateProfileIntW(L"LiquidOverlay", L"Enabled", 0, iniPath.c_str()) == 1;
        g_movementSloshRequested = ::GetPrivateProfileIntW(L"MovementSlosh", L"Enabled", 0, iniPath.c_str()) == 1;
        g_sloshFrequency = ReadIniFloat(iniPath, L"Frequency", 1.05f, 0.35f, 8.0f);
        g_sloshDamping = ReadIniFloat(iniPath, L"Damping", 0.24f, 0.05f, 2.0f);
        g_sloshTranslation = ReadIniFloat(iniPath, L"TranslationImpulse", 0.0013f, 0.0f, 0.02f);
        g_sloshRotation = ReadIniFloat(iniPath, L"RotationImpulse", 0.90f, 0.0f, 5.0f);
        g_sloshMaxSlope = std::tan(ReadIniFloat(iniPath, L"MaxTiltDegrees", 16.0f, 1.0f, 30.0f) * 3.14159265358979323846f / 180.0f);
        if (!g_overlayRequested) g_movementSloshRequested = false;
        spdlog::info("MOVEMENT SLOSH: enabled={}, frequency={:.2f} rad/s, damping={:.2f}, translate={:.5f}, rotate={:.2f}, maxSlope={:.2f}",
            g_movementSloshRequested, g_sloshFrequency, g_sloshDamping, g_sloshTranslation, g_sloshRotation, g_sloshMaxSlope);
        if (g_overlayRequested) {
            // The overlay is a SECOND draw; disable both replacement modes.
            g_gravityRequested = false;
            g_redRequested = false;
        } else if (g_gravityRequested) {
            g_redRequested = false;
        }
        spdlog::info("RENDER TRACE: requested={}, max_logged_passes={}", g_traceRequested, g_maxTraceLines);
        spdlog::info("PIXEL TEST: gravity={}, solidRed={}, overlay={} (Jar03 marker=1 pass=0x{:08X} flags=0x{:02X})",
            g_gravityRequested, g_redRequested, g_overlayRequested, kObservedJarPass, kObservedJarFlags);
    }

    bool IsMarkedJarPass(const RE::BSRenderPass* pass)
    {
        if (!pass || !pass->geometry) {
            return false;
        }
        auto* geom = pass->geometry;
        const char* name = geom->name.c_str();
        if (!name || std::strcmp(name, "Jar03") != 0) {
            return false;
        }
        const auto* tag = geom->GetExtraData<RE::NiIntegerExtraData>(kMarkerName);
        return tag && tag->value == 1;
    }

    void RestorePixelShader(const char* why)
    {
        if (!t_pixelSwap.active) {
            return;
        }
        if (t_pixelSwap.context) {
            // Restore the original pixel shader and the single constant-buffer
            // slot changed for gravity mode. Do not alter the original VS.
            t_pixelSwap.context->PSSetShader(t_pixelSwap.previousPS.Get(), nullptr, 0);
            if (t_pixelSwap.changedB13) {
                ID3D11Buffer* original = t_pixelSwap.previousPSB13.Get();
                t_pixelSwap.context->PSSetConstantBuffers(13, 1, &original);
            }
        }
        const auto n = g_redRestores.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= g_maxTraceLines) {
            spdlog::info("PIXEL TEST: restored original PS{} #{} via {}",
                t_pixelSwap.changedB13 ? " and PS b13" : "", n, why);
        }
        t_pixelSwap.active = false;
        t_pixelSwap.pass = nullptr;
        t_pixelSwap.previousPSB13.Reset();
        t_pixelSwap.changedB13 = false;
        t_pixelSwap.previousPS.Reset();
        t_pixelSwap.context.Reset();
    }

    void TrySwapPixelShader(RE::BSRenderPass* pass, std::uint32_t flags)
    {
        if ((!g_redRequested && !g_gravityRequested) ||
            (g_gravityRequested && !g_gravityPlanePS) ||
            (g_redRequested && !g_redDiagnosticPS)) {
            return;
        }
        if (pass->passEnum != kObservedJarPass || flags != kObservedJarFlags) {
            const auto n = g_redSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4) {
                spdlog::info("RED PIXEL TEST: tagged jar skipped (unrecognized pass=0x{:08X}, flags=0x{:02X})",
                    pass->passEnum, flags);
            }
            return;
        }
        auto* gameDevice = RE::BSGraphics::Renderer::GetDevice();
        auto* device = reinterpret_cast<ID3D11Device*>(gameDevice);
        if (!device) {
            spdlog::warn("RED PIXEL TEST: no D3D11 device; skipped");
            return;
        }
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.GetAddressOf());
        if (!context) {
            spdlog::warn("RED PIXEL TEST: no immediate context; skipped");
            return;
        }
        Microsoft::WRL::ComPtr<ID3D11PixelShader> originalPS;
        context->PSGetShader(originalPS.GetAddressOf(), nullptr, nullptr);
        if (!originalPS) {
            spdlog::warn("RED PIXEL TEST: no original PS bound; skipped");
            return;
        }
        // Inspect, but DO NOT change, how many render targets the active
        // pass uses. An MRT deferred pass may not show solid red because our
        // diagnostic PS deliberately writes only SV_Target0.
        ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, nullptr);
        std::uint32_t renderTargetCount = 0;
        for (auto*& target : renderTargets) {
            if (target) {
                ++renderTargetCount;
                target->Release(); // OMGetRenderTargets AddRefs returned COM pointers
                target = nullptr;
            }
        }
        // For Gravity mode, reuse Skyrim/Open Shaders' VERTEX-stage b2.
        // On this observed lighting permutation it begins with a row_major
        // float3x4 World matrix (the same one used to emit POSITION1).
        // Mirror it to PS b13; our independent pixel shader uses that matrix
        // only to calculate the world-space height of model point (0,0,20).
        Microsoft::WRL::ComPtr<ID3D11Buffer> previousPSB13;
        bool changedB13 = false;
        if (g_gravityRequested) {
            Microsoft::WRL::ComPtr<ID3D11Buffer> vsPerGeometry;
            context->VSGetConstantBuffers(2, 1, vsPerGeometry.GetAddressOf());
            if (!vsPerGeometry) {
                spdlog::warn("GRAVITY PLANE: vertex shader b2 is unbound; skipped marked pass");
                return;
            }
            D3D11_BUFFER_DESC desc{};
            vsPerGeometry->GetDesc(&desc);
            if (desc.ByteWidth < 3 * 4 * sizeof(float)) {
                spdlog::warn("GRAVITY PLANE: vertex shader b2 too small ({} bytes)", desc.ByteWidth);
                return;
            }
            context->PSGetConstantBuffers(13, 1, previousPSB13.GetAddressOf());
            ID3D11Buffer* worldParams = vsPerGeometry.Get();
            context->PSSetConstantBuffers(13, 1, &worldParams);
            changedB13 = true;
        }
        // Make the swap only after the game's SetupGeometry (including any
        // upstream Open Shaders hooks) has returned, and before the actual draw.
        context->PSSetShader(g_gravityRequested ? g_gravityPlanePS.Get() : g_redDiagnosticPS.Get(), nullptr, 0);
        t_pixelSwap.pass = pass;
        t_pixelSwap.previousPS = std::move(originalPS);
        t_pixelSwap.previousPSB13 = std::move(previousPSB13);
        t_pixelSwap.changedB13 = changedB13;
        t_pixelSwap.context = std::move(context);
        t_pixelSwap.active = true;
        const auto n = g_redAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= g_maxTraceLines) {
            spdlog::info("PIXEL TEST: bound {} #{} on tagged Jar03 (pass=0x{:08X}, flags=0x{:02X}, activeRTVs={}, VS b2->PS b13={})",
                g_gravityRequested ? "GRAVITY PLANE" : "SOLID RED", n, pass->passEnum, flags,
                renderTargetCount, changedB13);
        }
    }

    // On the known Jar03 lighting pass, the engine's own draw has just executed
    // when RestoreGeometry is called. Reuse its IA buffers, vertex shader, RTVs,
    // and transforms for a second, alpha-blended draw. Nothing global is patched.
    // This is deliberately opt-in, tightly guarded, and NOT production-ready.
    void DrawLiquidOverlay(RE::BSRenderPass* pass, std::uint32_t flags)
    {
        if (!g_overlayRequested || !g_gravityPlanePS || !IsMarkedJarPass(pass) ||
            pass->passEnum != kObservedJarPass || flags != kObservedJarFlags) {
            return;
        }
        auto skip = [&](const char* why) {
            const auto n = g_overlaySkipped.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6) spdlog::warn("LIQUID OVERLAY: skipped ({})", why);
        };

        // This proof of concept supports the jar geometry observed in RenderDoc:
        // BSTriShape, 176 triangles, 528 uint16 indices, triangle-list topology.
        auto* tri = pass->geometry->AsTriShape();
        if (!tri || tri->GetTrishapeRuntimeData().triangleCount != 176) {
            skip("not the observed 176-triangle Jar03 mesh");
            return;
        }
        auto* device = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
        if (!device) { skip("D3D11 device unavailable"); return; }
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(ctx.GetAddressOf());
        if (!ctx) { skip("immediate context unavailable"); return; }

        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        UINT indexOffset = 0;
        ctx->IAGetIndexBuffer(indexBuffer.GetAddressOf(), &indexFormat, &indexOffset);
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetPrimitiveTopology(&topology);
        if (!indexBuffer || indexFormat != DXGI_FORMAT_R16_UINT || indexOffset != 0 ||
            topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
            skip("index buffer/topology differs from observed jar draw");
            return;
        }

        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthView;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, depthView.GetAddressOf());
        UINT activeRTVs = 0;
        for (auto*& rtv : rtvs) {
            if (rtv) { ++activeRTVs; rtv->Release(); rtv = nullptr; }
        }
        // 3 targets on the user's tested pass, with an attached depth view.
        // Fail closed if a graphics mod changes this layout.
        if (activeRTVs != 3 || !depthView) {
            skip("unexpected output-merger target/depth layout");
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11PixelShader> originalPS;
        Microsoft::WRL::ComPtr<ID3D11Buffer> priorB13;
        Microsoft::WRL::ComPtr<ID3D11Buffer> priorSloshBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> perGeometry;
        Microsoft::WRL::ComPtr<ID3D11BlendState> originalBlend;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> originalDepth;
        FLOAT originalBlendFactor[4]{};
        UINT originalSampleMask = 0;
        UINT originalStencilRef = 0;
        ctx->PSGetShader(originalPS.GetAddressOf(), nullptr, nullptr);
        ctx->PSGetConstantBuffers(13, 1, priorB13.GetAddressOf());
        ctx->PSGetConstantBuffers(kSloshConstantSlot, 1, priorSloshBuffer.GetAddressOf());
        ctx->VSGetConstantBuffers(2, 1, perGeometry.GetAddressOf());
        ctx->OMGetBlendState(originalBlend.GetAddressOf(), originalBlendFactor, &originalSampleMask);
        ctx->OMGetDepthStencilState(originalDepth.GetAddressOf(), &originalStencilRef);
        if (!originalPS || !perGeometry || !originalDepth) {
            skip("missing original PS, VS b2, or depth state");
            return;
        }
        D3D11_BUFFER_DESC cbDesc{};
        perGeometry->GetDesc(&cbDesc);
        if (cbDesc.ByteWidth < 48) { skip("VS b2 too small for World matrix"); return; }

        if (!g_overlayBlend) {
            D3D11_BLEND_DESC bd{};
            bd.IndependentBlendEnable = TRUE;
            for (auto& target : bd.RenderTarget) {
                target.BlendEnable = FALSE;
                target.SrcBlend = D3D11_BLEND_ONE;
                target.DestBlend = D3D11_BLEND_ZERO;
                target.BlendOp = D3D11_BLEND_OP_ADD;
                target.SrcBlendAlpha = D3D11_BLEND_ONE;
                target.DestBlendAlpha = D3D11_BLEND_ZERO;
                target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                target.RenderTargetWriteMask = 0; // RTV1..RTV7 are untouched
            }
            auto& tint = bd.RenderTarget[0];
            tint.BlendEnable = TRUE;
            tint.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            tint.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            tint.BlendOp = D3D11_BLEND_OP_ADD;
            // Preserve the original alpha channel, only change RGB.
            tint.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
                D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
            const HRESULT hr = device->CreateBlendState(&bd, g_overlayBlend.GetAddressOf());
            if (FAILED(hr)) { skip("CreateBlendState failed"); return; }
        }
        if (!g_overlayDepth || g_overlayDepthSource.Get() != originalDepth.Get()) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            originalDepth->GetDesc(&dd);
            const bool originallyWroteDepth = dd.DepthEnable &&
                dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            // If the original draw wrote depth, redraw only at equal depth.
            // Otherwise preserve the original depth comparison (e.g. GREATER_EQUAL
            // for reverse-Z), merely disabling writes. Preserve stencil rules.
            if (originallyWroteDepth) dd.DepthFunc = D3D11_COMPARISON_EQUAL;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> newDepth;
            const HRESULT hr = device->CreateDepthStencilState(&dd, newDepth.GetAddressOf());
            if (FAILED(hr)) { skip("CreateDepthStencilState failed"); return; }
            g_overlayDepth = std::move(newDepth);
            g_overlayDepthSource = originalDepth;
            spdlog::info("LIQUID OVERLAY: depth state initialized (original wrote depth={})", originallyWroteDepth);
        }

        // Create and update one tiny dynamic buffer. Each tagged Jar03 receives
        // independent, persistent slopes; all other passes keep their PS b12.
        if (!g_sloshPerJarBuffer) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(JarSloshConstantData);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&desc, nullptr, g_sloshPerJarBuffer.GetAddressOf()))) {
                skip("couldn't create movement slosh constant buffer"); return;
            }
        }
        const auto slosh = AdvanceJarSlosh(pass->geometry);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(g_sloshPerJarBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            skip("couldn't update slosh buffer"); return;
        }
        std::memcpy(mapped.pData, &slosh, sizeof(slosh));
        ctx->Unmap(g_sloshPerJarBuffer.Get(), 0);

        // The game draw already completed. No original shader changes until here.
        ID3D11Buffer* worldBuffer = perGeometry.Get();
        ctx->PSSetConstantBuffers(13, 1, &worldBuffer);
        ID3D11Buffer* movementBuffer = g_sloshPerJarBuffer.Get();
        ctx->PSSetConstantBuffers(kSloshConstantSlot, 1, &movementBuffer);
        ctx->PSSetShader(g_gravityPlanePS.Get(), nullptr, 0);
        ctx->OMSetBlendState(g_overlayBlend.Get(), originalBlendFactor, originalSampleMask);
        ctx->OMSetDepthStencilState(g_overlayDepth.Get(), originalStencilRef);
        ctx->DrawIndexed(528, 0, 0); // observed: Jar03 DrawIndexed(528)

        // Restore EVERY state changed by the overlay before forwarding the
        // original Skyrim/Open Shaders RestoreGeometry callback.
        ctx->OMSetDepthStencilState(originalDepth.Get(), originalStencilRef);
        ctx->OMSetBlendState(originalBlend.Get(), originalBlendFactor, originalSampleMask);
        ctx->PSSetShader(originalPS.Get(), nullptr, 0);
        ID3D11Buffer* oldB13 = priorB13.Get();
        ctx->PSSetConstantBuffers(13, 1, &oldB13);
        ID3D11Buffer* oldSloshBuffer = priorSloshBuffer.Get();
        ctx->PSSetConstantBuffers(kSloshConstantSlot, 1, &oldSloshBuffer);
        const auto n = g_overlayAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= g_maxTraceLines) {
            spdlog::info("LIQUID OVERLAY: DRAW #{} Jar03 528 indices / 3 RTVs; original PS, blend, depth, b13 and b12 restored", n);
        }
    }

    void TraceLightingGeometrySetup(RE::BSShader* shader, RE::BSRenderPass* pass, std::uint32_t flags)
    {
        if (t_pixelSwap.active) {
            // Unexpected missing/mismatched RestoreGeometry: fail closed before
            // another lighting geometry setup. This is worth investigating.
            RestorePixelShader("next setup (unexpected; previous RestoreGeometry missing)");
        }
        if (g_originalSetupGeometry) {
            g_originalSetupGeometry(shader, pass, flags);
        }
        if (!g_firstPassSeen.exchange(true, std::memory_order_relaxed)) {
            spdlog::info("RENDER TRACE: first BSLightingShader::SetupGeometry call observed");
        }
        if (!IsMarkedJarPass(pass)) {
            return;
        }
        const auto seen = g_traceMatches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_traceRequested) {
            if (seen <= g_maxTraceLines) {
                spdlog::info("RENDER PASS MATCH #{}: Jar03 marker=1 geom={:p} pass={:p} passEnum=0x{:X} flags=0x{:X}",
                    seen, static_cast<const void*>(pass->geometry), static_cast<const void*>(pass),
                    pass->passEnum, flags);
            } else if (seen == g_maxTraceLines + 1) {
                spdlog::info("RENDER TRACE: log cap reached");
            }
        }
        TrySwapPixelShader(pass, flags);
    }

    void TraceLightingGeometryRestore(RE::BSShader* shader, RE::BSRenderPass* pass, std::uint32_t flags)
    {
        // Separate overlay AFTER the original jar draw, BEFORE renderer restore.
        DrawLiquidOverlay(pass, flags);
        if (t_pixelSwap.active) {
            RestorePixelShader(t_pixelSwap.pass == pass ? "paired RestoreGeometry" : "mismatched RestoreGeometry");
        }
        if (g_originalRestoreGeometry) {
            g_originalRestoreGeometry(shader, pass, flags);
        }
    }

    void InstallRenderTrace()
    {
        if (!(g_traceRequested || g_redRequested || g_gravityRequested || g_overlayRequested) || g_hooksInstalled) {
            return;
        }
        if (g_redRequested && !g_redDiagnosticPS) {
            spdlog::error("PIXEL TEST: diagnostic PS unavailable; continuing in trace-only mode");
            g_redRequested = false;
        }
        if (g_gravityRequested && !g_gravityPlanePS) {
            spdlog::error("PIXEL TEST: gravity PS unavailable; continuing in trace-only mode");
            g_gravityRequested = false;
        }
        if (g_overlayRequested && !g_gravityPlanePS) {
            spdlog::error("LIQUID OVERLAY: gravity PS unavailable; overlay disabled");
            g_overlayRequested = false;
        }
        REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSLightingShader[0] };
        // The paired RestoreGeometry hook handles both the old replacement
        // tests and the new opt-in second-pass overlay.
        if (g_redRequested || g_gravityRequested || g_overlayRequested) {
            const auto oldRestore = vtable.write_vfunc(0x07, &TraceLightingGeometryRestore);
            g_originalRestoreGeometry = reinterpret_cast<GeometryFn>(oldRestore);
            if (!g_originalRestoreGeometry) {
                spdlog::error("PIXEL TEST: cannot chain RestoreGeometry; pixel modes disabled");
                g_redRequested = false;
                g_gravityRequested = false;
                g_overlayRequested = false;
            }
        }
        const auto oldSetup = vtable.write_vfunc(0x06, &TraceLightingGeometrySetup);
        g_originalSetupGeometry = reinterpret_cast<GeometryFn>(oldSetup);
        g_hooksInstalled = g_originalSetupGeometry != nullptr;
        if (g_hooksInstalled) {
            spdlog::info("RENDER TRACE: installed SetupGeometry hook; previous={:p}",
                reinterpret_cast<const void*>(oldSetup));
            if (g_redRequested || g_gravityRequested || g_overlayRequested) {
                spdlog::info("RENDER TEST: paired RestoreGeometry hook installed; mode={}",
                    g_overlayRequested ? "second-pass liquid overlay" :
                    (g_gravityRequested ? "gravity replacement" : "solid-red replacement"));
            }
        } else {
            spdlog::error("RENDER TRACE: could not chain SetupGeometry");
            g_redRequested = false;
            g_gravityRequested = false;
            g_overlayRequested = false;
        }
    }

    void SetupLog()
    {
        auto logs = SKSE::log::log_directory();
        if (!logs) {
            SKSE::stl::report_and_fail("No SKSE log directory was provided");
        }
        const auto logPath = *logs / "FilledJarMarkerDetector.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        auto instance = std::make_shared<spdlog::logger>("FilledJarMarkerDetector", std::move(sink));
        spdlog::set_default_logger(std::move(instance));
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    }

    bool IsFilledJar(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return false;
        }
        auto* base = ref->GetBaseObject();
        if (!base) {
            return false;
        }
        const char* name = base->GetName();
        return name && std::string_view{ name }.find("Filled Jar") != std::string_view::npos;
    }

    void InspectJar(RE::TESObjectREFR* ref, const char* origin)
    {
        if (!IsFilledJar(ref)) {
            return;
        }
        const auto id = ref->GetFormID();
        auto* root = ref->Get3D();
        if (!root) {
            spdlog::info("CANDIDATE [{}]: ref {:08X}, but 3D is not loaded yet", origin, id);
            return;
        }

        bool sawTag = false;
        int tagCount = 0;
        RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* object) {
            if (!object) {
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            if (auto* tag = object->GetExtraData<RE::NiIntegerExtraData>(kMarkerName)) {
                ++tagCount;
                if (tag->value == 1) {
                    sawTag = true;
                    spdlog::info("MATCH [{}]: ref {:08X}, mesh '{}', FilledJarLiquid={}",
                        origin, id, object->name.c_str(), tag->value);
                    // The marker is the authorization; the extra Jar03 check ensures only
                    // the jar's glass (not its cork or collision objects) is modified.
                    const char* name = object->name.c_str();
                    if (name && std::string_view(name) == "Jar03") {
                        ApplyVisualTest(object, id, origin);
                    } else {
                        spdlog::warn("Marker is on a non-Jar03 object; visual change skipped: '{}'", name ? name : "<unnamed>");
                    }
                } else {
                    spdlog::warn("WRONG VALUE [{}]: ref {:08X}, mesh '{}', FilledJarLiquid={}",
                        origin, id, object->name.c_str(), tag->value);
                }
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });

        if (tagCount == 0) {
            spdlog::warn("NO MARKER [{}]: ref {:08X}; the loaded NIF may not be the tagged one", origin, id);
        } else if (sawTag) {
            if (auto* console = RE::ConsoleLog::GetSingleton()) {
                console->Print("[FilledJar] Marker found! See FilledJarMarkerDetector.log");
            }
        }
    }

    void ScanLoadedJars()
    {
        auto* world = RE::TES::GetSingleton();
        if (!world) {
            spdlog::warn("World not available for scan");
            return;
        }
        std::size_t candidates = 0;
        world->ForEachReference([&](RE::TESObjectREFR* ref) {
            if (IsFilledJar(ref)) {
                ++candidates;
                InspectJar(ref, "world scan");
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        spdlog::info("World scan finished: {} nearby Filled Jar references", candidates);
    }

    class AttachListener final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESCellAttachDetachEvent* event,
            RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
        {
            if (event && event->attached && event->reference) {
                // Queue to SKSE's game-thread task interface, rather than mutating
                // scene-graph state synchronously from an event callback.
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    auto ref = event->reference;  // keep NiPointer alive until the task executes
                    tasks->AddTask([ref]() { InspectJar(ref.get(), "cell attach"); });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    AttachListener g_attachListener;
    bool g_registered = false;

    void OnMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }
        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            ProbeShaderCompilation(); // game renderer is initialized by this stage
            InstallRenderTrace(); // optional; read-only hook, forwards original
            if (!g_registered) {
                if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
                    events->AddEventSink<RE::TESCellAttachDetachEvent>(&g_attachListener);
                    g_registered = true;
                    spdlog::info("Installed TESCellAttachDetachEvent marker detector");
                } else {
                    spdlog::error("Script event source was unavailable");
                }
            }
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask(ScanLoadedJars);
            }
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();
    g_mode = ReadVisualMode();
    ReadTraceSettings();
    spdlog::info("FilledJarVisualTest loaded; mode={} (Green=tint attempt, Hide=glass invisible, Off=diagnostics only)", ModeName(g_mode));
    spdlog::info("Searching for Jar03 NiIntegerExtraData FilledJarLiquid=1");
    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        spdlog::error("Could not register SKSE messaging listener");
        return false;
    }
    return true;
}
