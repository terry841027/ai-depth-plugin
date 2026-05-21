#pragma once
#include <FFGLSDK.h>
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

enum ParamIndex {
    PARAM_PERF      = 0,  // 0=skip-4 (fast)  1=every frame (precise)
    PARAM_EFFECT,         // 0=h-lines  0.33=warped  0.66=circular  1=dots
    PARAM_DENSITY,        // line frequency / dot grid size
    PARAM_WIDTH,          // line thickness / dot radius / sweep band
    PARAM_WARP,           // depth displacement (warped-lines mode)
    PARAM_OFFSET,         // BPM bounce: line scroll or sweep position
    PARAM_SWEEP,          // >0.5 = depth-slice sweep ON
    PARAM_SWEEP_DIR,      // 0=far→near  1=near→far
    PARAM_BLEND,          // 0=effect on black  1=overlay on video
    PARAM_INVERT,         // flip near/far
    PARAM_NEAR,           // depth range low  (C4D OC style)
    PARAM_FAR,            // depth range high (C4D OC style)
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

    // ── ONNX Runtime ─────────────────────────────────────
    bool                     m_ortOk   = false;
    Ort::Env*                m_ortEnv  = nullptr;
    Ort::Session*            m_ortSess = nullptr;
    std::vector<std::string> m_inNames, m_outNames;
    std::vector<const char*> m_inCStr,  m_outCStr;

    // ── Frame counter (for skip logic) ────────────────────
    unsigned int m_frameCount = 0;

    // ── Async inference worker ────────────────────────────
    void                    workerFunc();
    std::thread             m_worker;
    std::atomic<bool>       m_workerStop{false};

    // Input  render→worker
    std::mutex              m_inMtx;
    std::condition_variable m_inCV;
    std::vector<uint8_t>    m_rgbaFor;        // RGBA copy for worker
    std::atomic<bool>       m_frameAvail{false};

    // Output  worker→render
    std::mutex              m_outMtx;
    std::vector<float>      m_depthNew;       // normalised depth from worker
    std::atomic<bool>       m_depthAvail{false};

    // GL readback buffer (render thread only)
    std::vector<uint8_t>    m_rgba;

    // ── Parameters ────────────────────────────────────────
    float m_perf     = 0.5f;
    float m_effect   = 0.4f;   // warped lines default
    float m_density  = 0.3f;
    float m_width    = 0.1f;
    float m_warp     = 0.3f;
    float m_offset   = 0.0f;
    float m_sweep    = 0.0f;
    float m_sweepDir = 0.0f;
    float m_blend    = 0.5f;
    int   m_invert   = 0;
    float m_near     = 0.0f;
    float m_far      = 1.0f;

    static constexpr int MODEL_H = 518;
    static constexpr int MODEL_W = 518;

    bool loadONNX();
};
