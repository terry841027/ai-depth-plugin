#include "Plugin.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
//  Plugin registration
// ─────────────────────────────────────────────────────────────────────────────
static CFFGLPluginInfo PluginInfo(
    PluginFactory< AIDepthMap >,
    "TRAI",
    "AI Depth Map",
    2, 1,
    1, 0,
    FF_EFFECT,
    "Real-time monocular depth map via Depth Anything V2 Small",
    "FleetView"
);

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: get the directory that contains this DLL
// ─────────────────────────────────────────────────────────────────────────────
static void* _addrAnchor() { return (void*)_addrAnchor; }

static std::wstring GetDllDir() {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)_addrAnchor, &hMod);
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(hMod, buf, MAX_PATH);
    std::wstring path(buf);
    auto sep = path.rfind(L'\\');
    return (sep != std::wstring::npos) ? path.substr(0, sep + 1) : L".\\";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shaders
// ─────────────────────────────────────────────────────────────────────────────
// Pass 1: blit + scale input texture into the 518×518 FBO
static const char* kScaleVert = R"glsl(
#version 410 core
uniform vec2 MaxUV;
layout(location=0) in vec4 vPosition;
layout(location=1) in vec2 vUV0;
out vec2 vUV;
void main() { gl_Position = vPosition; vUV = vUV0 * MaxUV; }
)glsl";

static const char* kScaleFrag = R"glsl(
#version 410 core
uniform sampler2D InputTexture;
in  vec2 vUV;
out vec4 fragColor;
void main() { fragColor = texture(InputTexture, vUV); }
)glsl";

// Pass 2: visualise the R32F depth texture
static const char* kVizVert = R"glsl(
#version 410 core
layout(location=0) in vec4 vPosition;
layout(location=1) in vec2 vUV0;
out vec2 vUV;
void main() { gl_Position = vPosition; vUV = vUV0; }
)glsl";

static const char* kVizFrag = R"glsl(
#version 410 core
uniform sampler2D DepthTex;
uniform int   Invert;
uniform float Contrast;    // 0-1 → gamma 0.1-3.0
uniform float FalseColor;  // >0.5 = rainbow jet

in  vec2 vUV;
out vec4 fragColor;

// Jet-style rainbow: blue(far) → cyan → green → yellow → red(near)
vec3 jet(float t) {
    return clamp(vec3(
        1.5 - abs(4.0 * t - 1.0),   // red channel
        1.5 - abs(4.0 * t - 2.0),   // green channel
        1.5 - abs(4.0 * t - 3.0)    // blue channel
    ), 0.0, 1.0);
}

void main() {
    float d = texture(DepthTex, vUV).r;
    if (Invert == 1) d = 1.0 - d;
    float gamma = Contrast * 2.9 + 0.1;
    d = pow(clamp(d, 0.0, 1.0), gamma);
    vec3 col = (FalseColor > 0.5) ? jet(d) : vec3(d);
    fragColor = vec4(col, 1.0);
}
)glsl";

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
AIDepthMap::AIDepthMap()
    : m_invert(0), m_contrast(0.5f), m_color(0.0f)
{
    SetMinInputs(1);
    SetMaxInputs(1);
    SetParamInfo(PARAM_INVERT,   "Invert",      FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(PARAM_CONTRAST, "Contrast",    FF_TYPE_STANDARD, 0.5f);
    SetParamInfo(PARAM_COLOR,    "False Color", FF_TYPE_STANDARD, 0.0f);
}

AIDepthMap::~AIDepthMap() {
    delete m_ortSess;
    delete m_ortEnv;
}

// ─────────────────────────────────────────────────────────────────────────────
//  loadONNX  – locate model next to the DLL, create session
// ─────────────────────────────────────────────────────────────────────────────
bool AIDepthMap::loadONNX() {
    try {
        m_ortEnv = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "AIDepth");

        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(4);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::wstring modelPath = GetDllDir() + L"depth_anything_v2_vits.onnx";
        m_ortSess = new Ort::Session(*m_ortEnv, modelPath.c_str(), opts);

        // Discover input/output names at runtime (works with any ONNX export)
        Ort::AllocatorWithDefaultOptions alloc;
        size_t nIn  = m_ortSess->GetInputCount();
        size_t nOut = m_ortSess->GetOutputCount();
        for (size_t i = 0; i < nIn;  i++) m_inNames .push_back(m_ortSess->GetInputNameAllocated (i, alloc).get());
        for (size_t i = 0; i < nOut; i++) m_outNames.push_back(m_ortSess->GetOutputNameAllocated(i, alloc).get());
        for (auto& s : m_inNames)  m_inCStr .push_back(s.c_str());
        for (auto& s : m_outNames) m_outCStr.push_back(s.c_str());

        // Pre-allocate CPU buffers
        m_rgba .resize(MODEL_H * MODEL_W * 4, 0);
        m_input.resize(3 * MODEL_H * MODEL_W, 0.f);
        m_depth.resize(MODEL_H * MODEL_W, 0.f);

        return true;
    }
    catch (...) {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  InitGL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AIDepthMap::InitGL(const FFGLViewportStruct* vp) {
    m_vpW = vp->width;
    m_vpH = vp->height;

    // Compile shaders
    if (!m_scaleShader.Compile(kScaleVert, kScaleFrag)) return FF_FAIL;
    if (!m_vizShader  .Compile(kVizVert,   kVizFrag))   return FF_FAIL;
    if (!m_quad.Initialise())                            return FF_FAIL;

    // Create 518×518 scale FBO  (RGBA8 colour attachment)
    glGenTextures(1, &m_scaleTex);
    glBindTexture(GL_TEXTURE_2D, m_scaleTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MODEL_W, MODEL_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_scaleFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scaleFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scaleTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Create 518×518 depth result texture (R32F)
    glGenTextures(1, &m_depthTex);
    glBindTexture(GL_TEXTURE_2D, m_depthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, MODEL_W, MODEL_H, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Load ONNX model (fail gracefully – show black if model missing)
    m_ortOk = loadONNX();

    return CFFGLPlugin::InitGL(vp);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DeInitGL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AIDepthMap::DeInitGL() {
    m_scaleShader.FreeGLResources();
    m_vizShader  .FreeGLResources();
    m_quad.Release();
    if (m_scaleFBO) { glDeleteFramebuffers(1, &m_scaleFBO); m_scaleFBO = 0; }
    if (m_scaleTex) { glDeleteTextures(1,    &m_scaleTex);  m_scaleTex = 0; }
    if (m_depthTex) { glDeleteTextures(1,    &m_depthTex);  m_depthTex = 0; }
    return FF_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessOpenGL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AIDepthMap::ProcessOpenGL(ProcessOpenGLStruct* pGL) {
    if (!pGL->numInputTextures || !pGL->inputTextures[0]) return FF_FAIL;
    FFGLTextureStruct& tex = *(pGL->inputTextures[0]);
    if (!tex.Handle) return FF_FAIL;

    float maxU = tex.HardwareWidth  ? (float)tex.Width  / tex.HardwareWidth  : 1.f;
    float maxV = tex.HardwareHeight ? (float)tex.Height / tex.HardwareHeight : 1.f;

    if (m_ortOk) {
        // ── Pass 1: scale input → 518×518 FBO ────────────────────────────
        GLint prevFBO = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

        glBindFramebuffer(GL_FRAMEBUFFER, m_scaleFBO);
        glViewport(0, 0, MODEL_W, MODEL_H);
        {
            ffglex::ScopedShaderBinding     sb(m_scaleShader.GetGLID());
            ffglex::ScopedSamplerActivation sa(0);
            ffglex::Scoped2DTextureBinding  tb(tex.Handle);
            m_scaleShader.Set("InputTexture", 0);
            m_scaleShader.Set("MaxUV", maxU, maxV);
            m_quad.Draw();
        }

        // Read 518×518 RGBA pixels (fast – only ~1 MB)
        glReadPixels(0, 0, MODEL_W, MODEL_H, GL_RGBA, GL_UNSIGNED_BYTE, m_rgba.data());

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
        glViewport(0, 0, m_vpW, m_vpH);

        // ── CPU: RGBA uint8 → float32 CHW, ImageNet normalise ────────────
        // OpenGL FBO y=0 is at bottom; model expects y=0 at top → flip Y
        static const float mean[3] = {0.485f, 0.456f, 0.406f};
        static const float stdv[3] = {0.229f, 0.224f, 0.225f};
        const int total = MODEL_H * MODEL_W;
        for (int y = 0; y < MODEL_H; y++) {
            int fy = MODEL_H - 1 - y; // flip
            for (int x = 0; x < MODEL_W; x++) {
                int src = (fy * MODEL_W + x) * 4;
                int dst = y * MODEL_W + x;
                m_input[0 * total + dst] = (m_rgba[src + 0] / 255.f - mean[0]) / stdv[0];
                m_input[1 * total + dst] = (m_rgba[src + 1] / 255.f - mean[1]) / stdv[1];
                m_input[2 * total + dst] = (m_rgba[src + 2] / 255.f - mean[2]) / stdv[2];
            }
        }

        // ── ONNX inference ────────────────────────────────────────────────
        try {
            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            int64_t inShape[] = {1, 3, MODEL_H, MODEL_W};
            auto inTensor = Ort::Value::CreateTensor<float>(
                memInfo, m_input.data(), m_input.size(), inShape, 4);

            auto outputs = m_ortSess->Run(
                Ort::RunOptions{nullptr},
                m_inCStr.data(),  &inTensor, 1,
                m_outCStr.data(), 1);

            float* raw  = outputs[0].GetTensorMutableData<float>();
            int    sz   = (int)outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

            // Min-max normalise per frame → [0, 1]
            float dMin = *std::min_element(raw, raw + sz);
            float dMax = *std::max_element(raw, raw + sz);
            float rng  = std::max(dMax - dMin, 1e-6f);
            for (int i = 0; i < sz; i++) m_depth[i] = (raw[i] - dMin) / rng;

            // Upload to GPU texture
            glBindTexture(GL_TEXTURE_2D, m_depthTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MODEL_W, MODEL_H,
                            GL_RED, GL_FLOAT, m_depth.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        catch (...) {
            // Inference error – keep showing last frame
        }
    }

    // ── Pass 2: visualise depth texture ──────────────────────────────────
    {
        ffglex::ScopedShaderBinding     sb(m_vizShader.GetGLID());
        ffglex::ScopedSamplerActivation sa(0);
        ffglex::Scoped2DTextureBinding  tb(m_depthTex);
        m_vizShader.Set("DepthTex",   0);
        m_vizShader.Set("Invert",     m_invert);
        m_vizShader.Set("Contrast",   m_contrast);
        m_vizShader.Set("FalseColor", m_color);
        m_quad.Draw();
    }

    return FF_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parameters
// ─────────────────────────────────────────────────────────────────────────────
FFResult AIDepthMap::SetFloatParameter(unsigned int idx, float val) {
    switch (idx) {
        case PARAM_INVERT:   m_invert   = (val > 0.5f) ? 1 : 0; break;
        case PARAM_CONTRAST: m_contrast = val;                   break;
        case PARAM_COLOR:    m_color    = val;                   break;
        default: return FF_FAIL;
    }
    return FF_SUCCESS;
}

float AIDepthMap::GetFloatParameter(unsigned int idx) {
    switch (idx) {
        case PARAM_INVERT:   return (float)m_invert;
        case PARAM_CONTRAST: return m_contrast;
        case PARAM_COLOR:    return m_color;
        default: return 0.f;
    }
}
