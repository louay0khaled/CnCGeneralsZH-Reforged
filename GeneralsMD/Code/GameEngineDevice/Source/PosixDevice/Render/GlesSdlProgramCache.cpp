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

// The generated programs on SDL3 GPU (decision 7, phase A3c).  See SdlProgramCache.h.

#include "SdlProgramCache.h"
#include "Common/CrashHandler.h"
#include "SdlCreationLog.h"
#include "sdl3shadercompile.h"
#include "engineshader.h"
#include "ffshader.h"
#include "ffvertex.h"

#include <SDL3/SDL.h>

#include "GlesRenderCommon.h"
#include <spirv_cross_c.h>
#include <spirv.h>
#include <GLES3/gl3.h>

static std::string Gles_Spvc_Error(spvc_context context, const char *fallback)
{
    const char *error = context != NULL ? spvc_context_get_last_error_string(context) : NULL;
    return error != NULL && error[0] != '\0' ? std::string(error) : std::string(fallback);
}

/*
 * SPIRV-Cross flattens HLSL interface structs for GLES 3.00. Its generated varying
 * names can include both the outer interface variable ("output"/"input") and the
 * struct member name, so equal HLSL semantics may otherwise link under different
 * GLSL names. Rename reflected stage inputs/outputs by their SPIR-V Location, not
 * by an assumed HLSL variable name. This mirrors SPIRV-Cross's
 * rename_interface_variable utility using the C API available in the pinned vendor.
 */
static void Gles_Rename_Interface_Varyings(spvc_compiler compiler, spvc_resources resources,
    bool vertex_stage)
{
    const spvc_resource_type resource_type = vertex_stage
        ? SPVC_RESOURCE_TYPE_STAGE_OUTPUT : SPVC_RESOURCE_TYPE_STAGE_INPUT;
    const spvc_reflected_resource *interfaces = NULL;
    size_t interface_count = 0;
    if (spvc_resources_get_resource_list_for_type(resources, resource_type,
        &interfaces, &interface_count) != SPVC_SUCCESS) {
        return;
    }

    static unsigned int trace_count = 0;
    for (size_t i = 0; i < interface_count; ++i) {
        const spvc_reflected_resource &resource = interfaces[i];
        const spvc_type base_type = spvc_compiler_get_type_handle(compiler, resource.base_type_id);
        const bool is_struct = base_type != NULL
            && spvc_type_get_basetype(base_type) == SPVC_BASETYPE_STRUCT;
        const bool variable_has_location =
            spvc_compiler_has_decoration(compiler, resource.id, SpvDecorationLocation) != 0;
        const unsigned int variable_location = variable_has_location
            ? spvc_compiler_get_decoration(compiler, resource.id, SpvDecorationLocation) : 0u;
        const char *old_name_ptr = spvc_compiler_get_name(compiler, resource.id);
        char old_name[128];
        snprintf(old_name, sizeof(old_name), "%s",
            old_name_ptr != NULL ? old_name_ptr : "(unnamed)");

        if (is_struct) {
            // Flattened block members must also have identical names in both stages.
            spvc_compiler_set_name(compiler, resource.base_type_id, "GlesInterfaceBlock");
            const unsigned int members = spvc_type_get_num_member_types(base_type);
            for (unsigned int member = 0; member < members; ++member) {
                char member_name[64];
                if (spvc_compiler_has_member_decoration(compiler, resource.base_type_id,
                    member, SpvDecorationLocation)) {
                    const unsigned int location = spvc_compiler_get_member_decoration(compiler,
                        resource.base_type_id, member, SpvDecorationLocation);
                    snprintf(member_name, sizeof(member_name), "zh_varying_%u", location);
                } else {
                    // System values and compiler-generated fields may not have a user location.
                    snprintf(member_name, sizeof(member_name), "GlesInterfaceMember_%u", member);
                }
                spvc_compiler_set_member_name(compiler, resource.base_type_id, member, member_name);
            }
            // Stable outer name is needed because GLES 3.00 flattens with this name as a prefix.
            spvc_compiler_set_name(compiler, resource.id, "zh_io");
        } else if (variable_has_location) {
            char name[64];
            snprintf(name, sizeof(name), "zh_varying_%u", variable_location);
            spvc_compiler_set_name(compiler, resource.id, name);
        }

#if defined(__ANDROID__)
        if (trace_count < 24 && (variable_has_location || is_struct)) {
            char diagnostic[384];
            snprintf(diagnostic, sizeof(diagnostic),
                "GLES varying remap: stage=%s old=%s location=%s%u struct=%u",
                vertex_stage ? "vertex-out" : "fragment-in", old_name,
                variable_has_location ? "" : "member-based/", variable_location,
                is_struct ? 1u : 0u);
            appendAndroidDiagnostic(diagnostic);
            ++trace_count;
        }
#endif
    }
}

bool Gles_Compile_SPIRV_To_GLSLES(const std::vector<unsigned char> &spirv, bool vertex_stage,
    std::string &source, std::string &log, std::vector<std::pair<std::string, unsigned> > *samplers,
    std::vector<std::pair<std::string, unsigned> > *uniform_blocks)
{
    source.clear();
    log.clear();
    if (samplers != NULL) samplers->clear();
    if (uniform_blocks != NULL) uniform_blocks->clear();
    if (spirv.empty() || (spirv.size() & 3u) != 0) {
        log = "invalid SPIR-V byte count";
        return false;
    }
    std::vector<SpvId> words(spirv.size() / 4);
    memcpy(&words[0], &spirv[0], spirv.size());

    spvc_context context = NULL;
    spvc_parsed_ir parsed = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options options = NULL;

    if (spvc_context_create(&context) != SPVC_SUCCESS ||
        spvc_context_parse_spirv(context, &words[0], words.size(), &parsed) != SPVC_SUCCESS ||
        spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, parsed, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        log = Gles_Spvc_Error(context, "SPIRV-Cross could not create a GLSL compiler");
        if (context != NULL) spvc_context_destroy(context);
        return false;
    }

    if (spvc_compiler_create_compiler_options(compiler, &options) != SPVC_SUCCESS ||
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE) != SPVC_SUCCESS ||
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 300) != SPVC_SUCCESS ||
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_FORCE_FLATTENED_IO_BLOCKS, SPVC_TRUE) != SPVC_SUCCESS ||
        (vertex_stage && spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, SPVC_TRUE) != SPVC_SUCCESS) ||
        spvc_compiler_install_compiler_options(compiler, options) != SPVC_SUCCESS) {
        log = Gles_Spvc_Error(context, "SPIRV-Cross could not configure GLSL ES 3.00");
        spvc_context_destroy(context);
        return false;
    }

    spvc_resources resources = NULL;
    if (spvc_compiler_create_shader_resources(compiler, &resources) == SPVC_SUCCESS) {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER,
            &list, &count) == SPVC_SUCCESS) {
            for (size_t i = 0; i < count; ++i) {
                const unsigned binding = vertex_stage ? 0u : 1u;
                spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationBinding, binding);
                spvc_compiler_unset_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
                if (uniform_blocks != NULL && list[i].name != NULL && list[i].name[0] != '\\0')
                    uniform_blocks->push_back(std::make_pair(std::string(list[i].name), binding));
            }
        }
    }

    if (resources != NULL) {
        Gles_Rename_Interface_Varyings(compiler, resources, vertex_stage);
    }

    if (!vertex_stage && spvc_compiler_build_combined_image_samplers(compiler) == SPVC_SUCCESS) {
        const spvc_combined_image_sampler *combined = NULL;
        size_t count = 0;
        if (spvc_compiler_get_combined_image_samplers(compiler, &combined, &count) == SPVC_SUCCESS) {
            for (size_t i = 0; i < count; ++i) {
                unsigned binding = spvc_compiler_get_decoration(compiler, combined[i].image_id, SpvDecorationBinding);
                spvc_compiler_set_decoration(compiler, combined[i].combined_id, SpvDecorationBinding, binding);
                if (samplers != NULL) {
                    const char *name = spvc_compiler_get_name(compiler, combined[i].combined_id);
                    if (name != NULL && name[0] != '\0')
                        samplers->push_back(std::make_pair(std::string(name), binding));
                }
                spvc_compiler_unset_decoration(compiler, combined[i].combined_id, SpvDecorationDescriptorSet);
            }
        }
    }

    spvc_compiler_add_header_line(compiler, "precision highp float;");
    spvc_compiler_add_header_line(compiler, "precision highp int;");

    const char *compiled = NULL;
    if (spvc_compiler_compile(compiler, &compiled) != SPVC_SUCCESS || compiled == NULL || compiled[0] == '\0') {
        log = Gles_Spvc_Error(context, "SPIRV-Cross could not compile GLSL ES");
        spvc_context_destroy(context);
        return false;
    }
    source.assign(compiled);
    spvc_context_destroy(context);
    return true;
}

bool Gles_Compile_Shader(GLenum type, const std::string &source, GLuint &shader, std::string &log)
{
    shader = glCreateShader(type);
    if (shader == 0) {
        log = "glCreateShader failed";
        return false;
    }
    const GLchar *text = source.c_str();
    const GLint length = (GLint)source.size();
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint log_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        std::vector<char> buffer(log_length > 1 ? (size_t)log_length : 1u, '\0');
        glGetShaderInfoLog(shader, (GLsizei)buffer.size(), NULL, &buffer[0]);
        log.assign(&buffer[0]);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    log.clear();
    return true;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Sdl_Read_Slot_Lines(const std::string &hlsl, unsigned int slots, std::vector<int> &textures, std::vector<int> &samplers)
{
	textures.assign(slots, 0);
	samplers.assign(slots, 0);
	for (unsigned int slot = 0; slot < slots; ++slot) {
		textures[slot] = (int)slot;
		samplers[slot] = (int)slot;
	}
	static const char PREFIX[] = "// SDL3 slot ";
	for (size_t at = hlsl.find(PREFIX); at != std::string::npos; at = hlsl.find(PREFIX, at + 1)) {
		int slot = -1, texture = -1, sampler = -1;
		if (sscanf(hlsl.c_str() + at, "// SDL3 slot %d: texture t%d, sampler state s%d", &slot, &texture, &sampler) == 3
			&& slot >= 0 && (unsigned int)slot < slots) {
			textures[slot] = texture;
			samplers[slot] = sampler;
		}
	}
}


SdlProgramCache::SdlProgramCache(SDL_GPUDevice *device) :
    Device(device),
    Built(0),
    Refused(0),
    CompileMilliseconds(0.0)
{
}

SdlProgramCache::~SdlProgramCache()
{
    for (int stage = 0; stage < 2; ++stage) {
        std::map<std::string, SdlProgram> &cache = stage == 0 ? VertexPrograms : PixelPrograms;
        for (std::map<std::string, SdlProgram>::iterator it = cache.begin(); it != cache.end(); ++it) {
            if (it->second.Shader != NULL) {
                GlesShader *shader = Gles_Shader(it->second.Shader);
                if (shader != NULL) {
                    if (shader->Name != 0) glDeleteShader(shader->Name);
                    delete shader;
                }
            }
        }
    }
}

const SdlProgram &SdlProgramCache::Make(std::map<std::string, SdlProgram> &cache, const std::string &key, bool generated,
    const std::string &hlsl, bool vertex_stage)
{
    SdlProgram &program = cache[key];
    program.Shader = NULL;
    program.Key = key;
    program.Refusal.clear();
    program.SamplerSlots = 0;
    program.UniformBuffers = 0;
    program.Uses = 1;
    if (!generated) {
        program.Refusal = "the generator refuses the description";
    }
    else {
        const Uint64 start = SDL_GetTicksNS();
        std::vector<unsigned char> spirv;
        std::vector<std::pair<std::string, unsigned> > samplers;
        std::vector<std::pair<std::string, unsigned> > uniform_blocks;
        std::string log;
        std::string glsl;
        Dump_Program(key, vertex_stage, hlsl);
        if (!SDL3_Compile_HLSL_To_SPIRV(hlsl, vertex_stage, spirv, log)) {
            program.Refusal = "glslang: " + log;
        }
        else if (!Gles_Compile_SPIRV_To_GLSLES(spirv, vertex_stage, glsl, log, &samplers, &uniform_blocks)) {
            program.Refusal = "SPIRV-Cross: " + log;
        }
        else {
            GLuint object = 0;
            if (!Gles_Compile_Shader(vertex_stage ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, glsl, object, log)) {
                program.Refusal = "OpenGL ES shader: " + log;
            }
            else {
                GlesShader *shader = new GlesShader();
                shader->Name = object;
                shader->Vertex = vertex_stage;
                shader->Samplers = samplers;
                shader->UniformBlocks = uniform_blocks;
                SDL3_Shader_Slots(spirv, vertex_stage, shader->SamplerSlots, shader->UniformBuffers);
                program.SamplerSlots = shader->SamplerSlots;
                program.UniformBuffers = shader->UniformBuffers;
                program.Shader = reinterpret_cast<SDL_GPUShader *>(shader);
            }
        }
        CompileMilliseconds += (double)(SDL_GetTicksNS() - start) / 1.0e6;
        if (Sdl_Creation_Log_Asked()) {
            Sdl_Creation_Log(vertex_stage ? "gles-vprogram" : "gles-pprogram", (double)start / 1.0e6,
                (double)(SDL_GetTicksNS() - start) / 1.0e6, key.substr(0, 90).c_str());
        }
    }
    if (program.Shader == NULL) {
        ++Refused;
        fprintf(stderr, "GlesProgramCache: %s program %s refused: %s\n",
            vertex_stage ? "vertex" : "pixel", key.c_str(), program.Refusal.c_str());
        if (Sdl_Creation_Log_Asked()) {
            char line[1024];
            snprintf(line, sizeof(line), "ANDROID GLES PROGRAM REFUSED: stage=%s reason=%s key=%s",
                vertex_stage ? "vertex" : "pixel", program.Refusal.c_str(), key.c_str());
            Sdl_Creation_Log_Line(line);
        }
        return program;
    }
    Sdl_Read_Slot_Lines(hlsl, program.SamplerSlots, program.SlotTexture, program.SlotSampler);
    ++Built;
    return program;
}

// ZH_GPU_DUMP_PROGRAMS=<folder>: every program as the generator wrote it (N-v.hlsl, N-p.hlsl), with
// programs.txt naming each N's key.  For reading what a backend refuses; off unless asked for.
void SdlProgramCache::Dump_Program(const std::string &key, bool vertex_stage, const std::string &hlsl)
{
	static const char *const folder = getenv("ZH_GPU_DUMP_PROGRAMS");
	if (folder == NULL || folder[0] == '\0') {
		return;
	}
	static unsigned dumped = 0;
	const unsigned index = ++dumped;
	const char stage = vertex_stage ? 'v' : 'p';
	char path[1024];
	snprintf(path, sizeof(path), "%s/%u-%c.hlsl", folder, index, stage);
	if (FILE *file = fopen(path, "wb")) {
		fwrite(hlsl.data(), 1, hlsl.size(), file);
		fclose(file);
	}
	snprintf(path, sizeof(path), "%s/programs.txt", folder);
	if (FILE *file = fopen(path, "ab")) {
		fprintf(file, "%u %c %s\n", index, stage, key.c_str());
		fclose(file);
	}
}

std::string SdlProgramCache::Key_Of(const SDL_GPUShader *shader) const
{
	for (int stage = 0; stage < 2; ++stage) {
		const std::map<std::string, SdlProgram> &cache = stage == 0 ? VertexPrograms : PixelPrograms;
		for (std::map<std::string, SdlProgram>::const_iterator it = cache.begin(); it != cache.end(); ++it) {
			if (it->second.Shader == shader) {
				return it->first;
			}
		}
	}
	return std::string();
}

static uint64_t bytes_hash(const void *bytes, size_t size)
{
	const uint8_t *b = (const uint8_t *)bytes;
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i) {
		hash = (hash ^ b[i]) * 1099511628211ull;
	}
	return hash;
}

SdlProgram *SdlProgramCache::ByBytes::Find(const void *bytes, size_t size)
{
	if (Last != NULL && LastBytes.size() == size && memcmp(&LastBytes[0], bytes, size) == 0) {
		return Last;
	}
	std::unordered_map<uint64_t, std::vector<std::pair<std::vector<uint8_t>, SdlProgram *> > >::iterator bucket =
		Table.find(bytes_hash(bytes, size));
	if (bucket == Table.end()) {
		return NULL;
	}
	for (size_t i = 0; i < bucket->second.size(); ++i) {
		const std::vector<uint8_t> &kept = bucket->second[i].first;
		if (kept.size() == size && memcmp(&kept[0], bytes, size) == 0) {
			LastBytes = kept;
			Last = bucket->second[i].second;
			return Last;
		}
	}
	return NULL;
}

void SdlProgramCache::ByBytes::Add(const void *bytes, size_t size, SdlProgram *program)
{
	const uint8_t *b = (const uint8_t *)bytes;
	// A description whose padding varied would add itself on every draw: past this many, the lookup stops
	// growing and such draws go to the key, as they did before.
	const size_t MAXIMUM_ENTRIES = 16384;
	if (Entries < MAXIMUM_ENTRIES) {
		Table[bytes_hash(bytes, size)].push_back(std::make_pair(std::vector<uint8_t>(b, b + size), program));
		++Entries;
	}
	LastBytes.assign(b, b + size);
	Last = program;
}

const SdlProgram &SdlProgramCache::Vertex_Program(const VertexPipelineDescription &description)
{
	if (SdlProgram *found = VertexByBytes.Find(&description, sizeof(description))) {
		++found->Uses;
		return *found;
	}
	const std::string key = VertexShader_Key(description);
	std::map<std::string, SdlProgram>::iterator existing = VertexPrograms.find(key);
	if (existing != VertexPrograms.end()) {
		++existing->second.Uses;
		VertexByBytes.Add(&description, sizeof(description), &existing->second);
		return existing->second;
	}
	std::string hlsl;
	const bool generated = VertexShader_Generate(description, VERTEX_SHADER_TARGET_SDL3_GPU, hlsl);
	const SdlProgram &made = Make(VertexPrograms, key, generated, hlsl, true);
	VertexByBytes.Add(&description, sizeof(description), &VertexPrograms[key]);
	return made;
}

const SdlProgram &SdlProgramCache::Engine_Vertex_Program(int program)
{
	if (SdlProgram *found = EngineVertexByBytes.Find(&program, sizeof(program))) {
		++found->Uses;
		return *found;
	}
	const EngineShaderProgram engine = (EngineShaderProgram)program;
	const std::string key = std::string("engine:") + EngineShader_Name(engine);
	std::map<std::string, SdlProgram>::iterator existing = VertexPrograms.find(key);
	if (existing != VertexPrograms.end()) {
		++existing->second.Uses;
		EngineVertexByBytes.Add(&program, sizeof(program), &existing->second);
		return existing->second;
	}
	std::string hlsl;
	const bool written = EngineShader_Vertex_Program(engine, hlsl, VERTEX_SHADER_TARGET_SDL3_GPU);
	const SdlProgram &made = Make(VertexPrograms, key, written, hlsl, true);
	EngineVertexByBytes.Add(&program, sizeof(program), &VertexPrograms[key]);
	return made;
}

const SdlProgram &SdlProgramCache::Engine_Pixel_Program(int program, const PixelPipelineDescription &pipeline)
{
	// The program and the pipeline description, side by side, as the lookup's bytes.
	struct { int Program; PixelPipelineDescription Pipeline; } lookup;
	memset(&lookup, 0, sizeof(lookup));
	lookup.Program = program;
	memcpy(&lookup.Pipeline, &pipeline, sizeof(pipeline));	// padding and all, as the caller built it
	if (SdlProgram *found = EnginePixelByBytes.Find(&lookup, sizeof(lookup))) {
		++found->Uses;
		return *found;
	}
	const EngineShaderProgram engine = (EngineShaderProgram)program;
	const std::string key = std::string("engine:") + EngineShader_Name(engine) + CombinerShader_Pipeline_Key(pipeline);
	std::map<std::string, SdlProgram>::iterator existing = PixelPrograms.find(key);
	if (existing != PixelPrograms.end()) {
		++existing->second.Uses;
		EnginePixelByBytes.Add(&lookup, sizeof(lookup), &existing->second);
		return existing->second;
	}
	std::string hlsl;
	const bool written = EngineShader_Pixel_Program(engine, pipeline, hlsl, COMBINER_SHADER_TARGET_SDL3_GPU);
	const SdlProgram &made = Make(PixelPrograms, key, written, hlsl, false);
	EnginePixelByBytes.Add(&lookup, sizeof(lookup), &PixelPrograms[key]);
	return made;
}

const SdlProgram &SdlProgramCache::Pixel_Program(const CombinerDescription &description)
{
	if (SdlProgram *found = PixelByBytes.Find(&description, sizeof(description))) {
		++found->Uses;
		return *found;
	}
	const std::string key = CombinerShader_Key(description);
	std::map<std::string, SdlProgram>::iterator existing = PixelPrograms.find(key);
	if (existing != PixelPrograms.end()) {
		++existing->second.Uses;
		PixelByBytes.Add(&description, sizeof(description), &existing->second);
		return existing->second;
	}
	std::string hlsl;
	const bool generated = CombinerShader_Generate(description, COMBINER_SHADER_TARGET_SDL3_GPU, hlsl);
	const SdlProgram &made = Make(PixelPrograms, key, generated, hlsl, false);
	PixelByBytes.Add(&description, sizeof(description), &PixelPrograms[key]);
	return made;
}
