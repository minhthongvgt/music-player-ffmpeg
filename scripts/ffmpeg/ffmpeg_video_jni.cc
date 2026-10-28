#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

#define LOG_TAG "ffmpeg_video_jni"
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define VIDEO_FUNC(RETURN_TYPE, NAME, ...)                           \
  extern "C" JNIEXPORT RETURN_TYPE JNICALL                           \
      Java_androidx_media3_decoder_ffmpeg_FfmpegVideoDecoder_##NAME( \
          JNIEnv* env, jobject thiz, ##__VA_ARGS__)

static const int kApiVersion = 1;
static const int kResultError = -1;
static const int kResultNoFrame = 0;
static const int kResultFrame = 1;
static const int kResultSkipped = 1;
static const int kColorspaceUnknown = 0;
static const int kColorspaceBT601 = 1;
static const int kColorspaceBT709 = 2;
static const int kColorspaceBT2020 = 3;
static const jlong kFrameInfoFullRange = 1;
static const int kMaxAspectTerm = 0xFFFF;
static const GLuint kPositionAttribute = 0;
static const GLuint kLumaAttribute = 1;
static const GLuint kChromaAttribute = 2;

static const char kVertexShader[] =
    "attribute vec2 aPosition;\n"
    "attribute vec2 aLumaCoord;\n"
    "attribute vec2 aChromaCoord;\n"
    "varying vec2 vLumaCoord;\n"
    "varying vec2 vChromaCoord;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "  vLumaCoord = aLumaCoord;\n"
    "  vChromaCoord = aChromaCoord;\n"
    "}\n";

static const char kFragmentShader[] =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 vLumaCoord;\n"
    "varying vec2 vChromaCoord;\n"
    "uniform sampler2D uTextureY;\n"
    "uniform sampler2D uTextureU;\n"
    "uniform sampler2D uTextureV;\n"
    "uniform mat3 uMatrix;\n"
    "uniform vec3 uOffset;\n"
    "void main() {\n"
    "  vec3 yuv = vec3(texture2D(uTextureY, vLumaCoord).r,\n"
    "                  texture2D(uTextureU, vChromaCoord).r,\n"
    "                  texture2D(uTextureV, vChromaCoord).r) - uOffset;\n"
    "  gl_FragColor = vec4(clamp(uMatrix * yuv, 0.0, 1.0), 1.0);\n"
    "}\n";

struct GlRenderer {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  bool surfaceConfigured = false;
  GLuint program = 0;
  GLuint textures[3] = {0, 0, 0};
  int textureWidths[3] = {0, 0, 0};
  int textureHeights[3] = {0, 0, 0};
  GLint matrixLocation = -1;
  GLint offsetLocation = -1;
  GLint maxTextureSize = 0;
};

struct VideoContext {
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  bool hasFrame = false;
  bool draining = false;
  ANativeWindow* window = nullptr;
  jobject surface = nullptr;
  GlRenderer gl;
};

static jmethodID initForYuvFrameMethod = nullptr;
static jfieldID dataField = nullptr;
static jfieldID widthField = nullptr;
static jfieldID heightField = nullptr;
static jfieldID yuvStridesField = nullptr;
static jfieldID colorspaceField = nullptr;
static jfieldID decoderPrivateField = nullptr;

static bool ensureFields(JNIEnv* env) {
  if (initForYuvFrameMethod && dataField && widthField && heightField && yuvStridesField &&
      colorspaceField && decoderPrivateField) {
    return true;
  }
  jclass cls = env->FindClass("androidx/media3/decoder/VideoDecoderOutputBuffer");
  if (!cls) {
    return false;
  }
  initForYuvFrameMethod = env->GetMethodID(cls, "initForYuvFrame", "(IIIII)Z");
  dataField = env->GetFieldID(cls, "data", "Ljava/nio/ByteBuffer;");
  widthField = env->GetFieldID(cls, "width", "I");
  heightField = env->GetFieldID(cls, "height", "I");
  yuvStridesField = env->GetFieldID(cls, "yuvStrides", "[I");
  colorspaceField = env->GetFieldID(cls, "colorspace", "I");
  decoderPrivateField = env->GetFieldID(cls, "decoderPrivate", "J");
  env->DeleteLocalRef(cls);
  return initForYuvFrameMethod && dataField && widthField && heightField && yuvStridesField &&
         colorspaceField && decoderPrivateField;
}

static void logError(const char* where, int error) {
  char buffer[256];
  av_strerror(error, buffer, sizeof(buffer));
  LOGE("%s failed: %s", where, buffer);
}

static void destroyEglSurface(GlRenderer* gl) {
  if (gl->surface == EGL_NO_SURFACE) {
    return;
  }
  if (eglGetCurrentSurface(EGL_DRAW) == gl->surface || eglGetCurrentSurface(EGL_READ) == gl->surface) {
    eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  eglDestroySurface(gl->display, gl->surface);
  gl->surface = EGL_NO_SURFACE;
  gl->surfaceConfigured = false;
}

static void releaseGl(GlRenderer* gl) {
  destroyEglSurface(gl);
  if (gl->context != EGL_NO_CONTEXT) {
    if (eglGetCurrentContext() == gl->context) {
      eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    eglDestroyContext(gl->display, gl->context);
  }
  *gl = GlRenderer();
}

static void releaseWindow(JNIEnv* env, VideoContext* ctx) {
  destroyEglSurface(&ctx->gl);
  if (ctx->window) {
    ANativeWindow_release(ctx->window);
    ctx->window = nullptr;
  }
  if (ctx->surface) {
    env->DeleteGlobalRef(ctx->surface);
    ctx->surface = nullptr;
  }
}

static void releaseContext(JNIEnv* env, VideoContext* ctx) {
  if (!ctx) {
    return;
  }
  releaseWindow(env, ctx);
  releaseGl(&ctx->gl);
  if (ctx->frame) {
    av_frame_free(&ctx->frame);
  }
  if (ctx->packet) {
    av_packet_free(&ctx->packet);
  }
  if (ctx->codec) {
    avcodec_free_context(&ctx->codec);
  }
  delete ctx;
}

static int receiveFrame(VideoContext* ctx) {
  int result = avcodec_receive_frame(ctx->codec, ctx->frame);
  if (result == 0) {
    ctx->hasFrame = true;
    return kResultFrame;
  }
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return kResultNoFrame;
  }
  logError("avcodec_receive_frame", result);
  return result == AVERROR_INVALIDDATA ? kResultNoFrame : kResultError;
}

static int drainFrame(VideoContext* ctx) {
  if (!ctx->draining) {
    int sent = avcodec_send_packet(ctx->codec, nullptr);
    if (sent < 0 && sent != AVERROR_EOF) {
      logError("avcodec_send_packet", sent);
      return kResultNoFrame;
    }
    ctx->draining = true;
  }
  while (true) {
    int result = avcodec_receive_frame(ctx->codec, ctx->frame);
    if (result == 0) {
      ctx->hasFrame = true;
      return kResultFrame;
    }
    if (result == AVERROR_INVALIDDATA) {
      continue;
    }
    if (result != AVERROR_EOF && result != AVERROR(EAGAIN)) {
      logError("avcodec_receive_frame", result);
    }
    return kResultNoFrame;
  }
}

static int colorspaceOf(const AVFrame* frame) {
  switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
      return kColorspaceBT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      return kColorspaceBT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      return kColorspaceBT2020;
    default:
      return kColorspaceUnknown;
  }
}

static bool isFullRange(const AVFrame* frame) {
  if (frame->color_range == AVCOL_RANGE_JPEG) {
    return true;
  }
  switch (frame->format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P:
      return true;
    default:
      return false;
  }
}

static jlong frameInfoOf(const AVFrame* frame) {
  int num = 0;
  int den = 0;
  if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
    av_reduce(&num, &den, frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den, kMaxAspectTerm);
  }
  jlong info = ((jlong)(num & kMaxAspectTerm) << 32) | ((jlong)(den & kMaxAspectTerm) << 16);
  return isFullRange(frame) ? info | kFrameInfoFullRange : info;
}

static inline uint8_t readSample(const AVFrame* frame, const AVComponentDescriptor& comp, int x, int y) {
  const uint8_t* p = frame->data[comp.plane] + (ptrdiff_t)y * frame->linesize[comp.plane] +
                     (ptrdiff_t)x * comp.step + comp.offset;
  if (comp.depth <= 8) {
    return (uint8_t)((p[0] >> comp.shift) & ((1 << comp.depth) - 1));
  }
  int value = ((p[0] | (p[1] << 8)) >> comp.shift) & ((1 << comp.depth) - 1);
  const int shift = comp.depth - 8;
  return (uint8_t)std::min(255, (value + (1 << (shift - 1))) >> shift);
}

static void copyPlane(const AVFrame* frame, const AVComponentDescriptor& comp, int srcWidth, int srcHeight,
                      int scale, int log2W, int log2H, uint8_t* dst, int dstStride, int dstWidth,
                      int dstHeight) {
  bool direct = comp.depth == 8 && comp.step == 1 && comp.shift == 0 && comp.offset == 0 &&
                (scale >> log2W) == 1 && (scale >> log2H) == 1 && (1 << log2W) == scale &&
                (1 << log2H) == scale;
  for (int y = 0; y < dstHeight; y++) {
    uint8_t* row = dst + (ptrdiff_t)y * dstStride;
    if (direct) {
      int sy = std::min(y, srcHeight - 1);
      memcpy(row, frame->data[comp.plane] + (ptrdiff_t)sy * frame->linesize[comp.plane],
             std::min(dstWidth, srcWidth));
      continue;
    }
    int sy = std::min((y * scale) >> log2H, srcHeight - 1);
    for (int x = 0; x < dstWidth; x++) {
      int sx = std::min((x * scale) >> log2W, srcWidth - 1);
      row[x] = readSample(frame, comp, sx, sy);
    }
  }
}

static GLuint compileShader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  if (!shader) {
    return 0;
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    char log[512];
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    LOGE("Shader compile failed: %s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static bool createProgram(GlRenderer* gl) {
  GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader);
  GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vertex || !fragment) {
    if (vertex) {
      glDeleteShader(vertex);
    }
    if (fragment) {
      glDeleteShader(fragment);
    }
    return false;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, kPositionAttribute, "aPosition");
  glBindAttribLocation(program, kLumaAttribute, "aLumaCoord");
  glBindAttribLocation(program, kChromaAttribute, "aChromaCoord");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    char log[512];
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    LOGE("Program link failed: %s", log);
    glDeleteProgram(program);
    return false;
  }
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "uTextureY"), 0);
  glUniform1i(glGetUniformLocation(program, "uTextureU"), 1);
  glUniform1i(glGetUniformLocation(program, "uTextureV"), 2);
  gl->matrixLocation = glGetUniformLocation(program, "uMatrix");
  gl->offsetLocation = glGetUniformLocation(program, "uOffset");
  glGenTextures(3, gl->textures);
  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, gl->textures[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl->maxTextureSize);
  gl->program = program;
  return true;
}

static bool ensureEglContext(GlRenderer* gl) {
  if (gl->context != EGL_NO_CONTEXT) {
    return true;
  }
  gl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (gl->display == EGL_NO_DISPLAY || !eglInitialize(gl->display, nullptr, nullptr)) {
    LOGE("eglInitialize failed: 0x%x", eglGetError());
    gl->display = EGL_NO_DISPLAY;
    return false;
  }
  const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                                     EGL_RED_SIZE,        8,
                                     EGL_GREEN_SIZE,      8,
                                     EGL_BLUE_SIZE,       8,
                                     EGL_NONE};
  EGLConfig configs[32];
  EGLint count = 0;
  if (!eglChooseConfig(gl->display, configAttributes, configs, 32, &count) || count < 1) {
    LOGE("eglChooseConfig failed: 0x%x", eglGetError());
    return false;
  }
  gl->config = configs[0];
  for (int i = 0; i < count; i++) {
    EGLint red = 0;
    EGLint green = 0;
    EGLint blue = 0;
    eglGetConfigAttrib(gl->display, configs[i], EGL_RED_SIZE, &red);
    eglGetConfigAttrib(gl->display, configs[i], EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(gl->display, configs[i], EGL_BLUE_SIZE, &blue);
    if (red == 8 && green == 8 && blue == 8) {
      gl->config = configs[i];
      break;
    }
  }
  const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  gl->context = eglCreateContext(gl->display, gl->config, EGL_NO_CONTEXT, contextAttributes);
  if (gl->context == EGL_NO_CONTEXT) {
    LOGE("eglCreateContext failed: 0x%x", eglGetError());
    return false;
  }
  return true;
}

static void colorConversion(int colorspace, bool fullRange, int width, int height, GLfloat matrix[9],
                            GLfloat offset[3]) {
  if (colorspace == kColorspaceUnknown) {
    colorspace = (width >= 1280 || height >= 720) ? kColorspaceBT709 : kColorspaceBT601;
  }
  float kr;
  float kb;
  switch (colorspace) {
    case kColorspaceBT709:
      kr = 0.2126f;
      kb = 0.0722f;
      break;
    case kColorspaceBT2020:
      kr = 0.2627f;
      kb = 0.0593f;
      break;
    default:
      kr = 0.299f;
      kb = 0.114f;
      break;
  }
  const float kg = 1.0f - kr - kb;
  const float lumaScale = fullRange ? 1.0f : 255.0f / 219.0f;
  const float chromaScale = fullRange ? 1.0f : 255.0f / 224.0f;
  matrix[0] = lumaScale;
  matrix[1] = lumaScale;
  matrix[2] = lumaScale;
  matrix[3] = 0.0f;
  matrix[4] = -chromaScale * 2.0f * kb * (1.0f - kb) / kg;
  matrix[5] = chromaScale * 2.0f * (1.0f - kb);
  matrix[6] = chromaScale * 2.0f * (1.0f - kr);
  matrix[7] = -chromaScale * 2.0f * kr * (1.0f - kr) / kg;
  matrix[8] = 0.0f;
  offset[0] = fullRange ? 0.0f : 16.0f / 255.0f;
  offset[1] = 128.0f / 255.0f;
  offset[2] = 128.0f / 255.0f;
}

static void rotatedCoord(float x, float y, int rotationDegrees, float* u, float* v) {
  switch (rotationDegrees) {
    case 90:
      *u = y;
      *v = 1.0f - x;
      break;
    case 180:
      *u = 1.0f - x;
      *v = 1.0f - y;
      break;
    case 270:
      *u = 1.0f - y;
      *v = x;
      break;
    default:
      *u = x;
      *v = y;
      break;
  }
}

static void uploadPlane(GlRenderer* gl, int index, int width, int height, const uint8_t* pixels) {
  glActiveTexture(GL_TEXTURE0 + index);
  glBindTexture(GL_TEXTURE_2D, gl->textures[index]);
  if (gl->textureWidths[index] != width || gl->textureHeights[index] != height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
    gl->textureWidths[index] = width;
    gl->textureHeights[index] = height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
  }
}

static int drawFrame(JNIEnv* env, GlRenderer* gl, jobject outputBuffer, int rotationDegrees) {
  const int width = env->GetIntField(outputBuffer, widthField);
  const int height = env->GetIntField(outputBuffer, heightField);
  const int colorspace = env->GetIntField(outputBuffer, colorspaceField);
  const jlong info = env->GetLongField(outputBuffer, decoderPrivateField);
  jintArray stridesArray = (jintArray)env->GetObjectField(outputBuffer, yuvStridesField);
  jobject dataObject = env->GetObjectField(outputBuffer, dataField);
  if (!stridesArray || !dataObject || width <= 0 || height <= 0) {
    return kResultError;
  }
  jint strides[2];
  env->GetIntArrayRegion(stridesArray, 0, 2, strides);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(env->GetDirectBufferAddress(dataObject));
  if (!src || strides[0] < width || strides[1] < (width + 1) / 2) {
    return kResultError;
  }
  if (!gl->program && !createProgram(gl)) {
    return kResultError;
  }
  if (strides[0] > gl->maxTextureSize || height > gl->maxTextureSize) {
    LOGE("Frame %dx%d exceeds GL_MAX_TEXTURE_SIZE %d.", strides[0], height, gl->maxTextureSize);
    return kResultSkipped;
  }
  EGLint surfaceWidth = 0;
  EGLint surfaceHeight = 0;
  eglQuerySurface(gl->display, gl->surface, EGL_WIDTH, &surfaceWidth);
  eglQuerySurface(gl->display, gl->surface, EGL_HEIGHT, &surfaceHeight);
  if (surfaceWidth <= 0 || surfaceHeight <= 0) {
    return kResultSkipped;
  }
  const int uvWidth = (width + 1) / 2;
  const int uvHeight = (height + 1) / 2;
  const uint8_t* srcU = src + (ptrdiff_t)strides[0] * height;
  const uint8_t* srcV = srcU + (ptrdiff_t)strides[1] * uvHeight;
  glUseProgram(gl->program);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  uploadPlane(gl, 0, strides[0], height, src);
  uploadPlane(gl, 1, strides[1], uvHeight, srcU);
  uploadPlane(gl, 2, strides[1], uvHeight, srcV);
  GLfloat matrix[9];
  GLfloat offset[3];
  colorConversion(colorspace, (info & kFrameInfoFullRange) != 0, width, height, matrix, offset);
  glUniformMatrix3fv(gl->matrixLocation, 1, GL_FALSE, matrix);
  glUniform3fv(gl->offsetLocation, 1, offset);
  const float lumaStart = 0.5f / strides[0];
  const float lumaSpan = (float)(width - 1) / strides[0];
  const float chromaStart = 0.5f / strides[1];
  const float chromaSpan = (float)(uvWidth - 1) / strides[1];
  const float corners[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}};
  const GLfloat positions[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
  GLfloat lumaCoords[8];
  GLfloat chromaCoords[8];
  for (int i = 0; i < 4; i++) {
    float u;
    float v;
    rotatedCoord(corners[i][0], corners[i][1], rotationDegrees, &u, &v);
    lumaCoords[i * 2] = lumaStart + u * lumaSpan;
    lumaCoords[i * 2 + 1] = v;
    chromaCoords[i * 2] = chromaStart + u * chromaSpan;
    chromaCoords[i * 2 + 1] = v;
  }
  glViewport(0, 0, surfaceWidth, surfaceHeight);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(kPositionAttribute, 2, GL_FLOAT, GL_FALSE, 0, positions);
  glVertexAttribPointer(kLumaAttribute, 2, GL_FLOAT, GL_FALSE, 0, lumaCoords);
  glVertexAttribPointer(kChromaAttribute, 2, GL_FLOAT, GL_FALSE, 0, chromaCoords);
  glEnableVertexAttribArray(kPositionAttribute);
  glEnableVertexAttribArray(kLumaAttribute);
  glEnableVertexAttribArray(kChromaAttribute);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(kPositionAttribute);
  glDisableVertexAttribArray(kLumaAttribute);
  glDisableVertexAttribArray(kChromaAttribute);
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    LOGE("GL error 0x%x", error);
    return kResultError;
  }
  return 0;
}

VIDEO_FUNC(jint, ffmpegVideoApiVersion) {
  return kApiVersion;
}

VIDEO_FUNC(jlong, ffmpegVideoInit, jstring codecName, jbyteArray extraData, jint threads) {
  if (!ensureFields(env)) {
    LOGE("VideoDecoderOutputBuffer fields not found.");
    return 0L;
  }
  const char* name = env->GetStringUTFChars(codecName, nullptr);
  const AVCodec* codec = avcodec_find_decoder_by_name(name);
  env->ReleaseStringUTFChars(codecName, name);
  if (!codec) {
    LOGE("Video codec not found.");
    return 0L;
  }
  VideoContext* ctx = new VideoContext();
  ctx->codec = avcodec_alloc_context3(codec);
  ctx->packet = av_packet_alloc();
  ctx->frame = av_frame_alloc();
  if (!ctx->codec || !ctx->packet || !ctx->frame) {
    releaseContext(env, ctx);
    return 0L;
  }
  if (extraData) {
    jsize size = env->GetArrayLength(extraData);
    ctx->codec->extradata = (uint8_t*)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->codec->extradata) {
      releaseContext(env, ctx);
      return 0L;
    }
    ctx->codec->extradata_size = size;
    env->GetByteArrayRegion(extraData, 0, size, (jbyte*)ctx->codec->extradata);
  }
  ctx->codec->thread_count = threads > 0 ? threads : 1;
  ctx->codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  ctx->codec->pkt_timebase = AVRational{1, 1000000};
  int result = avcodec_open2(ctx->codec, codec, nullptr);
  if (result < 0) {
    logError("avcodec_open2", result);
    releaseContext(env, ctx);
    return 0L;
  }
  return reinterpret_cast<jlong>(ctx);
}

VIDEO_FUNC(jint, ffmpegVideoDecode, jlong jContext, jobject data, jint size, jlong timeUs) {
  VideoContext* ctx = reinterpret_cast<VideoContext*>(jContext);
  if (!ctx) {
    return kResultError;
  }
  if (ctx->hasFrame) {
    av_frame_unref(ctx->frame);
    ctx->hasFrame = false;
  }
  uint8_t* bytes = data ? reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(data)) : nullptr;
  if (!bytes || size <= 0) {
    return drainFrame(ctx);
  }
  if (ctx->draining) {
    return kResultNoFrame;
  }
  ctx->packet->data = bytes;
  ctx->packet->size = size;
  ctx->packet->pts = timeUs;
  ctx->packet->dts = AV_NOPTS_VALUE;
  int sent = avcodec_send_packet(ctx->codec, ctx->packet);
  int result = kResultNoFrame;
  if (sent == AVERROR(EAGAIN)) {
    result = receiveFrame(ctx);
    if (result == kResultError) {
      av_packet_unref(ctx->packet);
      return result;
    }
    sent = avcodec_send_packet(ctx->codec, ctx->packet);
    if (sent == AVERROR(EAGAIN)) {
      LOGE("Decoder queue full, packet dropped.");
    }
  }
  av_packet_unref(ctx->packet);
  if (sent < 0 && sent != AVERROR(EAGAIN)) {
    logError("avcodec_send_packet", sent);
    if (sent == AVERROR(ENOMEM)) {
      return kResultError;
    }
  }
  if (result == kResultFrame) {
    return result;
  }
  return receiveFrame(ctx);
}

VIDEO_FUNC(jlong, ffmpegVideoFrameTimeUs, jlong jContext, jlong fallbackTimeUs) {
  VideoContext* ctx = reinterpret_cast<VideoContext*>(jContext);
  if (!ctx || !ctx->hasFrame) {
    return fallbackTimeUs;
  }
  if (ctx->frame->best_effort_timestamp != AV_NOPTS_VALUE) {
    return ctx->frame->best_effort_timestamp;
  }
  if (ctx->frame->pts != AV_NOPTS_VALUE) {
    return ctx->frame->pts;
  }
  return fallbackTimeUs;
}

VIDEO_FUNC(jint, ffmpegVideoCopyFrame, jlong jContext, jobject outputBuffer) {
  VideoContext* ctx = reinterpret_cast<VideoContext*>(jContext);
  if (!ctx || !ctx->hasFrame) {
    return kResultError;
  }
  const AVFrame* frame = ctx->frame;
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
  if (!desc || desc->nb_components < 1 ||
      (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL |
                      AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_BE))) {
    LOGE("Unsupported pixel format %d.", frame->format);
    return kResultError;
  }
  const int width = frame->width;
  const int height = frame->height;
  if (width <= 0 || height <= 0) {
    return kResultError;
  }
  const int yStride = (width + 15) & ~15;
  const int uvWidth = (width + 1) / 2;
  const int uvHeight = (height + 1) / 2;
  const int uvStride = (uvWidth + 15) & ~15;
  jboolean ok = env->CallBooleanMethod(outputBuffer, initForYuvFrameMethod, width, height, yStride,
                                       uvStride, colorspaceOf(frame));
  if (env->ExceptionCheck() || !ok) {
    return kResultError;
  }
  env->SetLongField(outputBuffer, decoderPrivateField, frameInfoOf(frame));
  jobject dataObject = env->GetObjectField(outputBuffer, dataField);
  uint8_t* out = dataObject ? reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(dataObject)) : nullptr;
  if (!out) {
    return kResultError;
  }
  uint8_t* outU = out + (ptrdiff_t)yStride * height;
  uint8_t* outV = outU + (ptrdiff_t)uvStride * uvHeight;
  copyPlane(frame, desc->comp[0], width, height, 1, 0, 0, out, yStride, width, height);
  if (desc->nb_components < 3) {
    for (int y = 0; y < uvHeight; y++) {
      memset(outU + (ptrdiff_t)y * uvStride, 128, uvWidth);
      memset(outV + (ptrdiff_t)y * uvStride, 128, uvWidth);
    }
    return 0;
  }
  const int chromaWidth = AV_CEIL_RSHIFT(width, desc->log2_chroma_w);
  const int chromaHeight = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
  const int log2W = desc->log2_chroma_w;
  const int log2H = desc->log2_chroma_h;
  copyPlane(frame, desc->comp[1], chromaWidth, chromaHeight, 2, log2W, log2H, outU, uvStride, uvWidth, uvHeight);
  copyPlane(frame, desc->comp[2], chromaWidth, chromaHeight, 2, log2W, log2H, outV, uvStride, uvWidth, uvHeight);
  return 0;
}

VIDEO_FUNC(jint, ffmpegVideoRenderFrame, jlong jContext, jobject surface, jobject outputBuffer,
           jint rotationDegrees) {
  VideoContext* ctx = reinterpret_cast<VideoContext*>(jContext);
  if (!ctx || !surface || !outputBuffer) {
    return kResultError;
  }
  if (!ctx->surface || !env->IsSameObject(ctx->surface, surface)) {
    releaseWindow(env, ctx);
    ctx->window = ANativeWindow_fromSurface(env, surface);
    ctx->surface = env->NewGlobalRef(surface);
  }
  if (!ctx->window) {
    return kResultSkipped;
  }
  GlRenderer* gl = &ctx->gl;
  if (!ensureEglContext(gl)) {
    releaseGl(gl);
    return kResultError;
  }
  if (gl->surface == EGL_NO_SURFACE) {
    gl->surface = eglCreateWindowSurface(gl->display, gl->config, ctx->window, nullptr);
    if (gl->surface == EGL_NO_SURFACE) {
      LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
      return kResultSkipped;
    }
  }
  EGLDisplay previousDisplay = eglGetCurrentDisplay();
  EGLContext previousContext = eglGetCurrentContext();
  EGLSurface previousDraw = eglGetCurrentSurface(EGL_DRAW);
  EGLSurface previousRead = eglGetCurrentSurface(EGL_READ);
  if (!eglMakeCurrent(gl->display, gl->surface, gl->surface, gl->context)) {
    LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
    destroyEglSurface(gl);
    return kResultSkipped;
  }
  if (!gl->surfaceConfigured) {
    eglSwapInterval(gl->display, 0);
    gl->surfaceConfigured = true;
  }
  int result = drawFrame(env, gl, outputBuffer, rotationDegrees);
  bool surfaceLost = false;
  bool contextLost = false;
  if (result == 0 && !eglSwapBuffers(gl->display, gl->surface)) {
    EGLint error = eglGetError();
    LOGE("eglSwapBuffers failed: 0x%x", error);
    surfaceLost = true;
    contextLost = error == EGL_CONTEXT_LOST;
    result = kResultSkipped;
  }
  if (previousContext != EGL_NO_CONTEXT && previousContext != gl->context) {
    eglMakeCurrent(previousDisplay, previousDraw, previousRead, previousContext);
  } else {
    eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (contextLost) {
    releaseGl(gl);
  } else if (surfaceLost) {
    destroyEglSurface(gl);
  }
  return result;
}

VIDEO_FUNC(void, ffmpegVideoReleaseSurface, jlong jContext) {
  VideoContext* ctx = reinterpret_cast<VideoContext*>(jContext);
  if (!ctx) {
    return;
  }
  releaseWindow(env, ctx);
}

VIDEO_FUNC(void, ffmpegVideoFlush, jlong jContext) {
  VideoContext* ctx = reinterpret_cast<VideoContext*>(jContext);
  if (!ctx) {
    return;
  }
  if (ctx->hasFrame) {
    av_frame_unref(ctx->frame);
    ctx->hasFrame = false;
  }
  ctx->draining = false;
  avcodec_flush_buffers(ctx->codec);
}

VIDEO_FUNC(void, ffmpegVideoRelease, jlong jContext) {
  releaseContext(env, reinterpret_cast<VideoContext*>(jContext));
}
