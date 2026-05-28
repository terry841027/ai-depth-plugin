#pragma once
#include <FFGLSDK.h>
#include <onnxruntime_cxx_api.h>
// Fallback detection when not set by the build system (CMake always defines it).
// Actual #include <dml_provider_factory.h> lives in Plugin.cpp, after <Windows.h>.
#ifndef ORT_DML_AVAILABLE
#  if __has_include(<dml_provider_factory.h>)
#    define ORT_DML_AVAILABLE 1
#  else
#    define ORT_DML_AVAILABLE 0
#  endif
#endif
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

// ── Global inference controls ──────────────────────────────────────────────
// ── Z-depth controls ───────────────────────────────────────────────────────
// ── Sweep (works in both Z-depth and SL modes) ─────────────────────────────
// ── Scan-line toggle + sub-params ──────────────────────────────────────────
enum ParamIndex {
    PARAM_QUALITY   = 0,  // depth inference quality: low=省資源 high=精準即時
    PARAM_SMOOTH,         // temporal smoothing: low=跟手快 high=順滑
    PARAM_INVERT,         // flip near/far
    PARAM_NEAR,           // Z-depth range low
    PARAM_FAR,            // Z-depth range high
    PARAM_SWEEP,          // depth-slice sweep ON/OFF (global)
    PARAM_SWEEP_DIR,      // sweep direction: 0=far→near  1=near→far
    PARAM_SWEEP_POS,      // sweep position 0-1, connect BPM
    PARAM_SCANLINE,       // 0=Z-depth mode  1=scan-line mode
    PARAM_EFFECT,         // 0=h-lines  0.33=warped  0.66=circular  1=dots
    PARAM_DENSITY,        // line freq / dot grid size
    PARAM_WIDTH,          // line thickness / dot radius / sweep band
    PARAM_WARP,           // depth displacement (warped-lines only)
    PARAM_OFFSET,         // SL scroll position — connect BPM
    PARAM_BLEND,          // 0=effect on black  1=overlay on original video
    // ── Particle mode ──────────────────────────────────────────────────────────
    PARAM_PARTICLE,       // 0=off  1=particle-glow mode
    PARAM_P_DENSITY,      // grid density (more cells = finer particles)
    PARAM_P_SIZE,         // particle radius
    PARAM_P_GLOW,         // glow brightness
    PARAM_P_DRIFT,        // drift animation amount+speed
    PARAM_COUNT
};

class AIDepthMap : public CFFGLPlugin {
public:
    AIDepthMap();
    ~AIDepthMap();
    FFResult InitGL(const FFGLViewportStruct* vp) override;
    FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
    FFResult DeInitGL() override;
    FFResult SetFloatParameter(unsigned int index, float value) override;
    float    GetFloatParameter(unsigned int index) override;

private:
    // ── OpenGL resources ──────────────────────────────────
    ffglex::FFGLShader      m_scaleShader;
    ffglex::FFGLShader      m_vizShader;
    ffglex::FFGLScreenQuad  m_quad;
    GLuint m_scaleFBO = 0, m_scaleTex = 0, m_depthTex = 0;
    int    m_vpW = 1, m_vpH = 1;

    // ── PBO async readback (two alternating buffers) ──────
    GLuint m_pbo[2]       = {0, 0};
    bool   m_pboFilled[2] = {false, false};
    int    m_pboWrite     = 0;  // which PBO we write to this frame

    // ── ONNX Runtime ─────────────────────────────────────
    bool                     m_ortOk   = false;
    Ort::Env*                m_ortEnv  = nullptr;
    Ort::Session*            m_ortSess = nullptr;
    std::vector<std::string> m_inNames, m_outNames;
    std::vector<const char*> m_inCStr,  m_outCStr;

    // ── Frame counter ─────────────────────────────────────
    unsigned int m_frameCount = 0;

    // ── Async inference worker ────────────────────────────
    void                    workerFunc();
    std::thread             m_worker;
    std::atomic<bool>       m_workerStop{false};

    std::mutex              m_inMtx;
    std::condition_variable m_inCV;
    std::vector<uint8_t>    m_rgbaFor;
    std::atomic<bool>       m_frameAvail{false};

    std::mutex              m_outMtx;
    std::vector<float>      m_depthNew;
    std::vector<float>      m_depthSmooth;   // temporal smoothing buffer
    std::atomic<bool>       m_depthAvail{false};

    // ── Parameters ────────────────────────────────────────
    float m_quality  = 0.5f;
    float m_smooth   = 0.35f;
    int   m_invert   = 0;
    float m_near     = 0.0f;
    float m_far      = 1.0f;
    float m_sweep    = 0.0f;
    float m_sweepDir = 0.0f;
    float m_sweepPos = 0.0f;
    float m_scanLine = 0.0f;
    float m_effect   = 0.4f;
    float m_density  = 0.3f;
    float m_width    = 0.1f;
    float m_warp     = 0.3f;
    float m_offset   = 0.0f;
    float m_blend    = 0.5f;
    float m_particle  = 0.0f;
    float m_pDensity  = 0.5f;
    float m_pSize     = 0.5f;
    float m_pGlow     = 0.7f;
    float m_pDrift    = 0.3f;
    float m_time      = 0.0f;   // accumulated seconds (frame-count / 60)

    static constexpr int MODEL_H = 518;
    static constexpr int MODEL_W = 518;

    bool loadONNX();
};
