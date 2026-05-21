#pragma once
#include <FFGLSDK.h>
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>

enum ParamIndex {
    PARAM_INVERT = 0,   // flip near/far
    PARAM_CONTRAST,     // 0.5 = linear, <0.5 stretch darks, >0.5 stretch lights
    PARAM_COLOR,        // 0 = grayscale, >0.5 = jet false colour
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
    ffglex::FFGLShader      m_scaleShader; // blit input → 518×518 FBO
    ffglex::FFGLShader      m_vizShader;   // depth tex → colour output
    ffglex::FFGLScreenQuad  m_quad;

    GLuint m_scaleFBO = 0;  // off-screen 518×518 render target
    GLuint m_scaleTex = 0;  // colour attachment (RGBA8)
    GLuint m_depthTex = 0;  // depth result    (R32F)
    int    m_vpW = 1, m_vpH = 1;

    // ── ONNX Runtime ─────────────────────────────────────
    bool                     m_ortOk   = false;
    Ort::Env*                m_ortEnv  = nullptr;
    Ort::Session*            m_ortSess = nullptr;
    std::vector<std::string> m_inNames,  m_outNames;
    std::vector<const char*> m_inCStr,   m_outCStr;

    // ── CPU buffers ───────────────────────────────────────
    std::vector<uint8_t> m_rgba;  // MODEL_H × MODEL_W × 4
    std::vector<float>   m_input; // 1 × 3 × MODEL_H × MODEL_W
    std::vector<float>   m_depth; // MODEL_H × MODEL_W  (normalised 0–1)

    // ── Parameters ────────────────────────────────────────
    int   m_invert   = 0;
    float m_contrast = 0.5f;
    float m_color    = 0.0f;

    static constexpr int MODEL_H = 518;
    static constexpr int MODEL_W = 518;

    bool loadONNX();
};
