/*
** Copyright 2026 CnCGeneralsZH-Reforged contributors.
** Licensed under GPL-3.0-or-later; see LICENSE.md.
**
** Android OpenGL ES backend handles.  These are deliberately stored behind the existing SDL_GPU
** opaque pointer types so PosixDevice9 and WW3D2 keep their D3D9-shaped API unchanged.
*/
#pragma once

#include "SdlPipelineCache.h"

#include <GLES3/gl3.h>

#include <stdint.h>
#include <utility>
#include <string>
#include <vector>

struct GlesShader
{
    GLuint Name;
    bool Vertex;
    unsigned SamplerSlots;
    unsigned UniformBuffers;
    std::vector<std::pair<std::string, unsigned> > Samplers;
    std::vector<std::pair<std::string, unsigned> > UniformBlocks;
};

struct GlesPipeline
{
    GLuint Program;
    SdlVertexLayout Layout;
    GLenum Primitive;
    uint8_t Fill;
    uint8_t Cull;
    uint8_t BlendEnable;
    uint8_t SourceColour;
    uint8_t DestinationColour;
    uint8_t ColourOperation;
    uint8_t SourceAlpha;
    uint8_t DestinationAlpha;
    uint8_t AlphaOperation;
    uint8_t WriteMask;
    uint8_t DepthTest;
    uint8_t DepthWrite;
    uint8_t DepthCompare;
    uint8_t StencilEnable;
    uint8_t StencilRead;
    uint8_t StencilWrite;
    uint8_t FrontFail;
    uint8_t FrontDepthFail;
    uint8_t FrontPass;
    uint8_t FrontCompare;
    uint8_t BackFail;
    uint8_t BackDepthFail;
    uint8_t BackPass;
    uint8_t BackCompare;
    float DepthBias;
    float SlopeBias;
};

struct GlesSampler
{
    GLuint Name;
};

struct GlesBuffer
{
    GLuint Name;
    GLenum Target;
    uint32_t Size;
};

struct GlesTexture
{
    GLuint Name;
    GLuint Fbo;
    uint32_t Width;
    uint32_t Height;
    bool Depth;
    bool RenderTarget;
};

inline GlesShader *Gles_Shader(SDL_GPUShader *p) { return reinterpret_cast<GlesShader *>(p); }
inline const GlesShader *Gles_Shader(const SDL_GPUShader *p) { return reinterpret_cast<const GlesShader *>(p); }
inline GlesPipeline *Gles_Pipeline(SDL_GPUGraphicsPipeline *p) { return reinterpret_cast<GlesPipeline *>(p); }
inline const GlesPipeline *Gles_Pipeline(const SDL_GPUGraphicsPipeline *p) { return reinterpret_cast<const GlesPipeline *>(p); }
inline GlesSampler *Gles_Sampler(SDL_GPUSampler *p) { return reinterpret_cast<GlesSampler *>(p); }
inline GlesTexture *Gles_Texture(SDL_GPUTexture *p) { return reinterpret_cast<GlesTexture *>(p); }
inline const GlesTexture *Gles_Texture(const SDL_GPUTexture *p) { return reinterpret_cast<const GlesTexture *>(p); }
inline GlesBuffer *Gles_Buffer(SDL_GPUBuffer *p) { return reinterpret_cast<GlesBuffer *>(p); }
inline const GlesBuffer *Gles_Buffer(const SDL_GPUBuffer *p) { return reinterpret_cast<const GlesBuffer *>(p); }

bool Gles_Compile_SPIRV_To_GLSLES(const std::vector<unsigned char> &spirv, bool vertex_stage,
    std::string &source, std::string &log, std::vector<std::pair<std::string, unsigned> > *samplers = NULL,
    std::vector<std::pair<std::string, unsigned> > *uniform_blocks = NULL);
bool Gles_Compile_Shader(GLenum type, const std::string &source, GLuint &shader, std::string &log);

void Gles_Delete_Texture(SDL_GPUTexture *texture);
void Gles_Delete_Buffer(SDL_GPUBuffer *buffer);
