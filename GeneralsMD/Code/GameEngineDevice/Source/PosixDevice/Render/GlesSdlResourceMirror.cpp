/*
**	Copyright 2026 İlyas Akın
**	Additional terms under GNU GPL section 7 apply: see LICENSE.md.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GPU copies of A2's textures and buffers (decision 7, phase A3c).  See SdlResourceMirror.h.

#include "SdlResourceMirror.h"
#include "SdlCreationLog.h"
#include "PosixPixelCodec.h"
#include "PosixResources9.h"
#include "SdlGpuFrame.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SdlResourceMirrors * LiveMirrors = NULL;

// A development aid until A3d's capture: ZH_GPU_DUMP_TEXTURES=<dir> writes level 0 of every texture at
// least 256 wide, decoded to RGB, and its alpha as a second picture, each time it goes up.
static void dump_texture_if_asked(const PosixImage & image, const void * owner, unsigned int upload)
{
	const char * dir = getenv("ZH_GPU_DUMP_TEXTURES");
	if (dir == NULL || image.width() < 256) {
		return;
	}
	std::vector<uint8_t> bgra((size_t)image.width() * image.height() * 4);
	if (!Sdl_Convert_Level(image, false, &bgra[0])) {
		return;
	}
	for (int alpha = 0; alpha < 2; ++alpha) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/tex_%p_%u_%ux%u_fmt%u%s.ppm", dir, owner, upload, image.width(), image.height(),
			(unsigned int)image.format(), alpha ? "_alpha" : "");
		FILE * file = fopen(path, "wb");
		if (file == NULL) {
			return;
		}
		fprintf(file, "P6\n%u %u\n255\n", image.width(), image.height());
		for (size_t i = 0; i < bgra.size(); i += 4) {
			const uint8_t rgb[3] = { alpha ? bgra[i + 3] : bgra[i + 2], alpha ? bgra[i + 3] : bgra[i + 1],
				alpha ? bgra[i + 3] : bgra[i] };
			fwrite(rgb, 1, 3, file);
		}
		fclose(file);
	}
}

bool Sdl_Texture_Format(D3DFORMAT format, unsigned int width, unsigned int height, bool bc_ok,
	unsigned int & sdl_format, bool & native)
{
	const bool whole_blocks = width % 4 == 0 && height % 4 == 0;
	native = true;
	switch (format) {
		case D3DFMT_A8R8G8B8:
			sdl_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
			return true;
		case D3DFMT_DXT1:
			sdl_format = SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
			break;
		// DXT2 and DXT4 are the premultiplied forms.  The blocks are the same and D3D9 samples them as
		// stored, so they go up as DXT3 and DXT5 do.
		case D3DFMT_DXT2:
		case D3DFMT_DXT3:
			sdl_format = SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
			break;
		case D3DFMT_DXT4:
		case D3DFMT_DXT5:
			sdl_format = SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
			break;
		default:
			native = false;
			break;
	}
	if (native && bc_ok && whole_blocks) {
		return true;
	}
	native = false;
	sdl_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	return posixCanDecode(format);
}

uint32_t Sdl_Level_Upload_Size(const PosixImage & image, bool native)
{
	return native ? (uint32_t)image.slicePitch() : (uint32_t)(image.width() * image.height() * 4);
}

static uint8_t unit_to_byte(float value)
{
	if (!(value > 0.0f)) return 0;
	if (value >= 1.0f) return 255;
	return (uint8_t)(value * 255.0f + 0.5f);
}

bool Sdl_Convert_Level(const PosixImage & image, bool native, uint8_t * out)
{
	const uint8_t * bytes = image.bytes();
	if (bytes == NULL) {
		return false;
	}
	if (native) {
		memcpy(out, bytes, image.slicePitch());
		return true;
	}
	const unsigned int width = image.width();
	const unsigned int height = image.height();
	if (image.format() == D3DFMT_X8R8G8B8) {
		// The bytes are B, G, R and an unused X, which D3D9 samples as alpha 1.
		for (unsigned int y = 0; y < height; ++y) {
			const uint8_t * row = bytes + (size_t)y * image.rowPitch();
			uint8_t * target = out + (size_t)y * width * 4;
			memcpy(target, row, (size_t)width * 4);
			for (unsigned int x = 0; x < width; ++x) {
				target[x * 4 + 3] = 255;
			}
		}
		return true;
	}
	const PosixFormatLayout & layout = image.layout();
	PosixColor block[16];
	if (layout.blockWidth * layout.blockHeight > 16) {
		return false;
	}
	for (unsigned int block_y = 0; block_y < image.rowCount(); ++block_y) {
		const uint8_t * row = bytes + (size_t)block_y * image.rowPitch();
		for (unsigned int block_x = 0; block_x * layout.blockWidth < width; ++block_x) {
			if (!posixDecodeBlock(image.format(), row + (size_t)block_x * layout.bytesPerBlock, block)) {
				return false;
			}
			for (unsigned int j = 0; j < layout.blockHeight; ++j) {
				const unsigned int y = block_y * layout.blockHeight + j;
				for (unsigned int i = 0; i < layout.blockWidth; ++i) {
					const unsigned int x = block_x * layout.blockWidth + i;
					if (x >= width || y >= height) {
						continue;
					}
					const PosixColor & colour = block[j * layout.blockWidth + i];
					uint8_t * pixel = out + ((size_t)y * width + x) * 4;
					pixel[0] = unit_to_byte(colour.b);
					pixel[1] = unit_to_byte(colour.g);
					pixel[2] = unit_to_byte(colour.r);
					pixel[3] = unit_to_byte(colour.a);
				}
			}
		}
	}
	return true;
}


#include "GlesRenderCommon.h"

static SdlResourceMirrors *LiveGlesMirrors = NULL;

static GlesTexture *Gles_Create_Color_Texture(unsigned int width, unsigned int height, bool render_target)
{
    GlesTexture *texture = new GlesTexture();
    texture->Name = 0;
    texture->Fbo = 0;
    texture->Width = width;
    texture->Height = height;
    texture->Depth = false;
    texture->RenderTarget = render_target;
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (render_target) {
        glGenFramebuffers(1, &texture->Fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, texture->Fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->Name, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "GlesResourceMirror: color framebuffer incomplete (0x%x)\\n", (unsigned)status);
            Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(texture));
            return NULL;
        }
    }
    return texture;
}

static GlesTexture *Gles_Create_Depth_Texture(unsigned int width, unsigned int height)
{
    GlesTexture *texture = new GlesTexture();
    texture->Name = 0;
    texture->Fbo = 0;
    texture->Width = width;
    texture->Height = height;
    texture->Depth = true;
    texture->RenderTarget = true;
    glGenTextures(1, &texture->Name);
    glGenFramebuffers(1, &texture->Fbo);
    if (texture->Name == 0 || texture->Fbo == 0) {
        Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(texture));
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, texture->Name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)width, (GLsizei)height, 0,
        GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, texture->Fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture->Name, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GlesResourceMirror: depth framebuffer incomplete (0x%x)\\n", (unsigned)status);
        Gles_Delete_Texture(reinterpret_cast<SDL_GPUTexture *>(texture));
        return NULL;
    }
    return texture;
}

static bool Gles_Upload_Level(GlesTexture *texture, const PosixImage &image)
{
    std::vector<uint8_t> bgra((size_t)image.width() * image.height() * 4);
    if (!Sdl_Convert_Level(image, false, bgra.empty() ? NULL : &bgra[0])) {
        return false;
    }
    std::vector<uint8_t> rgba(bgra.size());
    const unsigned int width = image.width();
    const unsigned int height = image.height();
    for (unsigned int y = 0; y < height; ++y) {
        const unsigned int source_y = height - 1 - y;
        for (unsigned int x = 0; x < width; ++x) {
            const size_t s = ((size_t)source_y * width + x) * 4;
            const size_t d = ((size_t)y * width + x) * 4;
            rgba[d + 0] = bgra[s + 2];
            rgba[d + 1] = bgra[s + 1];
            rgba[d + 2] = bgra[s + 0];
            rgba[d + 3] = bgra[s + 3];
        }
    }
    glBindTexture(GL_TEXTURE_2D, texture->Name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)image.width(), (GLsizei)image.height(), 0,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba.empty() ? NULL : &rgba[0]);
    return glGetError() == GL_NO_ERROR;
}

static bool Gles_Upload_LevelAt(GlesTexture *texture, unsigned int level, const PosixImage &image)
{
    std::vector<uint8_t> bgra((size_t)image.width() * image.height() * 4);
    if (!Sdl_Convert_Level(image, false, bgra.empty() ? NULL : &bgra[0])) return false;
    std::vector<uint8_t> rgba(bgra.size());
    const unsigned int width = image.width();
    const unsigned int height = image.height();
    for (unsigned int y = 0; y < height; ++y) {
        const unsigned int source_y = height - 1 - y;
        for (unsigned int x = 0; x < width; ++x) {
            const size_t s = ((size_t)source_y * width + x) * 4;
            const size_t d = ((size_t)y * width + x) * 4;
            rgba[d + 0] = bgra[s + 2];
            rgba[d + 1] = bgra[s + 1];
            rgba[d + 2] = bgra[s + 0];
            rgba[d + 3] = bgra[s + 3];
        }
    }
    glBindTexture(GL_TEXTURE_2D, texture->Name);
    glTexImage2D(GL_TEXTURE_2D, (GLint)level, GL_RGBA8, (GLsizei)image.width(), (GLsizei)image.height(), 0,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba.empty() ? NULL : &rgba[0]);
    return glGetError() == GL_NO_ERROR;
}

void Gles_Delete_Texture(SDL_GPUTexture *opaque)
{
    if (opaque == NULL) return;
    GlesTexture *texture = Gles_Texture(opaque);
    if (texture->Fbo != 0) glDeleteFramebuffers(1, &texture->Fbo);
    if (texture->Name != 0) glDeleteTextures(1, &texture->Name);
    delete texture;
}

void Gles_Delete_Buffer(SDL_GPUBuffer *opaque)
{
    if (opaque == NULL) return;
    GlesBuffer *buffer = Gles_Buffer(opaque);
    if (buffer->Name != 0) glDeleteBuffers(1, &buffer->Name);
    delete buffer;
}

SdlResourceMirrors::SdlResourceMirrors(SdlGpuFrame *frame) :
    Frame(frame),
    WhiteTexture(NULL),
    BcSupported(0),
    StaleFlushes(0),
    TexturesUploaded(0),
    BuffersUploaded(0)
{
    LiveGlesMirrors = this;
    posixResourceDestroyed = &SdlResourceMirrors::Destroyed;
}

SdlResourceMirrors::~SdlResourceMirrors()
{
    posixResourceDestroyed = NULL;
    LiveGlesMirrors = NULL;
    std::vector<Copy> dead;
    {
        std::lock_guard<std::mutex> guard(Lock);
        for (std::unordered_map<const void *, Copy>::iterator it = Copies.begin(); it != Copies.end(); ++it)
            dead.push_back(it->second);
        Copies.clear();
    }
    for (size_t i = 0; i < dead.size(); ++i) Frame->Release_After_Batch(dead[i].Texture, dead[i].Buffer);
    Frame->Release_After_Batch(WhiteTexture, NULL);
}

void SdlResourceMirrors::Destroyed(const void *resource)
{
    SdlResourceMirrors *mirrors = LiveGlesMirrors;
    if (mirrors == NULL) return;
    std::lock_guard<std::mutex> guard(mirrors->Lock);
    std::unordered_map<const void *, Copy>::iterator it = mirrors->Copies.find(resource);
    if (it != mirrors->Copies.end()) {
        mirrors->Dead.push_back(it->second);
        mirrors->Copies.erase(it);
    }
}

void SdlResourceMirrors::Collect_Dead()
{
    std::vector<Copy> dead;
    {
        std::lock_guard<std::mutex> guard(Lock);
        dead.swap(Dead);
    }
    for (size_t i = 0; i < dead.size(); ++i) Frame->Release_After_Batch(dead[i].Texture, dead[i].Buffer);
}

bool SdlResourceMirrors::Has_Copy(const void *owner) const
{
    std::lock_guard<std::mutex> guard(Lock);
    return Copies.find(owner) != Copies.end();
}

size_t SdlResourceMirrors::Live_Copies() const
{
    std::lock_guard<std::mutex> guard(Lock);
    return Copies.size();
}

void SdlResourceMirrors::Prepare_Update(Copy &copy)
{
    if (copy.UsedBatch == Frame->Batch()) {
        ++StaleFlushes;
        Frame->Flush();
    }
}

SDL_GPUTexture *SdlResourceMirrors::Texture(IDirect3DBaseTexture9 *base, std::string &refusal)
{
    if (base == NULL || base->GetType() != D3DRTYPE_TEXTURE) {
        refusal = "a cube or volume texture (GLES draws 2D textures only)";
        return NULL;
    }
    PosixTexture9 *texture = static_cast<PosixTexture9 *>(base);
    const unsigned int levels = texture->levelCount();
    if (levels == 0) {
        refusal = "a texture with no levels";
        return NULL;
    }

    std::lock_guard<std::mutex> guard(Lock);
    std::unordered_map<const void *, Copy>::iterator found = Copies.find(texture);
    bool created = false;
    if (found == Copies.end()) {
        D3DSURFACE_DESC desc;
        if (texture->GetLevelDesc(0, &desc) != D3D_OK) {
            refusal = "a texture with no surface description";
            return NULL;
        }
        const bool render_target = (desc.Usage & D3DUSAGE_RENDERTARGET) != 0;
        if (!posixCanDecode(texture->level(0).format())) {
            refusal = "a texture format the GLES upload path cannot decode";
            return NULL;
        }
        GlesTexture *gpu = Gles_Create_Color_Texture(texture->level(0).width(), texture->level(0).height(), render_target);
        if (gpu == NULL) {
            refusal = "OpenGL ES could not create the texture";
            return NULL;
        }
        Copy copy;
        memset(&copy, 0, sizeof(copy));
        copy.Texture = reinterpret_cast<SDL_GPUTexture *>(gpu);
        copy.Native = false;
        copy.UsedBatch = 0;
        copy.Versions.resize(levels);
        for (unsigned int level = 0; level < levels; ++level) {
            copy.Versions[level] = render_target ? texture->level(level).version() : 0;
        }
        found = Copies.insert(std::make_pair((const void *)texture, copy)).first;
        created = true;
    }
    Copy &copy = found->second;

    bool stale = created || copy.Versions.size() != levels;
    for (unsigned int level = 0; !stale && level < levels; ++level)
        stale = copy.Versions[level] != texture->level(level).version();

    if (stale) {
        Prepare_Update(copy);
        GlesTexture *gpu = Gles_Texture(copy.Texture);
        bool ok = true;
        for (unsigned int level = 0; level < levels; ++level) {
            ok = ok && Gles_Upload_LevelAt(gpu, level, texture->level(level));
            copy.Versions[level] = texture->level(level).version();
        }
        if (!ok) {
            refusal = "OpenGL ES texture upload failed";
            return NULL;
        }
        ++TexturesUploaded;
    }
    copy.UsedBatch = Frame->Batch();
    (void)created;
    return copy.Texture;
}

SDL_GPUTexture *SdlResourceMirrors::Surface(IDirect3DSurface9 *surface, bool depth, std::string &refusal)
{
    if (surface == NULL) {
        refusal = "a null surface";
        return NULL;
    }
    D3DSURFACE_DESC desc;
    if (surface->GetDesc(&desc) != D3D_OK) {
        refusal = "a surface with no description";
        return NULL;
    }
    std::lock_guard<std::mutex> guard(Lock);
    std::unordered_map<const void *, Copy>::iterator found = Copies.find(surface);
    if (found != Copies.end()) return found->second.Texture;

    GlesTexture *gpu = depth ? Gles_Create_Depth_Texture(desc.Width, desc.Height)
        : Gles_Create_Color_Texture(desc.Width, desc.Height, true);
    if (gpu == NULL) {
        refusal = "OpenGL ES could not create the surface";
        return NULL;
    }
    Copy copy;
    memset(&copy, 0, sizeof(copy));
    copy.Texture = reinterpret_cast<SDL_GPUTexture *>(gpu);
    copy.Native = true;
    copy.UsedBatch = 0;
    Copies.insert(std::make_pair((const void *)surface, copy));
    return copy.Texture;
}

SDL_GPUBuffer *SdlResourceMirrors::Buffer(const void *owner, const PosixBufferStorage &storage, std::string &refusal)
{
    const uint32_t size = (storage.length() + 3u) & ~3u;
    if (size == 0) {
        refusal = "an empty buffer";
        return NULL;
    }
    std::lock_guard<std::mutex> guard(Lock);
    std::unordered_map<const void *, Copy>::iterator found = Copies.find(owner);
    bool created = false;
    if (found == Copies.end()) {
        GlesBuffer *gpu = new GlesBuffer();
        gpu->Name = 0;
        gpu->Target = GL_ARRAY_BUFFER;
        gpu->Size = size;
        glGenBuffers(1, &gpu->Name);
        if (gpu->Name == 0) {
            delete gpu;
            refusal = "glGenBuffers failed";
            return NULL;
        }
        glBindBuffer(GL_ARRAY_BUFFER, gpu->Name);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, NULL, GL_STATIC_DRAW);
        Copy copy;
        memset(&copy, 0, sizeof(copy));
        copy.Buffer = reinterpret_cast<SDL_GPUBuffer *>(gpu);
        copy.Size = size;
        copy.Native = true;
        copy.UsedBatch = 0;
        found = Copies.insert(std::make_pair(owner, copy)).first;
        created = true;
    }
    Copy &copy = found->second;
    if (copy.Versions.size() != 1 || copy.Versions[0] != storage.version()) {
        Prepare_Update(copy);
        GlesBuffer *gpu = Gles_Buffer(copy.Buffer);
        glBindBuffer(GL_ARRAY_BUFFER, gpu->Name);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)copy.Size, NULL, GL_STATIC_DRAW);
        if (storage.length() != 0)
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)storage.length(), storage.bytes());
        if (storage.length() < copy.Size) {
            std::vector<uint8_t> zeros(copy.Size - (uint32_t)storage.length(), 0);
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)storage.length(), (GLsizeiptr)zeros.size(), &zeros[0]);
        }
        if (glGetError() != GL_NO_ERROR) {
            refusal = "OpenGL ES buffer upload failed";
            return NULL;
        }
        copy.Versions.assign(1, storage.version());
        ++BuffersUploaded;
    }
    copy.UsedBatch = Frame->Batch();
    (void)created;
    return copy.Buffer;
}

SDL_GPUTexture *SdlResourceMirrors::White()
{
    if (WhiteTexture != NULL) return WhiteTexture;
    GlesTexture *gpu = Gles_Create_Color_Texture(1, 1, false);
    if (gpu == NULL) return NULL;
    const uint8_t pixel[4] = {255,255,255,255};
    glBindTexture(GL_TEXTURE_2D, gpu->Name);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    WhiteTexture = reinterpret_cast<SDL_GPUTexture *>(gpu);
    return WhiteTexture;
}
