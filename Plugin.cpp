// Windows.h MUST be first — FFGLSDK.h/GLEW would otherwise include it with
// WIN32_LEAN_AND_MEAN, stripping COM/OLE types (MSG, LPMSG) that DML needs.
// NOMINMAX prevents Windows from defining min/max macros that break std::min/max.
#define NOMINMAX
#include <Windows.h>
#if ORT_DML_AVAILABLE
#  include <dml_provider_factory.h>   // safe: COM types already defined above
#endif
#include "Plugin.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Plugin registration
// ─────────────────────────────────────────────────────────────────────────────
static CFFGLPluginInfo PluginInfo(
    PluginFactory< AIDepthMap >,
    "TRAI",
    "AI Depth Map",
    2, 4,
    1, 0,
    FF_EFFECT,
    "Real-time monocular depth map via Depth Anything V2 Small",
    "FleetView"
);

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: locate this DLL's directory
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

// ── Pass-2: Z-depth map  OR  scan-line effects ─────────────────────────────
static const char* kVizVert = R"glsl(
#version 410 core
layout(location=0) in vec4 vPosition;
layout(location=1) in vec2 vUV0;
out vec2 vUV;
void main() { gl_Position = vPosition; vUV = vUV0; }
)glsl";

static const char* kVizFrag = R"glsl(
#version 410 core
uniform sampler2D DepthTex;   // unit 0 – R32F normalised depth
uniform sampler2D VideoTex;   // unit 1 – original input
uniform vec2  MaxUV;

uniform int   Invert;
uniform float DepthNear;      // Z-depth only: depth range low  (C4D OC style)
uniform float DepthFar;       // Z-depth only: depth range high (C4D OC style)
uniform float ScanLine;       // boolean: 0=Z-depth mode  1=scan-line mode

uniform float Effect;    // 0=h-lines  0.33=warped  0.66=circular  1=dots
uniform float Density;
uniform float Width;
uniform float Warp;
uniform float Offset;    // SL scroll position (BPM)
uniform float Sweep;     // >0.5 = depth-slice sweep ON
uniform float SweepDir;  // >0.5 = near→far  else far→near
uniform float SweepPos;  // sweep slice position 0-1 (BPM)
uniform float Blend;     // 0=lines on black  1=overlay on video

uniform float ParticleMode;
uniform float ParticleDensity;
uniform float ParticleSize;
uniform float ParticleGlow;
uniform float ParticleDrift;
uniform float Time;
uniform float Aspect;

in  vec2 vUV;
out vec4 fragColor;

vec2 hash2(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453123);
}

void main() {
    // Raw depth sample – flip Y (GL tex row-0=bottom, model row-0=top)
    float rawD = texture(DepthTex, vec2(vUV.x, 1.0 - vUV.y)).r;
    if (Invert == 1) rawD = 1.0 - rawD;

    // ════════════════════════════════════════════════════════════════════
    //  PARTICLE GLOW MODE
    // ════════════════════════════════════════════════════════════════════
    if (ParticleMode > 0.5) {
        // ── DEBUG: red=edge strength, green=depth value ──
        float d = texture(DepthTex, vec2(vUV.x, 1.0 - vUV.y)).r;
        if (Invert == 1) d = 1.0 - d;
        float s  = 0.005;
        float dR = texture(DepthTex, vec2(clamp(vUV.x+s,0.001,0.999), 1.0-vUV.y)).r;
        float dL = texture(DepthTex, vec2(clamp(vUV.x-s,0.001,0.999), 1.0-vUV.y)).r;
        float dU = texture(DepthTex, vec2(vUV.x, 1.0-clamp(vUV.y+s,0.001,0.999))).r;
        float dD = texture(DepthTex, vec2(vUV.x, 1.0-clamp(vUV.y-s,0.001,0.999))).r;
        float edge = length(vec2(dR - dL, dU - dD)) * 4.0;
        // Red = edge*5 (amplified), Green = depth, Blue = 0
        fragColor = vec4(clamp(edge * 5.0, 0.0, 1.0), d, 0.0, 1.0);
        return;
    }

    // ════════════════════════════════════════════════════════════════════
    //  Z-DEPTH MODE  –  Near/Far remapping applies HERE ONLY
    // ════════════════════════════════════════════════════════════════════
    if (ScanLine < 0.5) {
        float range = max(DepthFar - DepthNear, 0.001);
        float d = clamp((rawD - DepthNear) / range, 0.0, 1.0);

        // Depth-slice sweep overlay (dual-band for seamless BPM loop)
        if (Sweep > 0.5) {
            float sp       = fract(SweepPos);
            float pos1     = (SweepDir > 0.5) ? (1.0 - sp) : sp;
            float pos2     = pos1 - 1.0;
            float sweepHalf = max(Width, 0.005);
            float band1 = 1.0 - smoothstep(sweepHalf * 0.6, sweepHalf, abs(d - pos1));
            float band2 = 1.0 - smoothstep(sweepHalf * 0.6, sweepHalf, abs(d - pos2));
            float band  = max(band1, band2);
            float lit   = mix(d, 1.0, band);
            fragColor = vec4(lit, lit, lit, 1.0);
        } else {
            fragColor = vec4(d, d, d, 1.0);
        }
        return;
    }

    // ════════════════════════════════════════════════════════════════════
    //  SCAN-LINE MODE  –  uses raw depth, Near/Far not applied
    // ════════════════════════════════════════════════════════════════════
    float d    = rawD;
    vec4 video = texture(VideoTex, vUV * MaxUV);
    vec3 bg    = mix(vec3(0.0), video.rgb, Blend);

    float effectMask  = 0.0;
    vec3  effectColor = vec3(1.0);
    float sweepDepth  = d;

    if (Effect < 0.85) {
        // Lines family
        float dens = Density * 50.0 + 2.0;
        float lc;
        if (Effect < 0.33) {
            lc = fract(vUV.y * dens + Offset);
        } else if (Effect < 0.66) {
            lc = fract(vUV.y * dens + d * Warp * dens + Offset);
        } else {
            lc = fract(d * dens + Offset);
        }
        float lineW = max(Width * 0.5, 0.001);
        effectMask  = 1.0 - smoothstep(lineW * 0.7, lineW, min(lc, 1.0 - lc));

    } else {
        // LED dot matrix
        float gridN  = Density * 60.0 + 4.0;
        vec2  cell   = floor(vUV * gridN);
        vec2  cellUV = fract(vUV * gridN);
        vec2  ctr    = (cell + 0.5) / gridN;

        float dc = texture(DepthTex, vec2(ctr.x, 1.0 - ctr.y)).r;
        if (Invert == 1) dc = 1.0 - dc;
        sweepDepth = dc;

        float dotR  = max(Width * 0.5, 0.02);
        effectMask  = 1.0 - smoothstep(dotR * 0.8, dotR, length(cellUV - 0.5));
        effectColor = texture(VideoTex, ctr * MaxUV).rgb;
    }

    // Depth-slice sweep (dual-band for seamless BPM loop)
    if (Sweep > 0.5) {
        float sp       = fract(SweepPos);
        float pos1     = (SweepDir > 0.5) ? (1.0 - sp) : sp;
        float pos2     = pos1 - 1.0;   // wrapped copy
        float sweepHalf = max(Width, 0.005);
        float band1 = 1.0 - smoothstep(sweepHalf * 0.6, sweepHalf, abs(sweepDepth - pos1));
        float band2 = 1.0 - smoothstep(sweepHalf * 0.6, sweepHalf, abs(sweepDepth - pos2));
        effectMask *= 1.0 - max(band1, band2);
    }

    fragColor = vec4(mix(bg, effectColor, effectMask), 1.0);
}
)glsl";

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
AIDepthMap::AIDepthMap()
{
    SetMinInputs(1);
    SetMaxInputs(1);
    // ── Inference controls ────────────────────────────────
    SetParamInfo(PARAM_QUALITY,   "Quality",      FF_TYPE_STANDARD, 0.5f);
    SetParamInfo(PARAM_SMOOTH,    "Smooth",       FF_TYPE_STANDARD, 0.35f);
    // ── Z-depth controls ──────────────────────────────────
    SetParamInfo(PARAM_INVERT,    "Invert",       FF_TYPE_BOOLEAN,  0.0f);
    SetParamInfo(PARAM_NEAR,      "Depth Near",   FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(PARAM_FAR,       "Depth Far",    FF_TYPE_STANDARD, 1.0f);
    // ── Sweep (global: works in Z-depth and SL mode) ──────
    SetParamInfo(PARAM_SWEEP,     "Sweep",        FF_TYPE_BOOLEAN,  0.0f);
    SetParamInfo(PARAM_SWEEP_DIR, "Sweep Dir",    FF_TYPE_BOOLEAN,  0.0f);
    SetParamInfo(PARAM_SWEEP_POS, "Sweep Pos",    FF_TYPE_STANDARD, 0.0f);
    // ── Scan-line mode ────────────────────────────────────
    SetParamInfo(PARAM_SCANLINE,  "Scan Line",    FF_TYPE_BOOLEAN,  0.0f);
    SetParamInfo(PARAM_EFFECT,    "SL Effect",    FF_TYPE_STANDARD, 0.4f);
    SetParamInfo(PARAM_DENSITY,   "SL Density",   FF_TYPE_STANDARD, 0.3f);
    SetParamInfo(PARAM_WIDTH,     "SL Width",     FF_TYPE_STANDARD, 0.1f);
    SetParamInfo(PARAM_WARP,      "SL Warp",      FF_TYPE_STANDARD, 0.3f);
    SetParamInfo(PARAM_OFFSET,    "SL Offset",    FF_TYPE_STANDARD, 0.0f);
    SetParamInfo(PARAM_BLEND,     "SL Blend Vid", FF_TYPE_STANDARD, 0.5f);
    // ── Particle mode ─────────────────────────────────────────────────
    SetParamInfo(PARAM_PARTICLE,  "Particle",     FF_TYPE_BOOLEAN,  0.0f);
    SetParamInfo(PARAM_P_DENSITY, "P Density",    FF_TYPE_STANDARD, 0.5f);
    SetParamInfo(PARAM_P_SIZE,    "P Size",       FF_TYPE_STANDARD, 0.5f);
    SetParamInfo(PARAM_P_GLOW,    "P Glow",       FF_TYPE_STANDARD, 0.7f);
    SetParamInfo(PARAM_P_DRIFT,   "P Drift",      FF_TYPE_STANDARD, 0.3f);
}

AIDepthMap::~AIDepthMap() {
    if (m_worker.joinable()) {
        m_workerStop = true;
        m_inCV.notify_all();
        m_worker.join();
    }
    delete m_ortSess;
    delete m_ortEnv;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Background inference worker  (never runs on render thread)
// ─────────────────────────────────────────────────────────────────────────────
void AIDepthMap::workerFunc() {
    static const float mean[3] = {0.485f, 0.456f, 0.406f};
    static const float stdv[3] = {0.229f, 0.224f, 0.225f};
    const int total = MODEL_H * MODEL_W;

    std::vector<uint8_t> localRgba(total * 4);
    std::vector<float>   localInput(3 * total);
    std::vector<float>   localDepth(total);

    while (true) {
        {
            std::unique_lock<std::mutex> lk(m_inMtx);
            m_inCV.wait(lk, [this] {
                return m_frameAvail.load() || m_workerStop.load();
            });
            if (m_workerStop) break;
            localRgba = m_rgbaFor;
            m_frameAvail = false;
        }

        // RGBA uint8 → float32 CHW, ImageNet normalise, flip Y
        for (int y = 0; y < MODEL_H; ++y) {
            int fy = MODEL_H - 1 - y;
            for (int x = 0; x < MODEL_W; ++x) {
                int src = (fy * MODEL_W + x) * 4;
                int dst = y  * MODEL_W + x;
                localInput[0*total+dst] = (localRgba[src+0]/255.f - mean[0]) / stdv[0];
                localInput[1*total+dst] = (localRgba[src+1]/255.f - mean[1]) / stdv[1];
                localInput[2*total+dst] = (localRgba[src+2]/255.f - mean[2]) / stdv[2];
            }
        }

        try {
            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            int64_t shape[] = {1, 3, MODEL_H, MODEL_W};
            auto inTensor = Ort::Value::CreateTensor<float>(
                memInfo, localInput.data(), localInput.size(), shape, 4);

            auto outputs = m_ortSess->Run(
                Ort::RunOptions{nullptr},
                m_inCStr.data(), &inTensor, 1,
                m_outCStr.data(), 1);

            float* raw = outputs[0].GetTensorMutableData<float>();
            int    sz  = (int)outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

            float dMin = *std::min_element(raw, raw + sz);
            float dMax = *std::max_element(raw, raw + sz);
            float rng  = std::max(dMax - dMin, 1e-6f);
            for (int i = 0; i < sz; ++i)
                localDepth[i] = (raw[i] - dMin) / rng;

            { std::lock_guard<std::mutex> lk(m_outMtx); m_depthNew = localDepth; }
            m_depthAvail = true;
        }
        catch (...) {}
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  loadONNX
// ─────────────────────────────────────────────────────────────────────────────
bool AIDepthMap::loadONNX() {
    try {
        m_ortEnv = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "AIDepth");

        Ort::SessionOptions opts;
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Try DirectML (GPU) first, fall back to CPU on failure
#if ORT_DML_AVAILABLE
        bool dmlOk = false;
        try {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(opts, 0));
            dmlOk = true;
        } catch (...) {}
        if (!dmlOk)
            opts.SetIntraOpNumThreads(2);   // CPU fallback
#else
        opts.SetIntraOpNumThreads(2);       // CPU-only build
#endif

        std::wstring modelPath = GetDllDir() + L"depth_anything_v2_vits.onnx";
        m_ortSess = new Ort::Session(*m_ortEnv, modelPath.c_str(), opts);

        Ort::AllocatorWithDefaultOptions alloc;
        size_t nIn  = m_ortSess->GetInputCount();
        size_t nOut = m_ortSess->GetOutputCount();
        for (size_t i = 0; i < nIn;  i++) m_inNames .push_back(m_ortSess->GetInputNameAllocated (i, alloc).get());
        for (size_t i = 0; i < nOut; i++) m_outNames.push_back(m_ortSess->GetOutputNameAllocated(i, alloc).get());
        for (auto& s : m_inNames)  m_inCStr .push_back(s.c_str());
        for (auto& s : m_outNames) m_outCStr.push_back(s.c_str());

        const int total = MODEL_H * MODEL_W;
        m_rgbaFor    .resize(total * 4, 0);
        m_depthNew   .resize(total, 0.5f);
        m_depthSmooth.resize(total, 0.5f);

        m_worker = std::thread(&AIDepthMap::workerFunc, this);
        return true;
    }
    catch (...) { return false; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  InitGL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AIDepthMap::InitGL(const FFGLViewportStruct* vp) {
    m_vpW = vp->width;
    m_vpH = vp->height;

    if (!m_scaleShader.Compile(kScaleVert, kScaleFrag)) return FF_FAIL;
    if (!m_vizShader  .Compile(kVizVert,   kVizFrag))   return FF_FAIL;
    if (!m_quad.Initialise())                            return FF_FAIL;

    // 518×518 scale FBO (RGBA8)
    glGenTextures(1, &m_scaleTex);
    glBindTexture(GL_TEXTURE_2D, m_scaleTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MODEL_W, MODEL_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &m_scaleFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scaleFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_scaleTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 518×518 depth result texture (R32F), init to 0.5
    std::vector<float> initDepth(MODEL_H * MODEL_W, 0.5f);
    glGenTextures(1, &m_depthTex);
    glBindTexture(GL_TEXTURE_2D, m_depthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, MODEL_W, MODEL_H, 0,
                 GL_RED, GL_FLOAT, initDepth.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Two PBOs for async pixel readback (GL_STREAM_READ = hint for GPU→CPU copy)
    const GLsizeiptr pboSize = MODEL_W * MODEL_H * 4;
    glGenBuffers(2, m_pbo);
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, pboSize, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

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
    if (m_pbo[0]) { glDeleteBuffers(2, m_pbo); m_pbo[0] = m_pbo[1] = 0; }
    if (m_scaleFBO) { glDeleteFramebuffers(1, &m_scaleFBO); m_scaleFBO = 0; }
    if (m_scaleTex) { glDeleteTextures(1,    &m_scaleTex);  m_scaleTex = 0; }
    if (m_depthTex) { glDeleteTextures(1,    &m_depthTex);  m_depthTex = 0; }
    return FF_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessOpenGL  – render thread, never blocks on ONNX
// ─────────────────────────────────────────────────────────────────────────────
FFResult AIDepthMap::ProcessOpenGL(ProcessOpenGLStruct* pGL) {
    if (!pGL->numInputTextures || !pGL->inputTextures[0]) return FF_FAIL;
    FFGLTextureStruct& tex = *(pGL->inputTextures[0]);
    if (!tex.Handle) return FF_FAIL;

    float maxU = tex.HardwareWidth  ? (float)tex.Width  / tex.HardwareWidth  : 1.f;
    float maxV = tex.HardwareHeight ? (float)tex.Height / tex.HardwareHeight : 1.f;

    if (m_ortOk) {
        // ── 1. Upload new depth result if worker just finished ─────────────
        if (m_depthAvail.exchange(false)) {
            std::lock_guard<std::mutex> lk(m_outMtx);
            // Temporal smoothing: EMA between previous and new depth map
            const float alpha = std::max(0.01f, std::min(1.0f, m_smooth));
            for (int i = 0; i < (int)m_depthSmooth.size(); ++i)
                m_depthSmooth[i] = m_depthSmooth[i] * (1.0f - alpha) + m_depthNew[i] * alpha;
            glBindTexture(GL_TEXTURE_2D, m_depthTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MODEL_W, MODEL_H,
                            GL_RED, GL_FLOAT, m_depthSmooth.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        // ── 2. Decide whether to submit this frame ─────────────────────────
        int  skip     = (m_quality < 0.33f) ? 4 : (m_quality < 0.66f) ? 2 : 1;
        bool doSubmit = ((m_frameCount % skip) == 0) && !m_frameAvail.load();
        m_frameCount++;

        if (doSubmit) {
            // ── Pass 1: scale input → 518×518 FBO ─────────────────────────
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

            // ── Async PBO readback (non-blocking) ─────────────────────────
            // Start writing this frame's pixels into m_pbo[m_pboWrite]
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_pboWrite]);
            glReadPixels(0, 0, MODEL_W, MODEL_H, GL_RGBA, GL_UNSIGNED_BYTE,
                         nullptr);  // nullptr = write into bound PBO asynchronously
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            m_pboFilled[m_pboWrite] = true;

            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
            glViewport(0, 0, m_vpW, m_vpH);

            // Map the OTHER PBO (filled last submit) – GPU copy should be done
            int readIdx = 1 - m_pboWrite;
            if (m_pboFilled[readIdx]) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readIdx]);
                const uint8_t* ptr =
                    (const uint8_t*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
                if (ptr) {
                    {
                        std::lock_guard<std::mutex> lk(m_inMtx);
                        std::memcpy(m_rgbaFor.data(), ptr, MODEL_W * MODEL_H * 4);
                        m_frameAvail = true;
                    }
                    m_inCV.notify_one();
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                }
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }

            m_pboWrite = 1 - m_pboWrite;
        }
    }

    // ── Pass 2: visualise ─────────────────────────────────────────────────
    {
        ffglex::ScopedShaderBinding sb(m_vizShader.GetGLID());

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex.Handle);
        glActiveTexture(GL_TEXTURE0);

        m_vizShader.Set("DepthTex",  0);
        m_vizShader.Set("VideoTex",  1);
        m_vizShader.Set("MaxUV",     maxU, maxV);
        m_vizShader.Set("Invert",    m_invert);
        m_vizShader.Set("DepthNear", m_near);
        m_vizShader.Set("DepthFar",  m_far);
        m_vizShader.Set("ScanLine",  m_scanLine);
        m_vizShader.Set("Effect",    m_effect);
        m_vizShader.Set("Density",   m_density);
        m_vizShader.Set("Width",     m_width);
        m_vizShader.Set("Warp",      m_warp);
        m_vizShader.Set("Offset",    m_offset);
        m_vizShader.Set("Sweep",     m_sweep);
        m_vizShader.Set("SweepDir",  m_sweepDir);
        m_vizShader.Set("SweepPos",  m_sweepPos);
        m_vizShader.Set("Blend",     m_blend);
        m_time += 1.0f / 60.0f;
        m_vizShader.Set("ParticleMode",    m_particle);
        m_vizShader.Set("ParticleDensity", m_pDensity);
        m_vizShader.Set("ParticleSize",    m_pSize);
        m_vizShader.Set("ParticleGlow",    m_pGlow);
        m_vizShader.Set("ParticleDrift",   m_pDrift);
        m_vizShader.Set("Time",            m_time);
        m_vizShader.Set("Aspect",          (float)m_vpW / (float)m_vpH);
        m_quad.Draw();

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    return FF_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parameters
// ─────────────────────────────────────────────────────────────────────────────
FFResult AIDepthMap::SetFloatParameter(unsigned int idx, float val) {
    switch (idx) {
        case PARAM_QUALITY:   m_quality  = val;                   break;
        case PARAM_SMOOTH:    m_smooth   = val;                   break;
        case PARAM_INVERT:    m_invert   = (val > 0.5f) ? 1 : 0; break;
        case PARAM_NEAR:      m_near     = val;                   break;
        case PARAM_FAR:       m_far      = val;                   break;
        case PARAM_SWEEP:     m_sweep    = val;                   break;
        case PARAM_SWEEP_DIR: m_sweepDir = val;                   break;
        case PARAM_SWEEP_POS: m_sweepPos = val;                   break;
        case PARAM_SCANLINE:  m_scanLine = val;                   break;
        case PARAM_EFFECT:    m_effect   = val;                   break;
        case PARAM_DENSITY:   m_density  = val;                   break;
        case PARAM_WIDTH:     m_width    = val;                   break;
        case PARAM_WARP:      m_warp     = val;                   break;
        case PARAM_OFFSET:    m_offset   = val;                   break;
        case PARAM_BLEND:     m_blend    = val;                   break;
        case PARAM_PARTICLE:  m_particle = val;                   break;
        case PARAM_P_DENSITY: m_pDensity = val;                   break;
        case PARAM_P_SIZE:    m_pSize    = val;                   break;
        case PARAM_P_GLOW:    m_pGlow    = val;                   break;
        case PARAM_P_DRIFT:   m_pDrift   = val;                   break;
        default: return FF_FAIL;
    }
    return FF_SUCCESS;
}

float AIDepthMap::GetFloatParameter(unsigned int idx) {
    switch (idx) {
        case PARAM_QUALITY:   return m_quality;
        case PARAM_SMOOTH:    return m_smooth;
        case PARAM_INVERT:    return (float)m_invert;
        case PARAM_NEAR:      return m_near;
        case PARAM_FAR:       return m_far;
        case PARAM_SWEEP:     return m_sweep;
        case PARAM_SWEEP_DIR: return m_sweepDir;
        case PARAM_SWEEP_POS: return m_sweepPos;
        case PARAM_SCANLINE:  return m_scanLine;
        case PARAM_EFFECT:    return m_effect;
        case PARAM_DENSITY:   return m_density;
        case PARAM_WIDTH:     return m_width;
        case PARAM_WARP:      return m_warp;
        case PARAM_OFFSET:    return m_offset;
        case PARAM_BLEND:     return m_blend;
        case PARAM_PARTICLE:  return m_particle;
        case PARAM_P_DENSITY: return m_pDensity;
        case PARAM_P_SIZE:    return m_pSize;
        case PARAM_P_GLOW:    return m_pGlow;
        case PARAM_P_DRIFT:   return m_pDrift;
        default: return 0.f;
    }
}
