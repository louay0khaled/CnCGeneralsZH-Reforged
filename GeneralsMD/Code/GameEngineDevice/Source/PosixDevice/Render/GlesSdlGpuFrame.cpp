/*
** Copyright 2026 CnCGeneralsZH-Reforged contributors.
** Licensed under GPL-3.0-or-later; see LICENSE.md.
**
** Android OpenGL ES implementation of the existing SdlGpuFrame command contract.
** This keeps PosixDevice9/WW3D2 unchanged while replacing SDL_GPU with an immediate GLES 3.0 path.
*/

#include "SdlGpuFrame.h"
#include "Common/CrashHandler.h"
#include "GlesRenderCommon.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{

struct GlesFrameState
{
    SDL_Window *Window;
    SDL_GLContext Context;
    GLuint TargetFbo;
    GLuint UniformBuffers[2];
    GLuint PresentProgram;
    GLuint PresentVao;
    GLint PresentSampler;
    unsigned int DrawableWidth;
    unsigned int DrawableHeight;
    unsigned int FrameId;
};

static const char *PRESENT_VERTEX_SHADER =
    "#version 300 es\n"
    "precision highp float;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "  vec2 p;\n"
    "  if (gl_VertexID == 0) p = vec2(-1.0, -1.0);\n"
    "  else if (gl_VertexID == 1) p = vec2(3.0, -1.0);\n"
    "  else p = vec2(-1.0, 3.0);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  vTexCoord = p * 0.5 + 0.5;\n"
    "}\n";

static const char *PRESENT_FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uBackBuffer;\n"
    "in vec2 vTexCoord;\n"
    "out vec4 outColor;\n"
    "void main() { outColor = texture(uBackBuffer, vTexCoord); }\n";

static GLuint CompilePresentShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (shader == 0)
        return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    char message[1024] = {};
    GLsizei length = 0;
    glGetShaderInfoLog(shader, (GLsizei)sizeof(message) - 1, &length, message);
    fprintf(stderr, "GLES presenter shader compilation failed (0x%x): %s\n",
        (unsigned)type, message);
    glDeleteShader(shader);
    return 0;
}

static GLuint CreatePresentProgram(GLint &samplerLocation)
{
    samplerLocation = -1;
    const GLuint vertex = CompilePresentShader(GL_VERTEX_SHADER, PRESENT_VERTEX_SHADER);
    const GLuint fragment = CompilePresentShader(GL_FRAGMENT_SHADER, PRESENT_FRAGMENT_SHADER);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return 0;
    }

    const GLuint program = glCreateProgram();
    if (program == 0) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return 0;
    }

    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char message[1024] = {};
        GLsizei length = 0;
        glGetProgramInfoLog(program, (GLsizei)sizeof(message) - 1, &length, message);
        fprintf(stderr, "GLES presenter program link failed: %s\n", message);
        glDeleteProgram(program);
        return 0;
    }

    samplerLocation = glGetUniformLocation(program, "uBackBuffer");
    if (samplerLocation < 0) {
        fprintf(stderr, "GLES presenter sampler uniform was not linked\n");
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static unsigned int NextGlesFrameId = 0;

template <typename T>
static void GrowBytes(std::vector<T> &bytes, uint32_t size, uint32_t &offset)
{
    const size_t old = bytes.size();
    offset = (uint32_t)((old + 15u) & ~(size_t)15u);
    bytes.resize((size_t)offset + size);
}

static GlesFrameState *State(SdlGpuFrame *frame)
{
    return reinterpret_cast<GlesFrameState *>(frame->Gles_State());
}

static const GlesFrameState *State(const SdlGpuFrame *frame)
{
    return reinterpret_cast<const GlesFrameState *>(frame->Gles_State());
}

static GlesTexture *MakeColorTexture(unsigned int width, unsigned int height)
{
    GlesTexture *texture = new GlesTexture();
    texture->Name = 0;
    texture->Fbo = 0;
    texture->Width = width;
    texture->Height = height;
    texture->Depth = false;
    texture->RenderTarget = true;

    glGenTextures(1, &texture->Name);
    if (texture->Name == 0) {
        delete texture;
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, texture->Name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &texture->Fbo);
    if (texture->Fbo == 0) {
        Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(texture));
        return NULL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, texture->Fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->Name, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GlesFrame: back/front framebuffer incomplete: 0x%x\\n", (unsigned)status);
        Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(texture));
        return NULL;
    }
    return texture;
}

static GlesTexture *MakeDepthTexture(unsigned int width, unsigned int height)
{
    GlesTexture *texture = new GlesTexture();
    texture->Name = 0;
    texture->Fbo = 0;
    texture->Width = width;
    texture->Height = height;
    texture->Depth = true;
    texture->RenderTarget = true;

    glGenTextures(1, &texture->Name);
    if (texture->Name == 0) {
        delete texture;
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, texture->Name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)width, (GLsizei)height, 0,
        GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);

    glGenFramebuffers(1, &texture->Fbo);
    if (texture->Fbo == 0) {
        Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(texture));
        return NULL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, texture->Fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture->Name, 0);
    glDrawBuffers(0, NULL);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GlesFrame: depth framebuffer incomplete: 0x%x\\n", (unsigned)status);
        Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(texture));
        return NULL;
    }
    return texture;
}

static bool BindTarget(GlesFrameState *state, const SdlTarget &target)
{
    glBindFramebuffer(GL_FRAMEBUFFER, state->TargetFbo);
    GlesTexture *colour = Gles_Texture(target.Colour);
    GlesTexture *depth = Gles_Texture(target.Depth);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        colour != NULL ? colour->Name : 0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
        depth != NULL ? depth->Name : 0, 0);
    if (colour != NULL) {
        const GLenum draw = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &draw);
    } else {
        glDrawBuffers(0, NULL);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GlesFrame: target framebuffer incomplete: 0x%x (%ux%u)\\n",
            (unsigned)status, target.Width, target.Height);
        return false;
    }
    glViewport(0, 0, (GLsizei)target.Width, (GLsizei)target.Height);
    return true;
}

static GLuint ReadFboFor(GlesTexture *texture, GLuint &temporary)
{
    temporary = 0;
    if (texture != NULL && texture->Fbo != 0)
        return texture->Fbo;
    if (texture == NULL || texture->Depth)
        return 0;
    glGenFramebuffers(1, &temporary);
    glBindFramebuffer(GL_FRAMEBUFFER, temporary);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->Name, 0);
    return temporary;
}

static void DeleteTemporaryFbo(GLuint &fbo)
{
    if (fbo != 0) {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
}

static void SetTopLeftScissor(unsigned int targetHeight, int x, int y, int width, int height)
{
    const int bottom = (int)targetHeight - y - height;
    glScissor(x, bottom, width, height);
}

static bool Gles_Vertex_Attrib_Local(uint32_t format, GLenum &type, GLint &components, GLboolean &normalized, bool &integer)
{
    type = GL_FLOAT;
    components = 4;
    normalized = GL_FALSE;
    integer = false;
    switch (format) {
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT: components = 1; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2: components = 2; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3: components = 3; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4: components = 4; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT: type = GL_INT; components = 1; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT2: type = GL_INT; components = 2; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT3: type = GL_INT; components = 3; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT4: type = GL_INT; components = 4; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT: type = GL_UNSIGNED_INT; components = 1; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT2: type = GL_UNSIGNED_INT; components = 2; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT3: type = GL_UNSIGNED_INT; components = 3; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT4: type = GL_UNSIGNED_INT; components = 4; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2: type = GL_BYTE; components = 2; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4: type = GL_BYTE; components = 4; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2: type = GL_UNSIGNED_BYTE; components = 2; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4: type = GL_UNSIGNED_BYTE; components = 4; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM: type = GL_BYTE; components = 2; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM: type = GL_BYTE; components = 4; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM: type = GL_UNSIGNED_BYTE; components = 2; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM: type = GL_UNSIGNED_BYTE; components = 4; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2: type = GL_SHORT; components = 2; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4: type = GL_SHORT; components = 4; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2: type = GL_UNSIGNED_SHORT; components = 2; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4: type = GL_UNSIGNED_SHORT; components = 4; integer = true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM: type = GL_SHORT; components = 2; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM: type = GL_SHORT; components = 4; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM: type = GL_UNSIGNED_SHORT; components = 2; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM: type = GL_UNSIGNED_SHORT; components = 4; normalized = GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_HALF2: type = GL_HALF_FLOAT; components = 2; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_HALF4: type = GL_HALF_FLOAT; components = 4; return true;
        default: return false;
    }
}

static void BindUniformBlockState(GlesFrameState *state, unsigned int stage, const void *data, uint32_t size)
{
    glBindBuffer(GL_UNIFORM_BUFFER, state->UniformBuffers[stage]);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)std::max<uint32_t>(size, 16u), data, GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, stage, state->UniformBuffers[stage]);
}

static bool SetupAttributes(const GlesPipeline &pipeline, const SdlRecordedDraw &draw,
    const GlesBuffer *vertex, unsigned int targetHeight)
{
    if (vertex == NULL || vertex->Name == 0) return false;
    glBindBuffer(GL_ARRAY_BUFFER, vertex->Name);
    const int64_t base = (int64_t)draw.BaseVertex;
    if (base < 0) return false;

    for (unsigned int i = 0; i < pipeline.Layout.AttributeCount; ++i) {
        GLenum type;
        GLint components;
        GLboolean normalized;
        bool integer;
        if (!Gles_Vertex_Attrib_Local(pipeline.Layout.Format[i], type, components, normalized, integer)) {
            return false;
        }
        glEnableVertexAttribArray(pipeline.Layout.Location[i]);
        const uintptr_t pointer = (uintptr_t)draw.VertexOffset +
            (uintptr_t)base * pipeline.Layout.Stride + pipeline.Layout.Offset[i];
        if (integer && normalized == GL_FALSE) {
            glVertexAttribIPointer(pipeline.Layout.Location[i], components, type,
                (GLsizei)pipeline.Layout.Stride, (const void *)pointer);
        } else {
            glVertexAttribPointer(pipeline.Layout.Location[i], components, type, normalized,
                (GLsizei)pipeline.Layout.Stride, (const void *)pointer);
        }
    }
    (void)targetHeight;
    return true;
}

static void SetDepthRange(const float *viewport)
{
    glDepthRangef(viewport[4], viewport[5]);
}

static GLenum GlesCompareLocal(uint8_t op);
static GLenum GlesStencilLocal(uint8_t op);
static GLenum GlesBlendLocal(uint8_t factor);
static GLenum GlesBlendOpLocal(uint8_t op);

static bool DrawOne(const SdlRecordedDraw &draw, const GlesBuffer *stagedBuffer, GlesFrameState *state,
    unsigned int targetWidth, unsigned int targetHeight, const uint8_t *constants, size_t constantsSize)
{
    if (draw.Pipeline == NULL) return false;
    GlesPipeline *pipeline = Gles_Pipeline(draw.Pipeline);
    if (pipeline == NULL || pipeline->Program == 0) return false;

    glUseProgram(pipeline->Program);

    const GlesBuffer *vertices = draw.VertexBuffer != NULL
        ? Gles_Buffer(draw.VertexBuffer)
        : stagedBuffer;
    if (vertices == NULL) {
        fprintf(stderr, "GlesFrame: draw without a vertex buffer\\n");
        return false;
    }

    const GlesBuffer *indices = draw.IndexBuffer != NULL
        ? Gles_Buffer(draw.IndexBuffer)
        : (draw.IndexSize != 0 ? stagedBuffer : NULL);
    (void)targetWidth;

    // Pipeline state is applied here because GLES has no immutable graphics pipeline object.
    // This label intentionally keeps all state changes adjacent to the immediate draw.
    if (pipeline->Cull == SDL_GPU_CULLMODE_NONE) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glCullFace(pipeline->Cull == SDL_GPU_CULLMODE_FRONT ? GL_FRONT : GL_BACK);
    }
    glFrontFace(GL_CW);

    if (pipeline->DepthTest) {
        glEnable(GL_DEPTH_TEST);
        switch (pipeline->DepthCompare) {
            case SDL_GPU_COMPAREOP_NEVER: glDepthFunc(GL_NEVER); break;
            case SDL_GPU_COMPAREOP_LESS: glDepthFunc(GL_LESS); break;
            case SDL_GPU_COMPAREOP_EQUAL: glDepthFunc(GL_EQUAL); break;
            case SDL_GPU_COMPAREOP_LESS_OR_EQUAL: glDepthFunc(GL_LEQUAL); break;
            case SDL_GPU_COMPAREOP_GREATER: glDepthFunc(GL_GREATER); break;
            case SDL_GPU_COMPAREOP_NOT_EQUAL: glDepthFunc(GL_NOTEQUAL); break;
            case SDL_GPU_COMPAREOP_GREATER_OR_EQUAL: glDepthFunc(GL_GEQUAL); break;
            default: glDepthFunc(GL_ALWAYS); break;
        }
    } else glDisable(GL_DEPTH_TEST);
    glDepthMask(pipeline->DepthWrite ? GL_TRUE : GL_FALSE);

    if (pipeline->StencilEnable) {
        glEnable(GL_STENCIL_TEST);
        glStencilMaskSeparate(GL_FRONT, pipeline->StencilWrite);
        glStencilMaskSeparate(GL_BACK, pipeline->StencilWrite);
        const GLenum frontCompare = GlesCompareLocal(pipeline->FrontCompare);
        const GLenum backCompare = GlesCompareLocal(pipeline->BackCompare);
        glStencilFuncSeparate(GL_FRONT, frontCompare, (GLint)draw.StencilReference, pipeline->StencilRead);
        glStencilFuncSeparate(GL_BACK, backCompare, (GLint)draw.StencilReference, pipeline->StencilRead);
        glStencilOpSeparate(GL_FRONT, GlesStencilLocal(pipeline->FrontFail),
            GlesStencilLocal(pipeline->FrontDepthFail), GlesStencilLocal(pipeline->FrontPass));
        glStencilOpSeparate(GL_BACK, GlesStencilLocal(pipeline->BackFail),
            GlesStencilLocal(pipeline->BackDepthFail), GlesStencilLocal(pipeline->BackPass));
    } else {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0);
    }

    glColorMask((pipeline->WriteMask & SDL_GPU_COLORCOMPONENT_R) != 0,
        (pipeline->WriteMask & SDL_GPU_COLORCOMPONENT_G) != 0,
        (pipeline->WriteMask & SDL_GPU_COLORCOMPONENT_B) != 0,
        (pipeline->WriteMask & SDL_GPU_COLORCOMPONENT_A) != 0);

    if (pipeline->BlendEnable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GlesBlendLocal(pipeline->SourceColour), GlesBlendLocal(pipeline->DestinationColour),
            GlesBlendLocal(pipeline->SourceAlpha), GlesBlendLocal(pipeline->DestinationAlpha));
        glBlendEquationSeparate(GlesBlendOpLocal(pipeline->ColourOperation), GlesBlendOpLocal(pipeline->AlphaOperation));
        const uint32_t f = draw.BlendFactor;
        glBlendColor((float)((f >> 16) & 0xFF) / 255.0f, (float)((f >> 8) & 0xFF) / 255.0f,
            (float)(f & 0xFF) / 255.0f, (float)((f >> 24) & 0xFF) / 255.0f);
    } else glDisable(GL_BLEND);

    if (pipeline->DepthBias != 0.0f || pipeline->SlopeBias != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pipeline->SlopeBias, pipeline->DepthBias);
    } else glDisable(GL_POLYGON_OFFSET_FILL);

    glViewport((GLint)draw.Viewport[0],
        (GLint)targetHeight - (GLint)draw.Viewport[1] - (GLint)draw.Viewport[3],
        (GLsizei)draw.Viewport[2], (GLsizei)draw.Viewport[3]);
    SetDepthRange(draw.Viewport);

    if (draw.Scissor[2] > 0 && draw.Scissor[3] > 0) {
        glEnable(GL_SCISSOR_TEST);
        SetTopLeftScissor(targetHeight, draw.Scissor[0], draw.Scissor[1], draw.Scissor[2], draw.Scissor[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    if (!SetupAttributes(*pipeline, draw, vertices, targetHeight)) {
#if defined(__ANDROID__)
        static unsigned int attributeReports = 0;
        if (attributeReports < 12) {
            char diagnostic[512];
            snprintf(diagnostic, sizeof(diagnostic),
                "GLES draw rejected: vertex attribute setup failed program=%u vertexBuffer=%u target=%ux%u attributes=%u",
                (unsigned)pipeline->Program, vertices != NULL ? (unsigned)vertices->Name : 0u,
                targetWidth, targetHeight, pipeline->Layout.AttributeCount);
            appendAndroidDiagnostic(diagnostic);
            ++attributeReports;
        }
#endif
        return false;
    }

    if (draw.VertexConstantsSize != 0 && draw.VertexConstants + draw.VertexConstantsSize <= constantsSize)
        BindUniformBlockState(state, 0, constants + draw.VertexConstants, draw.VertexConstantsSize);
    else
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);

    if (draw.PixelConstantsSize != 0 && draw.PixelConstants + draw.PixelConstantsSize <= constantsSize)
        BindUniformBlockState(state, 1, constants + draw.PixelConstants, draw.PixelConstantsSize);
    else
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, 0);

    for (unsigned int slot = 0; slot < draw.SamplerCount && slot < SdlRecordedDraw::MAXIMUM_SAMPLERS; ++slot) {
        GlesTexture *texture = Gles_Texture(draw.Textures[slot]);
        GlesSampler *sampler = Gles_Sampler(draw.Samplers[slot]);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, texture != NULL ? texture->Name : 0);
        glBindSampler(slot, sampler != NULL ? sampler->Name : 0);
    }

    GLenum stateError = GL_NO_ERROR;
    unsigned int stateErrorCount = 0;
    GLenum pendingError = GL_NO_ERROR;
    while ((pendingError = glGetError()) != GL_NO_ERROR) {
        if (stateError == GL_NO_ERROR) stateError = pendingError;
        ++stateErrorCount;
    }
#if defined(__ANDROID__)
    if (stateError != GL_NO_ERROR) {
        static unsigned int stateErrorReports = 0;
        if (stateErrorReports < 12) {
            char diagnostic[512];
            snprintf(diagnostic, sizeof(diagnostic),
                "GLES draw setup GL error: frame=%u program=%u firstError=0x%04X errorCount=%u",
                state->FrameId, (unsigned)pipeline->Program, (unsigned)stateError, stateErrorCount);
            appendAndroidDiagnostic(diagnostic);
            ++stateErrorReports;
        }
    }
#endif

    GLenum mode = GL_TRIANGLES;
    switch (pipeline->Primitive) {
        case SDL_GPU_PRIMITIVETYPE_POINTLIST: mode = GL_POINTS; break;
        case SDL_GPU_PRIMITIVETYPE_LINELIST: mode = GL_LINES; break;
        case SDL_GPU_PRIMITIVETYPE_LINESTRIP: mode = GL_LINE_STRIP; break;
        case SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP: mode = GL_TRIANGLE_STRIP; break;
        default: mode = GL_TRIANGLES; break;
    }

    if (draw.IndexSize != 0 && indices != NULL) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices->Name);
        const GLenum type = draw.IndexSize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        const uintptr_t indexPointer = (uintptr_t)draw.IndexOffset +
            (uintptr_t)draw.First * draw.IndexSize;
        glDrawElements(mode, (GLsizei)draw.Count, type, (const void *)indexPointer);
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDrawArrays(mode, (GLint)draw.First, (GLsizei)draw.Count);
    }
    const GLenum drawError = glGetError();
#if defined(__ANDROID__)
    if (drawError != GL_NO_ERROR) {
        static unsigned int drawErrorReports = 0;
        if (drawErrorReports < 10) {
            char diagnostic[512];
            snprintf(diagnostic, sizeof(diagnostic),
                "GLES draw GL error: frame=%u program=%u error=0x%04X",
                state->FrameId, (unsigned)pipeline->Program, (unsigned)drawError);
            appendAndroidDiagnostic(diagnostic);
            ++drawErrorReports;
        }
    }
#endif
    (void)targetWidth;
    return drawError == GL_NO_ERROR;
}

static GLenum GlesCompareLocal(uint8_t op)
{
    switch (op) {
        case SDL_GPU_COMPAREOP_NEVER: return GL_NEVER;
        case SDL_GPU_COMPAREOP_LESS: return GL_LESS;
        case SDL_GPU_COMPAREOP_EQUAL: return GL_EQUAL;
        case SDL_GPU_COMPAREOP_LESS_OR_EQUAL: return GL_LEQUAL;
        case SDL_GPU_COMPAREOP_GREATER: return GL_GREATER;
        case SDL_GPU_COMPAREOP_NOT_EQUAL: return GL_NOTEQUAL;
        case SDL_GPU_COMPAREOP_GREATER_OR_EQUAL: return GL_GEQUAL;
        default: return GL_ALWAYS;
    }
}

static GLenum GlesStencilLocal(uint8_t op)
{
    switch (op) {
        case SDL_GPU_STENCILOP_KEEP: return GL_KEEP;
        case SDL_GPU_STENCILOP_ZERO: return GL_ZERO;
        case SDL_GPU_STENCILOP_REPLACE: return GL_REPLACE;
        case SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP: return GL_INCR;
        case SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP: return GL_DECR;
        case SDL_GPU_STENCILOP_INVERT: return GL_INVERT;
        case SDL_GPU_STENCILOP_INCREMENT_AND_WRAP: return GL_INCR_WRAP;
        case SDL_GPU_STENCILOP_DECREMENT_AND_WRAP: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

static GLenum GlesBlendLocal(uint8_t factor)
{
    switch (factor) {
        case SDL_GPU_BLENDFACTOR_ZERO: return GL_ZERO;
        case SDL_GPU_BLENDFACTOR_ONE: return GL_ONE;
        case SDL_GPU_BLENDFACTOR_SRC_COLOR: return GL_SRC_COLOR;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
        case SDL_GPU_BLENDFACTOR_DST_COLOR: return GL_DST_COLOR;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_COLOR;
        case SDL_GPU_BLENDFACTOR_SRC_ALPHA: return GL_SRC_ALPHA;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case SDL_GPU_BLENDFACTOR_DST_ALPHA: return GL_DST_ALPHA;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
        case SDL_GPU_BLENDFACTOR_CONSTANT_COLOR: return GL_CONSTANT_COLOR;
        case SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
        case SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE: return GL_SRC_ALPHA_SATURATE;
        default: return GL_ONE;
    }
}

static GLenum GlesBlendOpLocal(uint8_t op)
{
    switch (op) {
        case SDL_GPU_BLENDOP_SUBTRACT: return GL_FUNC_SUBTRACT;
        case SDL_GPU_BLENDOP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case SDL_GPU_BLENDOP_MIN: return GL_MIN;
        case SDL_GPU_BLENDOP_MAX: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}

}

bool SdlGpuFrame::Gles_Replay()
{
    SdlGpuFrame *frame = this;
    GlesFrameState *state = State(frame);
    if (state == NULL || state->Context == NULL) return false;
    // SDL3: true means the context is current, false means failure.
    if (!SDL_GL_MakeCurrent(state->Window, state->Context)) {
#if defined(__ANDROID__)
        static bool reported = false;
        if (!reported) {
            reported = true;
            char diagnostic[512];
            snprintf(diagnostic, sizeof(diagnostic), "GLES replay SDL_GL_MakeCurrent failed: %s", SDL_GetError());
            appendAndroidDiagnostic(diagnostic);
        }
#endif
        return false;
    }

    unsigned int drawCommands = 0;
    unsigned int clearCommands = 0;
    unsigned int skippedDraws = 0;
    unsigned int executedDraws = 0;
    unsigned int failedDraws = 0;
    unsigned int blitCommands = 0;
    unsigned int failedBlits = 0;
    unsigned int failedTargetBinds = 0;
    GlesBuffer stagedBuffer;
    memset(&stagedBuffer, 0, sizeof(stagedBuffer));
    if (!frame->StreamBytes.empty()) {
        glGenBuffers(1, &stagedBuffer.Name);
        if (stagedBuffer.Name != 0) {
            stagedBuffer.Target = GL_ARRAY_BUFFER;
            stagedBuffer.Size = (uint32_t)frame->StreamBytes.size();
            glBindBuffer(GL_ARRAY_BUFFER, stagedBuffer.Name);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)frame->StreamBytes.size(),
                &frame->StreamBytes[0].Value, GL_STREAM_DRAW);
        }
    }

    for (size_t i = 0; i < frame->Commands.size(); ++i) {
        const SdlGpuFrame::Command &command = frame->Commands[i];
        if (command.IsBlit) {
            ++blitCommands;
            GlesTexture *source = Gles_Texture(command.BlitSource);
            GlesTexture *destination = Gles_Texture(command.BlitDestination);
            if (source == NULL || destination == NULL) {
                ++failedBlits;
#if defined(__ANDROID__)
                static unsigned int invalidBlitReports = 0;
                if (invalidBlitReports < 8) {
                    char diagnostic[512];
                    snprintf(diagnostic, sizeof(diagnostic),
                        "GLES blit rejected: frame=%u cmd=%u source=%s destination=%s",
                        state->FrameId, (unsigned)i, source != NULL ? "valid" : "null",
                        destination != NULL ? "valid" : "null");
                    appendAndroidDiagnostic(diagnostic);
                    ++invalidBlitReports;
                }
#endif
                continue;
            }
            GLuint sourceTemp = 0, destinationTemp = 0;
            const GLuint sourceFbo = ReadFboFor(source, sourceTemp);
            const GLuint destinationFbo = ReadFboFor(destination, destinationTemp);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFbo);
            const GLenum sourceStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            const GLenum destinationStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            const GLenum preBlitError = glGetError();
            glBlitFramebuffer(command.Rect[0], (GLint)source->Height - command.Rect[1] - command.Rect[3],
                command.Rect[0] + command.Rect[2], (GLint)source->Height - command.Rect[1],
                command.BlitRect[0], (GLint)destination->Height - command.BlitRect[1] - command.BlitRect[3],
                command.BlitRect[0] + command.BlitRect[2], (GLint)destination->Height - command.BlitRect[1],
                GL_COLOR_BUFFER_BIT, command.BlitLinear ? GL_LINEAR : GL_NEAREST);
            const GLenum blitError = glGetError();
            if (sourceStatus != GL_FRAMEBUFFER_COMPLETE || destinationStatus != GL_FRAMEBUFFER_COMPLETE ||
                preBlitError != GL_NO_ERROR || blitError != GL_NO_ERROR) {
                ++failedBlits;
            }
#if defined(__ANDROID__)
            {
                static unsigned int blitReports = 0;
                if (blitReports < 12 || sourceStatus != GL_FRAMEBUFFER_COMPLETE ||
                    destinationStatus != GL_FRAMEBUFFER_COMPLETE || preBlitError != GL_NO_ERROR ||
                    blitError != GL_NO_ERROR) {
                    char diagnostic[768];
                    snprintf(diagnostic, sizeof(diagnostic),
                        "GLES BLIT: frame=%u cmd=%u srcTex=%u srcFbo=%u %ux%u srcStatus=0x%04X "
                        "dstTex=%u dstFbo=%u %ux%u dstStatus=0x%04X rect=%dx%d preError=0x%04X blitError=0x%04X",
                        state->FrameId, (unsigned)i, (unsigned)source->Name, (unsigned)sourceFbo,
                        source->Width, source->Height, (unsigned)sourceStatus,
                        (unsigned)destination->Name, (unsigned)destinationFbo,
                        destination->Width, destination->Height, (unsigned)destinationStatus,
                        command.Rect[2], command.Rect[3], (unsigned)preBlitError, (unsigned)blitError);
                    appendAndroidDiagnostic(diagnostic);
                    ++blitReports;
                }
            }
#endif
            DeleteTemporaryFbo(sourceTemp);
            DeleteTemporaryFbo(destinationTemp);
            continue;
        }

        SdlTarget target = frame->Targets[command.Target];
        if (!BindTarget(state, target)) {
            ++failedTargetBinds;
#if defined(__ANDROID__)
            static unsigned int targetBindReports = 0;
            if (targetBindReports < 8) {
                GlesTexture *colour = Gles_Texture(target.Colour);
                GlesTexture *depth = Gles_Texture(target.Depth);
                char diagnostic[512];
                snprintf(diagnostic, sizeof(diagnostic),
                    "GLES target bind rejected: frame=%u cmd=%u target=%u size=%ux%u colourTex=%u depthTex=%u",
                    state->FrameId, (unsigned)i, (unsigned)command.Target, target.Width, target.Height,
                    colour != NULL ? (unsigned)colour->Name : 0u,
                    depth != NULL ? (unsigned)depth->Name : 0u);
                appendAndroidDiagnostic(diagnostic);
                ++targetBindReports;
            }
#endif
            continue;
        }

        if (!command.IsDraw) {
            ++clearCommands;
            GLbitfield clear = 0;
            GLboolean oldColor[4];
            GLboolean oldDepth = GL_FALSE;
            GLint oldStencil = 0;
            glGetBooleanv(GL_COLOR_WRITEMASK, oldColor);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepth);
            glGetIntegerv(GL_STENCIL_WRITEMASK, &oldStencil);
            if (command.Colour) {
                const float a = (float)((command.Argb >> 24) & 0xFF) / 255.0f;
                const float r = (float)((command.Argb >> 16) & 0xFF) / 255.0f;
                const float g = (float)((command.Argb >> 8) & 0xFF) / 255.0f;
                const float b = (float)(command.Argb & 0xFF) / 255.0f;
                glClearColor(r, g, b, a);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                clear |= GL_COLOR_BUFFER_BIT;
            }
            if (command.Depth) {
                glClearDepthf(command.Z);
                glDepthMask(GL_TRUE);
                clear |= GL_DEPTH_BUFFER_BIT;
            }
            if (command.Stencil) {
                glClearStencil((GLint)command.StencilValue);
                glStencilMask(0xFF);
                clear |= GL_STENCIL_BUFFER_BIT;
            }
            glDisable(GL_SCISSOR_TEST);
            glClear(clear);
            glColorMask(oldColor[0], oldColor[1], oldColor[2], oldColor[3]);
            glDepthMask(oldDepth);
            glStencilMask((GLuint)oldStencil);
        } else if (command.IsRect) {
            ++clearCommands;
            GLbitfield clear = 0;
            const unsigned int h = target.Height;
            glEnable(GL_SCISSOR_TEST);
            SetTopLeftScissor(h, command.Rect[0], command.Rect[1], command.Rect[2], command.Rect[3]);
            if (command.Colour) {
                const float a = (float)((command.Argb >> 24) & 0xFF) / 255.0f;
                const float r = (float)((command.Argb >> 16) & 0xFF) / 255.0f;
                const float g = (float)((command.Argb >> 8) & 0xFF) / 255.0f;
                const float b = (float)(command.Argb & 0xFF) / 255.0f;
                glClearColor(r, g, b, a);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                clear |= GL_COLOR_BUFFER_BIT;
            }
            if (command.Depth) {
                glClearDepthf(command.Z);
                glDepthMask(GL_TRUE);
                clear |= GL_DEPTH_BUFFER_BIT;
            }
            if (command.Stencil) {
                glClearStencil((GLint)command.StencilValue);
                glStencilMask(0xFF);
                clear |= GL_STENCIL_BUFFER_BIT;
            }
            glClear(clear);
            glDisable(GL_SCISSOR_TEST);
        } else {
            // The program cache has already compiled and pipeline cache has already linked the program.
            // Constants and resource mirrors are current at this point.
            ++drawCommands;
            if (frame->Draws[command.Draw].Pipeline == NULL) {
                ++skippedDraws;
                continue;
            }
            if (DrawOne(frame->Draws[command.Draw], stagedBuffer.Name != 0 ? &stagedBuffer : NULL,
                state, target.Width, target.Height,
                frame->ConstantBytes.empty() ? NULL : &frame->ConstantBytes[0].Value, frame->ConstantBytes.size())) {
                ++executedDraws;
            } else {
                ++failedDraws;
#if defined(__ANDROID__)
                static unsigned int failedDrawReports = 0;
                if (failedDrawReports < 16) {
                    const SdlRecordedDraw &failed = frame->Draws[command.Draw];
                    GlesPipeline *failedPipeline = Gles_Pipeline(failed.Pipeline);
                    char diagnostic[512];
                    snprintf(diagnostic, sizeof(diagnostic),
                        "GLES draw not executed: frame=%u cmd=%u draw=%u program=%u target=%ux%u "
                        "samplers=%u count=%u indexSize=%u",
                        state->FrameId, (unsigned)i, (unsigned)command.Draw,
                        failedPipeline != NULL ? (unsigned)failedPipeline->Program : 0u,
                        target.Width, target.Height, failed.SamplerCount, failed.Count, failed.IndexSize);
                    appendAndroidDiagnostic(diagnostic);
                    ++failedDrawReports;
                }
#endif
            }
        }
    }

    // Draw state is owned by the game command stream. Reset the state that can affect
    // the subsequent FBO -> default-framebuffer presentation explicitly.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
#if defined(__ANDROID__)
    {
        static bool reportedBackPixel = false;
        GlesTexture *backPixelTarget = Gles_Texture(frame->BackBuffer);
        if (!reportedBackPixel && backPixelTarget != NULL && backPixelTarget->Fbo != 0) {
            unsigned char pixel[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_FRAMEBUFFER, backPixelTarget->Fbo);
            glReadPixels((GLint)(frame->BackWidth / 2u), (GLint)(frame->BackHeight / 2u),
                1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            const GLenum readError = glGetError();
            char diagnostic[512];
            snprintf(diagnostic, sizeof(diagnostic),
                "GLES back pixel: frame=%u rgba=%u,%u,%u,%u readError=0x%04X",
                state->FrameId, pixel[0], pixel[1], pixel[2], pixel[3], (unsigned)readError);
            appendAndroidDiagnostic(diagnostic);
            reportedBackPixel = true;
        }
    }
#endif
    if (stagedBuffer.Name != 0) {
        glDeleteBuffers(1, &stagedBuffer.Name);
        stagedBuffer.Name = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    const GLenum replayError = glGetError();
#if defined(__ANDROID__)
    {
        static unsigned int reports = 0;
        static bool reportedDrawBatch = false;
        if (reports < 6 || (executedDraws > 0 && !reportedDrawBatch)) {
            char diagnostic[768];
            snprintf(diagnostic, sizeof(diagnostic),
                "GLES replay: frame=%u commands=%u draws=%u executed=%u skipped=%u failedDraws=%u "
                "blits=%u failedBlits=%u targetBindFailures=%u clears=%u glError=0x%04X",
                state->FrameId, (unsigned)frame->Commands.size(), drawCommands, executedDraws,
                skippedDraws, failedDraws, blitCommands, failedBlits, failedTargetBinds,
                clearCommands, (unsigned)replayError);
            appendAndroidDiagnostic(diagnostic);
            ++reports;
            if (executedDraws > 0) reportedDrawBatch = true;
        }
    }
#endif
    return replayError == GL_NO_ERROR;
}

SdlGpuFrame::SdlGpuFrame() :
    SerializeSubmits(false),
    FlushMs(0.0),
    FenceMs(0.0),
    Flushes(0),
    AcquireMs(0.0),
    NotVisible(0),
    NotVisibleTotal(0),
    NotShown(0),
    NotShownTotal(0),
    ShownWidth(0),
    ShownHeight(0),
    FlushLimit(0),
    FlushesTotal(0),
    FlushInFlightMost(0),
    FlushWaits(0),
    BatchDrawLimit(0),
    DrawLimitFlushes(0),
    OffscreenMs(0.0),
    OffscreenPresents(false),
    OffscreenHz(0),
    DisplayTexture(NULL),
    InFlightNext(0),
    NextTickNs(0),
    GpuDevice(NULL),
    Window(NULL),
    BackBuffer(NULL),
    DepthStencil(NULL),
    FrontCopy(NULL),
    DepthFormat(SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT),
    BackWidth(0),
    BackHeight(0),
    TargetSet(false),
    BatchNumber(0),
    ClearVertex(NULL),
    ClearPixel(NULL),
    StreamBuffer(NULL),
    StreamBufferSize(0),
    Transfer(NULL),
    TransferSize(0),
    GammaVertex(NULL),
    GammaPixel(NULL),
    GammaPipeline(NULL),
    GammaPipelineFormat(0),
    RampTexture(NULL),
    PointSampler(NULL)
{
    LastConstants[0] = LastConstants[1] = 0;
    LastConstantsSize[0] = LastConstantsSize[1] = 0;
    ClearPipelines[0] = ClearPipelines[1] = ClearPipelines[2] = ClearPipelines[3] = NULL;
    ClearPipelines[4] = ClearPipelines[5] = ClearPipelines[6] = ClearPipelines[7] = NULL;
    InFlight[0] = InFlight[1] = NULL;
#if defined(__ANDROID__)
    GlesState = NULL;
#endif
}

SdlGpuFrame *SdlGpuFrame::Create(RenderWindow window, unsigned int width, unsigned int height, std::string &error)
{
    error.clear();
#if defined(__ANDROID__)
    char stage[256];
    snprintf(stage, sizeof(stage), "GLES Create entered: %ux%u", width, height);
    appendAndroidDiagnostic(stage);
#endif
    if (window == NULL) {
        error = "Android GLES requires a window";
        return NULL;
    }

    // SDL3 applies GL attributes when the OpenGL window is created. Android sets
    // these before SDL_CreateWindow() in SdlGameEngine::createWindow().
    // Keep context creation here side-effect free: SDL_GL_CreateContext() itself
    // makes the newly-created context current.
    SDL_GLContext context = SDL_GL_CreateContext((SDL_Window *)window);
    if (context == NULL) {
#if defined(__ANDROID__)
        {
            char diagnostic[512];
            snprintf(diagnostic, sizeof(diagnostic), "SDL_GL_CreateContext failed: %s", SDL_GetError());
            appendAndroidDiagnostic(diagnostic);
        }
#endif
        error = std::string("OpenGL ES context: ") + SDL_GetError();
        return NULL;
    }
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
#if defined(__ANDROID__)
    {
        char info[768];
        snprintf(info, sizeof(info), "GLES context active: version=%s vendor=%s",
            version != NULL ? version : "(null)", vendor != NULL ? vendor : "(null)");
        appendAndroidDiagnostic(info);
    }
#endif
    if (version == NULL || strstr(version, "OpenGL ES 3.") != version) {
        error = "the device did not expose OpenGL ES 3.x";
        SDL_GL_DestroyContext(context);
        return NULL;
    }

    SdlGpuFrame *frame = new SdlGpuFrame();
    GlesFrameState *state = new GlesFrameState();
    state->Window = (SDL_Window *)window;
    state->Context = context;
    state->TargetFbo = 0;
    state->UniformBuffers[0] = state->UniformBuffers[1] = 0;
    state->PresentProgram = 0;
    state->PresentVao = 0;
    state->PresentSampler = -1;
    state->DrawableWidth = width;
    state->DrawableHeight = height;
    state->FrameId = ++NextGlesFrameId;
    glGenFramebuffers(1, &state->TargetFbo);
    glGenBuffers(2, state->UniformBuffers);
#if defined(__ANDROID__)
    {
        char info[256];
        snprintf(info, sizeof(info), "GLES frame id=%u objects: fbo=%u ubo0=%u ubo1=%u",
            state->FrameId, (unsigned)state->TargetFbo, (unsigned)state->UniformBuffers[0], (unsigned)state->UniformBuffers[1]);
        appendAndroidDiagnostic(info);
    }
#endif
    if (state->TargetFbo == 0 || state->UniformBuffers[0] == 0 || state->UniformBuffers[1] == 0) {
        error = "OpenGL ES could not create frame objects";
        if (state->UniformBuffers[0] != 0) glDeleteBuffers(2, state->UniformBuffers);
        if (state->TargetFbo != 0) glDeleteFramebuffers(1, &state->TargetFbo);
        delete state;
        SDL_GL_DestroyContext(context);
        delete frame;
        return NULL;
    }

    frame->GpuDevice = reinterpret_cast<SDL_GPUDevice *>(frame);
    frame->Window = (SDL_Window *)window;
    frame->GlesState = state;
    frame->BackWidth = width;
    frame->BackHeight = height;
    frame->BatchDrawLimit = 0;
    frame->FlushLimit = 0;

    if (!frame->Create_Targets(width, height)) {
#if defined(__ANDROID__)
        appendAndroidDiagnostic( "GLES Create_Targets failed" );
#endif
        error = "OpenGL ES could not create the back/depth targets";
        delete frame;
        return NULL;
    }

    state->PresentProgram = CreatePresentProgram(state->PresentSampler);
    glGenVertexArrays(1, &state->PresentVao);
    if (state->PresentProgram == 0 || state->PresentVao == 0) {
#if defined(__ANDROID__)
        appendAndroidDiagnostic("GLES presenter program/VAO creation failed");
#endif
        error = "OpenGL ES could not initialize the Android surface presenter";
        delete frame;
        return NULL;
    }

    fprintf(stderr, "PosixDevice9: OpenGL ES %s / %s\n", version, vendor != NULL ? vendor : "unknown vendor");
#if defined(__ANDROID__)
    {
        char info[256];
        snprintf(info, sizeof(info), "GLES render targets created: %ux%u", width, height);
        appendAndroidDiagnostic(info);
    }
#endif
    return frame;
}

bool SdlGpuFrame::Create_Targets(unsigned int width, unsigned int height)
{
    GlesTexture *back = MakeColorTexture(width, height);
    GlesTexture *depth = MakeDepthTexture(width, height);
    GlesTexture *front = MakeColorTexture(width, height);
    if (back == NULL || depth == NULL || front == NULL) {
        if (back != NULL) Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(back));
        if (depth != NULL) Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(depth));
        if (front != NULL) Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(front));
        return false;
    }
    BackBuffer = reinterpret_cast<SDL_GPUTexture *>(back);
    DepthStencil = reinterpret_cast<SDL_GPUTexture *>(depth);
    FrontCopy = reinterpret_cast<SDL_GPUTexture *>(front);
    BackWidth = width;
    BackHeight = height;
    return true;
}

void SdlGpuFrame::Release_Targets()
{
    Gles_Delete_Texture(BackBuffer);
    Gles_Delete_Texture(DepthStencil);
    Gles_Delete_Texture(FrontCopy);
    BackBuffer = DepthStencil = FrontCopy = NULL;
    for (size_t i = 0; i < ScratchDepths.size(); ++i)
        Gles_Delete_Texture(ScratchDepths[i].Texture);
    ScratchDepths.clear();
    for (size_t i = 0; i < FeedbackCopies.size(); ++i)
        Gles_Delete_Texture(FeedbackCopies[i].Texture);
    FeedbackCopies.clear();
}

SdlGpuFrame::~SdlGpuFrame()
{
    Flush();
#if defined(__ANDROID__)
    GlesFrameState *state = State(this);
    if (state != NULL)
        SDL_GL_MakeCurrent(state->Window, state->Context);
#endif
    Release_Targets();
    if (StreamBuffer != NULL) {
        Gles_Delete_Buffer(StreamBuffer);
        StreamBuffer = NULL;
    }
#if defined(__ANDROID__)
    if (state != NULL) {
#if defined(__ANDROID__)
        {
            char diagnostic[256];
            snprintf(diagnostic, sizeof(diagnostic), "GLES Destroy frame id=%u", state->FrameId);
            appendAndroidDiagnostic(diagnostic);
        }
#endif
        if (state->PresentVao != 0) glDeleteVertexArrays(1, &state->PresentVao);
        if (state->PresentProgram != 0) glDeleteProgram(state->PresentProgram);
        if (state->UniformBuffers[0] != 0) glDeleteBuffers(2, state->UniformBuffers);
        if (state->TargetFbo != 0) glDeleteFramebuffers(1, &state->TargetFbo);
        SDL_GL_DestroyContext(state->Context);
        delete state;
        GlesState = NULL;
    }
#endif
}

bool SdlGpuFrame::Resize(unsigned int width, unsigned int height)
{
    if (width == 0 || height == 0) return false;
    if (!Flush()) return false;
    Release_Targets();
    return Create_Targets(width, height);
}

void SdlGpuFrame::Set_Target(const SdlTarget &target)
{
    CurrentTarget = target;
    TargetSet = true;
}

SdlTarget SdlGpuFrame::Back_Buffer_Target() const
{
    SdlTarget target = { BackBuffer, DepthStencil, BackWidth, BackHeight };
    return target;
}

SDL_GPUTexture *SdlGpuFrame::Depth_For(unsigned int width, unsigned int height)
{
    if (width == BackWidth && height == BackHeight) return DepthStencil;
    for (size_t i = 0; i < ScratchDepths.size(); ++i) {
        if (ScratchDepths[i].Width == width && ScratchDepths[i].Height == height)
            return ScratchDepths[i].Texture;
    }
    GlesTexture *depth = MakeDepthTexture(width, height);
    if (depth == NULL) return NULL;
    ScratchDepth scratch = { reinterpret_cast<SDL_GPUTexture *>(depth), width, height };
    ScratchDepths.push_back(scratch);
    return scratch.Texture;
}

SDL_GPUTexture *SdlGpuFrame::Feedback_Copy_For(unsigned int width, unsigned int height)
{
    if (width == 0 || height == 0) return NULL;
    for (size_t i = 0; i < FeedbackCopies.size(); ++i) {
        if (FeedbackCopies[i].Width == width && FeedbackCopies[i].Height == height)
            return FeedbackCopies[i].Texture;
    }
    // Keep one same-sized texture per target size, reusable across batches. The blit and the
    // composite draw are recorded in-order, so the snapshot is refreshed immediately before each
    // feedback draw and remains alive even if an intervening resource update flushes the batch.
    GlesTexture *copy = MakeColorTexture(width, height);
    if (copy == NULL) return NULL;
    FeedbackCopy scratch = { reinterpret_cast<SDL_GPUTexture *>(copy), width, height };
    FeedbackCopies.push_back(scratch);
    return scratch.Texture;
}

void SdlGpuFrame::Log_Feedback_Loop(SDL_GPUTexture *texture, SDL_GPUGraphicsPipeline *pipeline)
{
#if defined(__ANDROID__)
    static unsigned int reports = 0;
    if (reports >= 16) return;
    GlesFrameState *state = State(this);
    const GlesTexture *source = Gles_Texture(texture);
    const GlesPipeline *program = Gles_Pipeline(pipeline);
    char diagnostic[512];
    snprintf(diagnostic, sizeof(diagnostic),
        "GLES FEEDBACK LOOP: tex=%u fbo=%u prog=%u textureFbo=%u",
        source != NULL ? (unsigned)source->Name : 0u,
        state != NULL ? (unsigned)state->TargetFbo : 0u,
        program != NULL ? (unsigned)program->Program : 0u,
        source != NULL ? (unsigned)source->Fbo : 0u);
    appendAndroidDiagnostic(diagnostic);
    ++reports;
#else
    (void)texture;
    (void)pipeline;
#endif
}

void SdlGpuFrame::Record_Blit(SDL_GPUTexture *source, const int32_t source_rect[4], SDL_GPUTexture *destination,
    const int32_t destination_rect[4], bool linear)
{
    Command command;
    memset(&command, 0, sizeof(command));
    command.IsBlit = true;
    command.BlitSource = source;
    command.BlitDestination = destination;
    memcpy(command.Rect, source_rect, sizeof(command.Rect));
    memcpy(command.BlitRect, destination_rect, sizeof(command.BlitRect));
    command.BlitLinear = linear;
    command.Target = Target_Index();
    Commands.push_back(command);
}

void SdlGpuFrame::Clear_Back_Buffer(bool colour, bool depth, bool stencil, uint32_t argb, float z, uint32_t stencil_value)
{
    if (!colour && !depth && !stencil) return;
    Command command;
    memset(&command, 0, sizeof(command));
    command.Target = Target_Index();
    command.Colour = colour;
    command.Depth = depth;
    command.Stencil = stencil;
    command.Argb = argb;
    command.Z = z;
    command.StencilValue = stencil_value;
    Commands.push_back(command);
}

void SdlGpuFrame::Clear_Rect(int x, int y, int width, int height, bool colour, bool depth, bool stencil,
    uint32_t argb, float z, uint32_t stencil_value)
{
    if (width <= 0 || height <= 0 || (!colour && !depth && !stencil)) return;
    Command command;
    memset(&command, 0, sizeof(command));
    command.IsRect = true;
    command.Target = Target_Index();
    command.Rect[0] = x;
    command.Rect[1] = y;
    command.Rect[2] = width;
    command.Rect[3] = height;
    command.Colour = colour;
    command.Depth = depth;
    command.Stencil = stencil;
    command.Argb = argb;
    command.Z = z;
    command.StencilValue = stencil_value;
    Commands.push_back(command);
}

uint8_t *SdlGpuFrame::Stage(uint32_t size, uint32_t &offset)
{
    GrowBytes(StreamBytes, size, offset);
    return size == 0 ? NULL : &StreamBytes[offset].Value;
}

uint32_t SdlGpuFrame::Stage_Offset_Next() const
{
    return (uint32_t)((StreamBytes.size() + 15u) & ~(size_t)15u);
}

uint8_t *SdlGpuFrame::Upload_Space(uint32_t size, uint32_t &offset)
{
    GrowBytes(UploadBytes, size, offset);
    return size == 0 ? NULL : &UploadBytes[offset].Value;
}

void SdlGpuFrame::Queue_Buffer_Upload(SDL_GPUBuffer *, uint32_t, uint32_t) {}
void SdlGpuFrame::Queue_Texture_Upload(SDL_GPUTexture *, unsigned int, unsigned int, unsigned int, uint32_t) {}

uint32_t SdlGpuFrame::Constants(unsigned int stage, const void *bytes, uint32_t size)
{
    if (stage > 1) return 0;
    if (LastConstantsSize[stage] == size &&
        size != 0 && LastConstants[stage] + size <= ConstantBytes.size() &&
        memcmp(&ConstantBytes[LastConstants[stage]].Value, bytes, size) == 0)
        return LastConstants[stage];
    uint32_t offset = 0;
    GrowBytes(ConstantBytes, size, offset);
    if (size != 0) memcpy(&ConstantBytes[offset].Value, bytes, size);
    LastConstants[stage] = offset;
    LastConstantsSize[stage] = size;
    return offset;
}

void SdlGpuFrame::Record_Draw(const SdlRecordedDraw &draw)
{
    Command command;
    memset(&command, 0, sizeof(command));
    command.IsDraw = true;
    command.Draw = (uint32_t)Draws.size();
    command.Target = Target_Index();
    Draws.push_back(draw);
    Commands.push_back(command);
}

void SdlGpuFrame::Release_After_Batch(SDL_GPUTexture *texture, SDL_GPUBuffer *buffer)
{
    if (texture != NULL) Gles_Delete_Texture(texture);
    if (buffer != NULL) Gles_Delete_Buffer(buffer);
}

bool SdlGpuFrame::Batch_Is_Full()
{
    return StreamBytes.size() + UploadBytes.size() > (64u * 1024u * 1024u);
}

bool SdlGpuFrame::Flush()
{
    if (Commands.empty() && Uploads.empty()) return true;
    const bool ok = Gles_Replay();
    if (ok) {
        Commands.clear();
        Targets.clear();
        Draws.clear();
        StreamBytes.clear();
        UploadBytes.clear();
        ConstantBytes.clear();
        Uploads.clear();
        LastConstants[0] = LastConstants[1] = 0;
        LastConstantsSize[0] = LastConstantsSize[1] = 0;
        ++BatchNumber;
        ++FlushesTotal;
        ++Flushes;
    }
    return ok;
}

bool SdlGpuFrame::Present(const uint16_t (*ramp)[256])
{
    (void)ramp;
    if (!Flush()) {
#if defined(__ANDROID__)
        appendAndroidDiagnostic("GLES Present: Flush failed");
#endif
        return false;
    }
    GlesFrameState *state = State(this);
    GlesTexture *back = Gles_Texture(BackBuffer);
    GlesTexture *front = Gles_Texture(FrontCopy);
    if (state == NULL || state->Context == NULL || back == NULL || back->Fbo == 0) {
#if defined(__ANDROID__)
        appendAndroidDiagnostic("GLES Present: missing GL state/back buffer");
#endif
        return false;
    }

    if (!SDL_GL_MakeCurrent(state->Window, state->Context)) {
#if defined(__ANDROID__)
        char diagnostic[512];
        snprintf(diagnostic, sizeof(diagnostic), "GLES Present: MakeCurrent failed: %s", SDL_GetError());
        appendAndroidDiagnostic(diagnostic);
#endif
        return false;
    }

    // Presentation is a fresh framebuffer operation. Do not inherit the last draw's
    // scissor/depth/stencil/blend state when copying into Android's default framebuffer.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    (void)Copy_To_Front(NULL);

    int drawableWidth = 0, drawableHeight = 0;
    SDL_GetWindowSizeInPixels(state->Window, &drawableWidth, &drawableHeight);
    state->DrawableWidth = drawableWidth > 0 ? (unsigned)drawableWidth : BackWidth;
    state->DrawableHeight = drawableHeight > 0 ? (unsigned)drawableHeight : BackHeight;
    if (state->DrawableWidth == 0 || state->DrawableHeight == 0) {
#if defined(__ANDROID__)
        appendAndroidDiagnostic("GLES Present: drawable size is zero");
#endif
        return false;
    }

    // Drain stale errors from the engine's draw commands so the result below belongs
    // to this presentation operation, not an earlier draw.
    while (glGetError() != GL_NO_ERROR) {}

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)state->DrawableWidth, (GLsizei)state->DrawableHeight);
    glDisable(GL_CULL_FACE);
#ifdef GL_RASTERIZER_DISCARD
    glDisable(GL_RASTERIZER_DISCARD);
#endif

    // Sample the RGBA8 engine target into the window's actual EGL surface. A shader
    // presentation works across RGB/RGBA EGL configs and avoids GLES blit-format
    // restrictions that can leave a successful SwapWindow displaying only black.
    glUseProgram(state->PresentProgram);
    glBindVertexArray(state->PresentVao);
    glActiveTexture(GL_TEXTURE0);
    // The previous game draw may have left a mipmapped/repeating sampler object on
    // unit zero. Render targets have no mip chain, so force texture-owned sampling.
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, back->Name);
    glUniform1i(state->PresentSampler, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const GLenum shaderError = glGetError();
    const bool shaderPresented = shaderError == GL_NO_ERROR;

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);

    GLenum presentError = shaderError;
    const char *presentPath = "shader";
    if (!shaderPresented) {
        // Retain the former path as a best-effort fallback for unusual drivers.
        while (glGetError() != GL_NO_ERROR) {}
        glBindFramebuffer(GL_READ_FRAMEBUFFER, back->Fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, (GLint)BackWidth, (GLint)BackHeight,
            0, 0, (GLint)state->DrawableWidth, (GLint)state->DrawableHeight,
            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        presentError = glGetError();
        presentPath = "blit-fallback";
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)state->DrawableWidth, (GLsizei)state->DrawableHeight);
    const bool swapped = SDL_GL_SwapWindow(state->Window);
#if defined(__ANDROID__)
    {
        static unsigned int reports = 0;
        if (reports < 5 || !swapped || presentError != GL_NO_ERROR || !shaderPresented) {
            char diagnostic[768];
            snprintf(diagnostic, sizeof(diagnostic),
                "GLES Present: path=%s back=%ux%u drawable=%ux%u shaderError=0x%04X presentError=0x%04X swap=%s",
                presentPath, BackWidth, BackHeight, state->DrawableWidth, state->DrawableHeight,
                (unsigned)shaderError, (unsigned)presentError, swapped ? "OK" : "FAIL");
            appendAndroidDiagnostic(diagnostic);
            ++reports;
        }
    }
#endif
    (void)front;
    return presentError == GL_NO_ERROR && swapped;
}

void SdlGpuFrame::Take_Timing(double &flush_ms, double &fence_ms, unsigned int &flushes, double &acquire_ms,
    double &offscreen_ms, unsigned int &not_visible, unsigned int &not_shown)
{
    flush_ms = FlushMs;
    fence_ms = FenceMs;
    flushes = Flushes;
    acquire_ms = AcquireMs;
    offscreen_ms = OffscreenMs;
    not_visible = NotVisible;
    not_shown = NotShown;
    FlushMs = FenceMs = AcquireMs = OffscreenMs = 0.0;
    Flushes = NotVisible = NotShown = 0;
}

void SdlGpuFrame::Set_Offscreen_Presents(unsigned int hz)
{
    OffscreenPresents = true;
    OffscreenHz = hz;
}

bool SdlGpuFrame::Present_To(SDL_GPUTexture *target, unsigned int width, unsigned int height,
    const uint16_t (*ramp)[256])
{
    (void)ramp;
    if (!Flush() || width != BackWidth || height != BackHeight) return false;
    GlesTexture *source = Gles_Texture(BackBuffer);
    GlesTexture *destination = Gles_Texture(target);
    if (source == NULL || destination == NULL) return false;
    GLuint sourceTemp = 0, destinationTemp = 0;
    const GLuint sourceFbo = ReadFboFor(source, sourceTemp);
    const GLuint destinationFbo = ReadFboFor(destination, destinationTemp);
    if (sourceFbo == 0 || destinationFbo == 0) {
        DeleteTemporaryFbo(sourceTemp);
        DeleteTemporaryFbo(destinationTemp);
        return false;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFbo);
    glBlitFramebuffer(0, 0, (GLint)BackWidth, (GLint)BackHeight,
        0, 0, (GLint)width, (GLint)height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    DeleteTemporaryFbo(sourceTemp);
    DeleteTemporaryFbo(destinationTemp);
    return glGetError() == GL_NO_ERROR;
}

bool SdlGpuFrame::Read_Back(SDL_GPUTexture *texture, unsigned int width, unsigned int height, std::vector<uint8_t> &bgra)
{
    if (!Flush()) return false;
    GlesTexture *source = Gles_Texture(texture);
    if (source == NULL || source->Width < width || source->Height < height) return false;
    GLuint temporary = 0;
    const GLuint fbo = ReadFboFor(source, temporary);
    if (fbo == 0) {
        DeleteTemporaryFbo(temporary);
        return false;
    }
    bgra.resize((size_t)width * height * 4);
    std::vector<uint8_t> rgba(bgra.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
    if (glGetError() != GL_NO_ERROR) {
        DeleteTemporaryFbo(temporary);
        return false;
    }
    for (unsigned int y = 0; y < height; ++y) {
        const unsigned int sourceY = height - 1 - y;
        for (unsigned int x = 0; x < width; ++x) {
            const size_t s = ((size_t)sourceY * width + x) * 4;
            const size_t d = ((size_t)y * width + x) * 4;
            bgra[d + 0] = rgba[s + 2];
            bgra[d + 1] = rgba[s + 1];
            bgra[d + 2] = rgba[s + 0];
            bgra[d + 3] = rgba[s + 3];
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    DeleteTemporaryFbo(temporary);
    return true;
}

bool SdlGpuFrame::Upload_Back_Buffer(const std::vector<uint8_t> &bgra)
{
    if (bgra.size() != (size_t)BackWidth * BackHeight * 4) return false;
    std::vector<uint8_t> rgba(bgra.size());
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        rgba[i + 0] = bgra[i + 2];
        rgba[i + 1] = bgra[i + 1];
        rgba[i + 2] = bgra[i + 0];
        rgba[i + 3] = bgra[i + 3];
    }
    GlesTexture *back = Gles_Texture(BackBuffer);
    if (back == NULL) return false;
    glBindTexture(GL_TEXTURE_2D, back->Name);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)BackWidth, (GLsizei)BackHeight,
        GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
    return glGetError() == GL_NO_ERROR;
}

unsigned int SdlGpuFrame::Target_Format()
{
    return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
}

uint32_t SdlGpuFrame::Target_Index()
{
    const SdlTarget target = TargetSet ? CurrentTarget : Back_Buffer_Target();
    for (size_t i = 0; i < Targets.size(); ++i) {
        if (Targets[i].Colour == target.Colour && Targets[i].Depth == target.Depth)
            return (uint32_t)i;
    }
    Targets.push_back(target);
    return (uint32_t)(Targets.size() - 1);
}

bool SdlGpuFrame::Submit(struct SDL_GPUCommandBuffer *) { return true; }
bool SdlGpuFrame::Submit_Offscreen(struct SDL_GPUCommandBuffer *) { return true; }
bool SdlGpuFrame::Submit_Flush(struct SDL_GPUCommandBuffer *) { return true; }
bool SdlGpuFrame::Record_Batch(struct SDL_GPUCommandBuffer *) { return true; }
bool SdlGpuFrame::Record_Passes(struct SDL_GPUCommandBuffer *, SDL_GPUBuffer *) { return true; }
bool SdlGpuFrame::Begin_Pass(struct SDL_GPUCommandBuffer *, const SdlTarget &, bool, bool, bool, uint32_t, float, uint32_t,
    struct SDL_GPURenderPass **) { return false; }
SDL_GPUGraphicsPipeline *SdlGpuFrame::Clear_Pipeline(unsigned int) { return NULL; }
void SdlGpuFrame::End_Batch() {}
bool SdlGpuFrame::Present_Into(struct SDL_GPUCommandBuffer *, SDL_GPUTexture *, unsigned int, unsigned int,
    unsigned int, const uint16_t (*)[256]) { return false; }
SDL_GPUGraphicsPipeline *SdlGpuFrame::Gamma_Pipeline(unsigned int) { return NULL; }
bool SdlGpuFrame::Upload_Ramp(struct SDL_GPUCommandBuffer *, const uint16_t (*)[256]) { return true; }
bool SdlGpuFrame::Copy_To_Front(struct SDL_GPUCommandBuffer *)
{
    GlesFrameState *state = State(this);
    GlesTexture *back = Gles_Texture(BackBuffer);
    GlesTexture *front = Gles_Texture(FrontCopy);
    if (state == NULL || back == NULL || front == NULL || back->Fbo == 0 || front->Fbo == 0) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, back->Fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, front->Fbo);
    glBlitFramebuffer(0, 0, (GLint)BackWidth, (GLint)BackHeight,
        0, 0, (GLint)BackWidth, (GLint)BackHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

bool Sdl_Gamma_Is_Identity(const uint16_t (*ramp)[256])
{
    if (ramp == NULL) return true;
    for (int channel = 0; channel < 3; ++channel)
        for (int i = 0; i < 256; ++i)
            if (ramp[channel][i] != (uint16_t)(i * 257)) return false;
    return true;
}