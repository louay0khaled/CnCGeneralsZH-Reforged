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

// Pipelines and samplers on SDL3 GPU (decision 7, phase A3c).  See SdlPipelineCache.h.

#include "SdlPipelineCache.h"
#include "Common/CrashHandler.h"
#include "SdlCreationLog.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

//-------------------------------------------------------------------------------------------------
// The vertex layout.
//-------------------------------------------------------------------------------------------------

static void add_attribute(SdlVertexLayout &layout, unsigned int location, SDL_GPUVertexElementFormat format, unsigned int offset)
{
	const unsigned int at = layout.AttributeCount++;
	layout.Location[at] = location;
	layout.Format[at] = format;
	layout.Offset[at] = offset;
}

bool Sdl_Vertex_Layout(RenderUInt32 fvf, SdlVertexLayout &layout, std::string &refusal)
{
	memset(&layout, 0, sizeof(layout));
	unsigned int offset = 0;
	const RenderUInt32 position = fvf & D3DFVF_POSITION_MASK;
	if (position == D3DFVF_XYZ) {
		add_attribute(layout, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offset);
		offset += 12;
	}
	else if (position == D3DFVF_XYZRHW) {
		// Four floats, all read: the pretransformed program takes w from the vertex's RHW, which is
		// what makes its colours and coordinates interpolate with perspective.
		add_attribute(layout, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offset);
		offset += 16;
	}
	else {
		refusal = "a blend-weighted position (vertex blending)";
		return false;
	}
	if (fvf & D3DFVF_NORMAL) {
		add_attribute(layout, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offset);
		offset += 12;
	}
	if (fvf & D3DFVF_PSIZE) {
		offset += 4;	// not read: point sizes are refused elsewhere
	}
	if (fvf & D3DFVF_DIFFUSE) {
		add_attribute(layout, 2, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offset);
		offset += 4;
	}
	if (fvf & D3DFVF_SPECULAR) {
		// COLOR1, a D3DCOLOR like the diffuse, which the program swaps the same way.
		add_attribute(layout, 3, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offset);
		offset += 4;
	}
	const unsigned int sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	static const SDL_GPUVertexElementFormat FLOATS[4] = { SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,
		SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4 };
	for (unsigned int set = 0; set < sets; ++set) {
		// D3DFVF_TEXCOORDSIZEn: two bits per set, 0 two floats, 1 three, 2 four, 3 one.
		static const unsigned int COUNT_OF_BITS[4] = { 2, 3, 4, 1 };
		const unsigned int floats = COUNT_OF_BITS[(fvf >> (16 + set * 2)) & 3];
		// The program declares the first four sets, as two floats: a wider set gives its first two, a
		// one-float set its first and a zero.
		if (set < 4) {
			add_attribute(layout, 4 + set, FLOATS[floats - 1], offset);
		}
		offset += floats * 4;
	}
	layout.Stride = offset;
	return true;
}

//-------------------------------------------------------------------------------------------------
// The pipeline key.
//-------------------------------------------------------------------------------------------------

static bool blend_factor(RenderUInt32 value, uint8_t &out)
{
	switch (value) {
		case D3DBLEND_ZERO:				out = SDL_GPU_BLENDFACTOR_ZERO; return true;
		case D3DBLEND_ONE:				out = SDL_GPU_BLENDFACTOR_ONE; return true;
		case D3DBLEND_SRCCOLOR:			out = SDL_GPU_BLENDFACTOR_SRC_COLOR; return true;
		case D3DBLEND_INVSRCCOLOR:		out = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR; return true;
		case D3DBLEND_SRCALPHA:			out = SDL_GPU_BLENDFACTOR_SRC_ALPHA; return true;
		case D3DBLEND_INVSRCALPHA:		out = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; return true;
		case D3DBLEND_DESTALPHA:		out = SDL_GPU_BLENDFACTOR_DST_ALPHA; return true;
		case D3DBLEND_INVDESTALPHA:		out = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA; return true;
		case D3DBLEND_DESTCOLOR:		out = SDL_GPU_BLENDFACTOR_DST_COLOR; return true;
		case D3DBLEND_INVDESTCOLOR:		out = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR; return true;
		case D3DBLEND_SRCALPHASAT:		out = SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE; return true;
		case D3DBLEND_BLENDFACTOR:		out = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR; return true;
		case D3DBLEND_INVBLENDFACTOR:	out = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR; return true;
		default: return false;
	}
}

static bool blend_operation(RenderUInt32 value, uint8_t &out)
{
	switch (value) {
		case D3DBLENDOP_ADD:			out = SDL_GPU_BLENDOP_ADD; return true;
		case D3DBLENDOP_SUBTRACT:		out = SDL_GPU_BLENDOP_SUBTRACT; return true;
		case D3DBLENDOP_REVSUBTRACT:	out = SDL_GPU_BLENDOP_REVERSE_SUBTRACT; return true;
		case D3DBLENDOP_MIN:			out = SDL_GPU_BLENDOP_MIN; return true;
		case D3DBLENDOP_MAX:			out = SDL_GPU_BLENDOP_MAX; return true;
		default: return false;
	}
}

// A source and destination pair, with D3D9's two shorthands that set both from the source state.
static bool blend_pair(RenderUInt32 source, RenderUInt32 destination, uint8_t &source_out, uint8_t &destination_out)
{
	if (source == D3DBLEND_BOTHSRCALPHA) {
		source_out = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		destination_out = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		return true;
	}
	if (source == D3DBLEND_BOTHINVSRCALPHA) {
		source_out = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		destination_out = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		return true;
	}
	return blend_factor(source, source_out) && blend_factor(destination, destination_out);
}

static bool compare_operation(RenderUInt32 value, uint8_t &out)
{
	switch (value) {
		case D3DCMP_NEVER:			out = SDL_GPU_COMPAREOP_NEVER; return true;
		case D3DCMP_LESS:			out = SDL_GPU_COMPAREOP_LESS; return true;
		case D3DCMP_EQUAL:			out = SDL_GPU_COMPAREOP_EQUAL; return true;
		case D3DCMP_LESSEQUAL:		out = SDL_GPU_COMPAREOP_LESS_OR_EQUAL; return true;
		case D3DCMP_GREATER:		out = SDL_GPU_COMPAREOP_GREATER; return true;
		case D3DCMP_NOTEQUAL:		out = SDL_GPU_COMPAREOP_NOT_EQUAL; return true;
		case D3DCMP_GREATEREQUAL:	out = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL; return true;
		case D3DCMP_ALWAYS:			out = SDL_GPU_COMPAREOP_ALWAYS; return true;
		default: return false;
	}
}

static bool stencil_operation(RenderUInt32 value, uint8_t &out)
{
	switch (value) {
		case D3DSTENCILOP_KEEP:		out = SDL_GPU_STENCILOP_KEEP; return true;
		case D3DSTENCILOP_ZERO:		out = SDL_GPU_STENCILOP_ZERO; return true;
		case D3DSTENCILOP_REPLACE:	out = SDL_GPU_STENCILOP_REPLACE; return true;
		case D3DSTENCILOP_INCRSAT:	out = SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP; return true;
		case D3DSTENCILOP_DECRSAT:	out = SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP; return true;
		case D3DSTENCILOP_INVERT:	out = SDL_GPU_STENCILOP_INVERT; return true;
		case D3DSTENCILOP_INCR:		out = SDL_GPU_STENCILOP_INCREMENT_AND_WRAP; return true;
		case D3DSTENCILOP_DECR:		out = SDL_GPU_STENCILOP_DECREMENT_AND_WRAP; return true;
		default: return false;
	}
}

static float float_of(RenderUInt32 bits)
{
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

// D3D9 measures the depth bias in depth values and SDL, as D3D11, in units of the depth buffer's
// resolution: 2^-24 for D24, and for D32F the same wherever the depth is in [0.5, 1), which is where
// a perspective scene's depths are.  dx11state.cpp scales the same way.
static const float DEPTH_BIAS_UNIT_SCALE = 16777216.0f;

bool Sdl_Pipeline_Key(const RenderUInt32 rs[256], D3DPRIMITIVETYPE primitive, SDL_GPUShader *vertex_shader,
	SDL_GPUShader *pixel_shader, RenderUInt32 fvf, uint32_t colour_format, uint32_t depth_format, SdlPipelineKey &key,
	std::string &refusal)
{
	memset(&key, 0, sizeof(key));
	key.VertexShader = vertex_shader;
	key.PixelShader = pixel_shader;
	key.FVF = fvf;
	key.ColourFormat = colour_format;
	key.DepthFormat = depth_format;

	switch (primitive) {
		case D3DPT_POINTLIST:		key.Primitive = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
		case D3DPT_LINELIST:		key.Primitive = SDL_GPU_PRIMITIVETYPE_LINELIST; break;
		case D3DPT_LINESTRIP:		key.Primitive = SDL_GPU_PRIMITIVETYPE_LINESTRIP; break;
		case D3DPT_TRIANGLELIST:	key.Primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;
		case D3DPT_TRIANGLESTRIP:	key.Primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP; break;
		case D3DPT_TRIANGLEFAN:		key.Primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; break;	// expanded by the draw
		default: refusal = "an unknown primitive type"; return false;
	}

	switch (rs[D3DRS_FILLMODE]) {
		case D3DFILL_SOLID:			key.Fill = SDL_GPU_FILLMODE_FILL; break;
		case D3DFILL_WIREFRAME:		key.Fill = SDL_GPU_FILLMODE_LINE; break;
		default: refusal = "point fill mode"; return false;
	}
	// Clockwise triangles face front (SdlPipelineCache::Pipeline), so D3D9's cull modes are these.
	switch (rs[D3DRS_CULLMODE]) {
		case D3DCULL_NONE:	key.Cull = SDL_GPU_CULLMODE_NONE; break;
		case D3DCULL_CW:	key.Cull = SDL_GPU_CULLMODE_FRONT; break;
		case D3DCULL_CCW:	key.Cull = SDL_GPU_CULLMODE_BACK; break;
		default: refusal = "an unknown cull mode"; return false;
	}
	key.DepthBias = float_of(rs[D3DRS_DEPTHBIAS]) * DEPTH_BIAS_UNIT_SCALE;
	key.SlopeBias = float_of(rs[D3DRS_SLOPESCALEDEPTHBIAS]);

	// Blending.  Disabled, the factors are left at zero so every disabled blend is one key.
	key.BlendEnable = rs[D3DRS_ALPHABLENDENABLE] != 0;
	if (key.BlendEnable) {
		if (!blend_pair(rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], key.SourceColour, key.DestinationColour)
			|| !blend_operation(rs[D3DRS_BLENDOP], key.ColourOperation)) {
			refusal = "an unknown blend factor or operation";
			return false;
		}
		if (rs[D3DRS_SEPARATEALPHABLENDENABLE] != 0) {
			if (!blend_pair(rs[D3DRS_SRCBLENDALPHA], rs[D3DRS_DESTBLENDALPHA], key.SourceAlpha, key.DestinationAlpha)
				|| !blend_operation(rs[D3DRS_BLENDOPALPHA], key.AlphaOperation)) {
				refusal = "an unknown alpha blend factor or operation";
				return false;
			}
		}
		else {
			key.SourceAlpha = key.SourceColour;
			key.DestinationAlpha = key.DestinationColour;
			key.AlphaOperation = key.ColourOperation;
		}
	}
	key.WriteMask = (uint8_t)(rs[D3DRS_COLORWRITEENABLE] & 0xF);

	// Depth: only with a depth-stencil to test against.
	key.DepthTest = depth_format != 0 && rs[D3DRS_ZENABLE] != D3DZB_FALSE;
	if (key.DepthTest) {
		key.DepthWrite = rs[D3DRS_ZWRITEENABLE] != 0;
		if (!compare_operation(rs[D3DRS_ZFUNC], key.DepthCompare)) {
			refusal = "an unknown depth compare function";
			return false;
		}
	}

	// Stencil: front is the clockwise face; the CCW_ states are the other face's when two-sided.
	key.StencilEnable = depth_format != 0 && rs[D3DRS_STENCILENABLE] != 0;
	if (key.StencilEnable) {
		key.StencilRead = (uint8_t)(rs[D3DRS_STENCILMASK] & 0xFF);
		key.StencilWrite = (uint8_t)(rs[D3DRS_STENCILWRITEMASK] & 0xFF);
		const bool two_sided = rs[D3DRS_TWOSIDEDSTENCILMODE] != 0;
		if (!stencil_operation(rs[D3DRS_STENCILFAIL], key.FrontFail)
			|| !stencil_operation(rs[D3DRS_STENCILZFAIL], key.FrontDepthFail)
			|| !stencil_operation(rs[D3DRS_STENCILPASS], key.FrontPass)
			|| !compare_operation(rs[D3DRS_STENCILFUNC], key.FrontCompare)
			|| !stencil_operation(rs[two_sided ? D3DRS_CCW_STENCILFAIL : D3DRS_STENCILFAIL], key.BackFail)
			|| !stencil_operation(rs[two_sided ? D3DRS_CCW_STENCILZFAIL : D3DRS_STENCILZFAIL], key.BackDepthFail)
			|| !stencil_operation(rs[two_sided ? D3DRS_CCW_STENCILPASS : D3DRS_STENCILPASS], key.BackPass)
			|| !compare_operation(rs[two_sided ? D3DRS_CCW_STENCILFUNC : D3DRS_STENCILFUNC], key.BackCompare)) {
			refusal = "an unknown stencil operation or function";
			return false;
		}
	}
	return true;
}

size_t SdlPipelineKeyHash::operator()(const SdlPipelineKey &key) const
{
	// FNV-1a over the key's bytes, padding included: the key is built with memset first.
	const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&key);
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < sizeof(key); ++i) {
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	}
	return (size_t)hash;
}

bool SdlPipelineKeyEqual::operator()(const SdlPipelineKey &left, const SdlPipelineKey &right) const
{
	return memcmp(&left, &right, sizeof(left)) == 0;
}

//-------------------------------------------------------------------------------------------------

#include "GlesRenderCommon.h"

static GLenum Gles_Compare(uint8_t op)
{
    switch (op) {
        case SDL_GPU_COMPAREOP_NEVER: return GL_NEVER;
        case SDL_GPU_COMPAREOP_LESS: return GL_LESS;
        case SDL_GPU_COMPAREOP_EQUAL: return GL_EQUAL;
        case SDL_GPU_COMPAREOP_LESS_OR_EQUAL: return GL_LEQUAL;
        case SDL_GPU_COMPAREOP_GREATER: return GL_GREATER;
        case SDL_GPU_COMPAREOP_NOT_EQUAL: return GL_NOTEQUAL;
        case SDL_GPU_COMPAREOP_GREATER_OR_EQUAL: return GL_GEQUAL;
        case SDL_GPU_COMPAREOP_ALWAYS: return GL_ALWAYS;
        default: return GL_ALWAYS;
    }
}

static GLenum Gles_Stencil(uint8_t op)
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

static GLenum Gles_Blend(uint8_t factor)
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

static GLenum Gles_BlendOp(uint8_t op)
{
    switch (op) {
        case SDL_GPU_BLENDOP_ADD: return GL_FUNC_ADD;
        case SDL_GPU_BLENDOP_SUBTRACT: return GL_FUNC_SUBTRACT;
        case SDL_GPU_BLENDOP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case SDL_GPU_BLENDOP_MIN: return GL_MIN;
        case SDL_GPU_BLENDOP_MAX: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}

static GLenum Gles_Primitive(uint8_t primitive)
{
    switch (primitive) {
        case SDL_GPU_PRIMITIVETYPE_POINTLIST: return GL_POINTS;
        case SDL_GPU_PRIMITIVETYPE_LINELIST: return GL_LINES;
        case SDL_GPU_PRIMITIVETYPE_LINESTRIP: return GL_LINE_STRIP;
        case SDL_GPU_PRIMITIVETYPE_TRIANGLELIST: return GL_TRIANGLES;
        case SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
        default: return GL_TRIANGLES;
    }
}

static void Gles_Apply_Pipeline_State(const GlesPipeline &p, uint32_t stencil_reference, uint32_t blend_factor)
{
    if (p.Cull == SDL_GPU_CULLMODE_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(p.Cull == SDL_GPU_CULLMODE_FRONT ? GL_FRONT : GL_BACK);
    }
    // PosixDevice9/SdlPipelineCache deliberately models D3D9's clockwise-front convention.
    glFrontFace(GL_CW);

    // OpenGL ES 3.0 has no polygon-mode API; non-fill pipelines are rejected by Pipeline().
    if (p.DepthBias != 0.0f || p.SlopeBias != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(p.SlopeBias, p.DepthBias);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (p.BlendEnable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(Gles_Blend(p.SourceColour), Gles_Blend(p.DestinationColour),
            Gles_Blend(p.SourceAlpha), Gles_Blend(p.DestinationAlpha));
        glBlendEquationSeparate(Gles_BlendOp(p.ColourOperation), Gles_BlendOp(p.AlphaOperation));
        const float r = (float)((blend_factor >> 16) & 0xFF) / 255.0f;
        const float g = (float)((blend_factor >> 8) & 0xFF) / 255.0f;
        const float b = (float)(blend_factor & 0xFF) / 255.0f;
        const float a = (float)((blend_factor >> 24) & 0xFF) / 255.0f;
        glBlendColor(r, g, b, a);
    } else {
        glDisable(GL_BLEND);
    }

    glColorMask((p.WriteMask & SDL_GPU_COLORCOMPONENT_R) != 0,
        (p.WriteMask & SDL_GPU_COLORCOMPONENT_G) != 0,
        (p.WriteMask & SDL_GPU_COLORCOMPONENT_B) != 0,
        (p.WriteMask & SDL_GPU_COLORCOMPONENT_A) != 0);

    if (p.DepthTest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(Gles_Compare(p.DepthCompare));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(p.DepthWrite ? GL_TRUE : GL_FALSE);

    if (p.StencilEnable) {
        glEnable(GL_STENCIL_TEST);
        glStencilMaskSeparate(GL_FRONT, p.StencilWrite);
        glStencilMaskSeparate(GL_BACK, p.StencilWrite);
        glStencilFuncSeparate(GL_FRONT, Gles_Compare(p.FrontCompare), (GLint)stencil_reference, p.StencilRead);
        glStencilFuncSeparate(GL_BACK, Gles_Compare(p.BackCompare), (GLint)stencil_reference, p.StencilRead);
        glStencilOpSeparate(GL_FRONT, Gles_Stencil(p.FrontFail), Gles_Stencil(p.FrontDepthFail), Gles_Stencil(p.FrontPass));
        glStencilOpSeparate(GL_BACK, Gles_Stencil(p.BackFail), Gles_Stencil(p.BackDepthFail), Gles_Stencil(p.BackPass));
    } else {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0);
    }
}

static bool Gles_Vertex_Attrib(uint32_t format, GLenum &type, GLint &components, GLboolean &normalized, bool &integer)
{
    type = GL_FLOAT;
    components = 4;
    normalized = GL_FALSE;
    integer = false;
    switch (format) {
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT: components=1; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2: components=2; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3: components=3; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4: components=4; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT: type=GL_INT; components=1; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT2: type=GL_INT; components=2; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT3: type=GL_INT; components=3; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT4: type=GL_INT; components=4; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT: type=GL_UNSIGNED_INT; components=1; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT2: type=GL_UNSIGNED_INT; components=2; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT3: type=GL_UNSIGNED_INT; components=3; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT4: type=GL_UNSIGNED_INT; components=4; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2: type=GL_BYTE; components=2; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4: type=GL_BYTE; components=4; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2: type=GL_UNSIGNED_BYTE; components=2; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4: type=GL_UNSIGNED_BYTE; components=4; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM: type=GL_BYTE; components=2; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM: type=GL_BYTE; components=4; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM: type=GL_UNSIGNED_BYTE; components=2; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM: type=GL_UNSIGNED_BYTE; components=4; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2: type=GL_SHORT; components=2; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4: type=GL_SHORT; components=4; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2: type=GL_UNSIGNED_SHORT; components=2; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4: type=GL_UNSIGNED_SHORT; components=4; integer=true; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM: type=GL_SHORT; components=2; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM: type=GL_SHORT; components=4; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM: type=GL_UNSIGNED_SHORT; components=2; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM: type=GL_UNSIGNED_SHORT; components=4; normalized=GL_TRUE; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_HALF2: type=GL_HALF_FLOAT; components=2; return true;
        case SDL_GPU_VERTEXELEMENTFORMAT_HALF4: type=GL_HALF_FLOAT; components=4; return true;
        default: return false;
    }
}

SdlPipelineCache::SdlPipelineCache(SDL_GPUDevice *device) :
    Device(device),
    LastPipeline(NULL),
    Built(0)
{
    memset(&LastKey, 0, sizeof(LastKey));
}

SdlPipelineCache::~SdlPipelineCache()
{
    for (std::unordered_map<SdlPipelineKey, SDL_GPUGraphicsPipeline *, SdlPipelineKeyHash, SdlPipelineKeyEqual>::iterator it = Pipelines.begin();
        it != Pipelines.end(); ++it) {
        if (it->second != NULL) {
            GlesPipeline *pipeline = Gles_Pipeline(it->second);
            if (pipeline != NULL) {
                if (pipeline->Program != 0) glDeleteProgram(pipeline->Program);
                delete pipeline;
            }
        }
    }
}

SDL_GPUGraphicsPipeline *SdlPipelineCache::Pipeline(const SdlPipelineKey &key)
{
    if (LastPipeline != NULL && memcmp(&key, &LastKey, sizeof(key)) == 0) return LastPipeline;
    std::unordered_map<SdlPipelineKey, SDL_GPUGraphicsPipeline *, SdlPipelineKeyHash, SdlPipelineKeyEqual>::iterator existing = Pipelines.find(key);
    if (existing != Pipelines.end()) {
        LastKey = key;
        LastPipeline = existing->second;
        return existing->second;
    }

    SdlVertexLayout layout;
    memset(&layout, 0, sizeof(layout));
    std::string refusal;
    if (!Sdl_Vertex_Layout(key.FVF, layout, refusal)) {
        fprintf(stderr, "GlesPipelineCache: vertex layout refused: %s\n", refusal.c_str());
        Pipelines[key] = NULL;
        LastKey = key;
        LastPipeline = NULL;
        return NULL;
    }
    if (key.Fill != SDL_GPU_FILLMODE_FILL) {
        fprintf(stderr, "GlesPipelineCache: wireframe pipeline is not available on GLES 3.0\n");
        Pipelines[key] = NULL;
        LastKey = key;
        LastPipeline = NULL;
        return NULL;
    }

    const GlesShader *vs = Gles_Shader(key.VertexShader);
    const GlesShader *fs = Gles_Shader(key.PixelShader);
    if (vs == NULL || fs == NULL || vs->Name == 0 || fs->Name == 0) {
        refusal = "missing GLES shader";
    }
    GLuint program = 0;
    if (refusal.empty()) {
        program = glCreateProgram();
        glAttachShader(program, vs->Name);
        glAttachShader(program, fs->Name);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::vector<char> log(length > 1 ? (size_t)length : 1u, '\0');
            glGetProgramInfoLog(program, (GLsizei)log.size(), NULL, &log[0]);
            refusal = std::string("OpenGL ES link: ") + &log[0];
            glDeleteProgram(program);
            program = 0;
        }
    }

    SDL_GPUGraphicsPipeline *opaque = NULL;
    if (program != 0) {
        glUseProgram(program);
        if (vs != NULL) {
            for (size_t i = 0; i < vs->UniformBlocks.size(); ++i) {
                const std::string &name = vs->UniformBlocks[i].first;
                const GLuint index = glGetUniformBlockIndex(program, name.c_str());
                if (index != GL_INVALID_INDEX)
                    glUniformBlockBinding(program, index, vs->UniformBlocks[i].second);
            }
        }
        if (fs != NULL) {
            for (size_t i = 0; i < fs->UniformBlocks.size(); ++i) {
                const std::string &name = fs->UniformBlocks[i].first;
                const GLuint index = glGetUniformBlockIndex(program, name.c_str());
                if (index != GL_INVALID_INDEX)
                    glUniformBlockBinding(program, index, fs->UniformBlocks[i].second);
            }
            for (size_t i = 0; i < fs->Samplers.size(); ++i) {
                const GLint location = glGetUniformLocation(program, fs->Samplers[i].first.c_str());
                if (location >= 0)
                    glUniform1i(location, (GLint)fs->Samplers[i].second);
            }
        }
        GlesPipeline *p = new GlesPipeline();
        memset(p, 0, sizeof(*p));
        p->Program = program;
        p->Layout = layout;
        p->Primitive = Gles_Primitive(key.Primitive);
        p->Fill = key.Fill;
        p->Cull = key.Cull;
        p->BlendEnable = key.BlendEnable;
        p->SourceColour = key.SourceColour;
        p->DestinationColour = key.DestinationColour;
        p->ColourOperation = key.ColourOperation;
        p->SourceAlpha = key.SourceAlpha;
        p->DestinationAlpha = key.DestinationAlpha;
        p->AlphaOperation = key.AlphaOperation;
        p->WriteMask = key.WriteMask;
        p->DepthTest = key.DepthTest;
        p->DepthWrite = key.DepthWrite;
        p->DepthCompare = key.DepthCompare;
        p->StencilEnable = key.StencilEnable;
        p->StencilRead = key.StencilRead;
        p->StencilWrite = key.StencilWrite;
        p->FrontFail = key.FrontFail;
        p->FrontDepthFail = key.FrontDepthFail;
        p->FrontPass = key.FrontPass;
        p->FrontCompare = key.FrontCompare;
        p->BackFail = key.BackFail;
        p->BackDepthFail = key.BackDepthFail;
        p->BackPass = key.BackPass;
        p->BackCompare = key.BackCompare;
        p->DepthBias = key.DepthBias;
        p->SlopeBias = key.SlopeBias;
        opaque = reinterpret_cast<SDL_GPUGraphicsPipeline *>(p);
        ++Built;
    } else if (refusal.empty()) {
        refusal = "glCreateProgram failed";
    }

    if (opaque == NULL) {
        fprintf(stderr, "GlesPipelineCache: pipeline refused: %s; FVF 0x%x primitive %u depth %u/%u\n",
            refusal.c_str(), (unsigned)key.FVF, (unsigned)key.Primitive, (unsigned)key.DepthTest, (unsigned)key.DepthWrite);
#if defined(__ANDROID__)
        // Preserve the actual GL link error in the app's retrievable native startup trace.
        char androidDiagnostic[768];
        snprintf(androidDiagnostic, sizeof(androidDiagnostic),
            "ANDROID GLES PIPELINE REFUSED: reason=%.480s FVF=0x%x primitive=%u depth=%u/%u",
            refusal.c_str(), (unsigned)key.FVF, (unsigned)key.Primitive,
            (unsigned)key.DepthTest, (unsigned)key.DepthWrite);
        appendAndroidDiagnostic(androidDiagnostic);
#endif
        if (Sdl_Creation_Log_Asked()) {
            char line[1024];
            snprintf(line, sizeof(line),
                "ANDROID GLES PIPELINE REFUSED: reason=%s FVF=0x%x primitive=%u depthTest=%u depthWrite=%u",
                refusal.c_str(), (unsigned)key.FVF, (unsigned)key.Primitive,
                (unsigned)key.DepthTest, (unsigned)key.DepthWrite);
            Sdl_Creation_Log_Line(line);
        }
    }
    Pipelines[key] = opaque;
    LastKey = key;
    LastPipeline = opaque;
    return opaque;
}

// ------------------------------------------------------------------------------------------------
// Samplers.

size_t SdlSamplerCache::KeyHash::operator()(const Key &key) const
{
    // FNV-1a over the complete 14-state sampler key.
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(key.States);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(key.States); ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return (size_t)hash;
}

SdlSamplerCache::SdlSamplerCache(SDL_GPUDevice *device) :
    Device(device),
    LastSampler(NULL),
    HaveLast(false)
{
    memset(&LastKey, 0, sizeof(LastKey));
}

SdlSamplerCache::~SdlSamplerCache()
{
    for (std::unordered_map<SdlSamplerCache::Key, SDL_GPUSampler *, SdlSamplerCache::KeyHash>::iterator it = Samplers.begin();
        it != Samplers.end(); ++it) {
        if (it->second != NULL) {
            GlesSampler *sampler = Gles_Sampler(it->second);
            if (sampler != NULL) {
                if (sampler->Name != 0) glDeleteSamplers(1, &sampler->Name);
                delete sampler;
            }
        }
    }
}

SDL_GPUSampler *SdlSamplerCache::Sampler(const RenderUInt32 ss[14])
{
    Key key;
    memcpy(key.States, ss, sizeof(key.States));
    if (HaveLast && memcmp(&key, &LastKey, sizeof(key)) == 0) return LastSampler;

    std::unordered_map<SdlSamplerCache::Key, SDL_GPUSampler *, SdlSamplerCache::KeyHash>::iterator existing = Samplers.find(key);
    if (existing != Samplers.end()) {
        LastKey = key;
        LastSampler = existing->second;
        HaveLast = true;
        return existing->second;
    }

    SDL_GPUSamplerCreateInfo info;
    std::string refusal;
    SDL_GPUSampler *opaque = NULL;
    if (Sdl_Sampler_Description(ss, &info, refusal)) {
        GlesSampler *sampler = new GlesSampler();
        sampler->Name = 0;
        glGenSamplers(1, &sampler->Name);
        if (sampler->Name != 0) {
            glSamplerParameteri(sampler->Name, GL_TEXTURE_WRAP_S, (GLint)(info.address_mode_u == SDL_GPU_SAMPLERADDRESSMODE_REPEAT ? GL_REPEAT :
                info.address_mode_u == SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE));
            glSamplerParameteri(sampler->Name, GL_TEXTURE_WRAP_T, (GLint)(info.address_mode_v == SDL_GPU_SAMPLERADDRESSMODE_REPEAT ? GL_REPEAT :
                info.address_mode_v == SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE));
            glSamplerParameteri(sampler->Name, GL_TEXTURE_WRAP_R, (GLint)(info.address_mode_w == SDL_GPU_SAMPLERADDRESSMODE_REPEAT ? GL_REPEAT :
                info.address_mode_w == SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE));
            const bool minLinear = info.min_filter == SDL_GPU_FILTER_LINEAR;
            const bool magLinear = info.mag_filter == SDL_GPU_FILTER_LINEAR;
            GLenum minFilter = magLinear ? GL_LINEAR : GL_NEAREST;
            if (info.mipmap_mode == SDL_GPU_SAMPLERMIPMAPMODE_LINEAR) minFilter = minLinear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
            else minFilter = minLinear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
            // D3D9's NONE mip filter means one explicit mip level. The max/min LOD keeps that level.
            if (info.mipmap_mode == SDL_GPU_SAMPLERMIPMAPMODE_NEAREST && info.min_lod == info.max_lod) {
                minFilter = minLinear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
            }
            glSamplerParameteri(sampler->Name, GL_TEXTURE_MIN_FILTER, minFilter);
            glSamplerParameteri(sampler->Name, GL_TEXTURE_MAG_FILTER, magLinear ? GL_LINEAR : GL_NEAREST);
            glSamplerParameterf(sampler->Name, GL_TEXTURE_MIN_LOD, info.min_lod);
            glSamplerParameterf(sampler->Name, GL_TEXTURE_MAX_LOD, info.max_lod);
            opaque = reinterpret_cast<SDL_GPUSampler *>(sampler);
        } else {
            delete sampler;
            refusal = "glGenSamplers failed";
        }
    }
    if (opaque == NULL && !refusal.empty()) {
        LastRefusal = refusal;
        fprintf(stderr, "GlesSamplerCache: sampler refused: %s\n", refusal.c_str());
    } else {
        LastRefusal.clear();
    }
    Samplers[key] = opaque;
    LastKey = key;
    LastSampler = opaque;
    HaveLast = true;
    return opaque;
}

// Samplers.
//-------------------------------------------------------------------------------------------------

static bool address_mode(RenderUInt32 value, SDL_GPUSamplerAddressMode &out)
{
	switch (value) {
		case D3DTADDRESS_WRAP:		out = SDL_GPU_SAMPLERADDRESSMODE_REPEAT; return true;
		case D3DTADDRESS_MIRROR:	out = SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT; return true;
		case D3DTADDRESS_CLAMP:		out = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; return true;
		default: return false;		// BORDER and MIRRORONCE: SDL3 GPU has neither
	}
}

bool Sdl_Sampler_Description(const RenderUInt32 ss[14], void *create_info, std::string &refusal)
{
	SDL_GPUSamplerCreateInfo &info = *static_cast<SDL_GPUSamplerCreateInfo *>(create_info);
	SDL_zero(info);
	const RenderUInt32 minification = ss[D3DSAMP_MINFILTER];
	const RenderUInt32 magnification = ss[D3DSAMP_MAGFILTER];
	const RenderUInt32 mip = ss[D3DSAMP_MIPFILTER];
	if (minification > D3DTEXF_ANISOTROPIC || magnification > D3DTEXF_ANISOTROPIC || mip > D3DTEXF_ANISOTROPIC) {
		refusal = "a pyramidal or gaussian filter";
		return false;
	}
	const bool anisotropic = minification == D3DTEXF_ANISOTROPIC || magnification == D3DTEXF_ANISOTROPIC;
	info.min_filter = (minification == D3DTEXF_LINEAR || anisotropic) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
	info.mag_filter = (magnification == D3DTEXF_LINEAR || anisotropic) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
	info.mipmap_mode = (mip == D3DTEXF_LINEAR || mip == D3DTEXF_ANISOTROPIC)
		? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	if (!address_mode(ss[D3DSAMP_ADDRESSU], info.address_mode_u)
		|| !address_mode(ss[D3DSAMP_ADDRESSV], info.address_mode_v)
		|| !address_mode(ss[D3DSAMP_ADDRESSW], info.address_mode_w)) {
		refusal = "border or mirror-once addressing";
		return false;
	}
	info.mip_lod_bias = float_of(ss[D3DSAMP_MIPMAPLODBIAS]);
	// D3DSAMP_MAXMIPLEVEL is the most detailed level the sampler may use.  With no mip filter the
	// sampler reads that level and no other, and still magnifies or minifies by the LOD: SDL3 GPU's
	// backends (Metal, Vulkan) choose between the two filters on the LOD after this clamp, so pinning
	// both ends to the level would magnify everywhere.  A quarter level of room keeps the nearest level
	// the same one and leaves a minified pixel's LOD above zero.
	info.min_lod = (float)ss[D3DSAMP_MAXMIPLEVEL];
	info.max_lod = mip == D3DTEXF_NONE ? info.min_lod + 0.25f : 1000.0f;
	// MAXANISOTROPY held to 1..16: the caps' MaxAnisotropy (PosixD3D9Caps.cpp), and the range Metal's
	// maxAnisotropy takes (SDL3 passes the value through as it is).
	const RenderUInt32 anisotropy = ss[D3DSAMP_MAXANISOTROPY];
	info.enable_anisotropy = anisotropic;
	info.max_anisotropy = anisotropic ? (float)(anisotropy < 1 ? 1 : anisotropy > 16 ? 16 : anisotropy) : 1.0f;
	return true;
}

