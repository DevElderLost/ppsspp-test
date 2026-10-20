// See ReshadeEffect.h.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ext/reshadefx/effect_codegen.hpp"
#include "ext/reshadefx/effect_parser.hpp"
#include "ext/reshadefx/effect_preprocessor.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO_WRITE
#include "ext/reshadefx/stb_image.h"

#include "ext/SPIRV-Cross/spirv.hpp"
#include "ext/SPIRV-Cross/spirv_glsl.hpp"

#include "GPU/Common/ReshadeEffect.h"

namespace {

std::string DirName(const std::string &p) {
	size_t s = p.find_last_of("/\\");
	return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

const reshadefx::annotation *FindAnnotation(const std::vector<reshadefx::annotation> &a, const char *name) {
	for (const auto &x : a)
		if (x.name == name)
			return &x;
	return nullptr;
}

// Loads `file` from the texture directory (or next to the shader) as RGBA8 at the size the shader declared.
bool LoadImageFile(const std::string &textureDir, const std::string &fxDir, const std::string &file, int w, int h, ReshadeImage *out) {
	int iw = 0, ih = 0, n = 0;
	stbi_uc *px = nullptr;
	if (!textureDir.empty())
		px = stbi_load((textureDir + "/" + file).c_str(), &iw, &ih, &n, 4);
	if (!px)
		px = stbi_load((fxDir + "/" + file).c_str(), &iw, &ih, &n, 4);
	if (!px)
		return false;
	if (w <= 0) w = iw;
	if (h <= 0) h = ih;
	out->width = w;
	out->height = h;
	out->rgba.resize((size_t)w * h * 4);
	if (iw == w && ih == h) {
		memcpy(out->rgba.data(), px, out->rgba.size());
	} else {
		// Bilinear resize to the declared size.
		for (int y = 0; y < h; y++) {
			float fy = h > 1 ? (float)y * (ih - 1) / (float)(h - 1) : 0.0f;
			int y0 = (int)fy, y1 = std::min(y0 + 1, ih - 1);
			float ty = fy - y0;
			for (int x = 0; x < w; x++) {
				float fx = w > 1 ? (float)x * (iw - 1) / (float)(w - 1) : 0.0f;
				int x0 = (int)fx, x1 = std::min(x0 + 1, iw - 1);
				float tx = fx - x0;
				for (int k = 0; k < 4; k++) {
					float a = px[((size_t)y0 * iw + x0) * 4 + k], b = px[((size_t)y0 * iw + x1) * 4 + k];
					float c = px[((size_t)y1 * iw + x0) * 4 + k], d = px[((size_t)y1 * iw + x1) * 4 + k];
					float v = (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
					out->rgba[((size_t)y * w + x) * 4 + k] = (uint8_t)(v + 0.5f);
				}
			}
		}
	}
	stbi_image_free(px);
	return true;
}

// Generates GLSL for one entry point. `samplerSlots` maps SPIR-V sampler binding -> texture slot (0..2).
bool Generate(const std::vector<uint32_t> &spirv, const std::string &entry, spv::ExecutionModel model, ReshadeTarget target, int glslVersion,
	const std::map<uint32_t, int> &samplerSlots, const std::vector<ReshadeUniform> &uniforms, std::string *out, std::string *error, int outputToLocation0 = -1) {
	spirv_cross::CompilerGLSL compiler(spirv);
	compiler.set_entry_point(entry, model);

	spirv_cross::CompilerGLSL::Options opts;
	if (target == ReshadeTarget::VULKAN) {
		opts.vulkan_semantics = true;
		opts.version = 450;
		opts.es = false;
	} else {
		opts.vulkan_semantics = false;
		opts.version = glslVersion;
		opts.es = target == ReshadeTarget::GLSL_ES3;
		opts.emit_uniform_buffer_as_plain_uniforms = true;
		opts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
		opts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
	}
	compiler.set_common_options(opts);

	spirv_cross::ShaderResources res = compiler.get_shader_resources(compiler.get_active_interface_variables());
	std::map<int, int> variablesPerSlot;
	for (const auto &si : res.sampled_images) {
		uint32_t binding = compiler.get_decoration(si.id, spv::DecorationBinding);
		auto it = samplerSlots.find(binding);
		if (it == samplerSlots.end()) {
			*error = "sampler binding " + std::to_string(binding) + " has no texture slot";
			return false;
		}
		// Several ReShade samplers can look at the same texture (BackBuffer + DepthBuffer, for one). On GL the first is
		// sampler<slot> and SPIRV-Cross names the next one sampler<slot>_1, which keeps the default texture unit 0: only
		// right for slot 0.
		if (target != ReshadeTarget::VULKAN && ++variablesPerSlot[it->second] > 1 && it->second != 0) {
			*error = "one texture is sampled through two samplers; only supported for the back buffer on OpenGL";
			return false;
		}
		if (target == ReshadeTarget::VULKAN) {
			compiler.set_decoration(si.id, spv::DecorationDescriptorSet, 0);
			compiler.set_decoration(si.id, spv::DecorationBinding, (uint32_t)it->second + 1);
		} else {
			compiler.set_name(si.id, "sampler" + std::to_string(it->second));
		}
	}
	for (const auto &ub : res.uniform_buffers) {
		if (target == ReshadeTarget::VULKAN) {
			compiler.set_decoration(ub.id, spv::DecorationDescriptorSet, 0);
			compiler.set_decoration(ub.id, spv::DecorationBinding, 0);
		} else {
			compiler.set_name(ub.id, "rs");  // the uniform description addresses members as rs.<name>
			// Members become plain uniforms; give them the names the uniform description uses.
			for (size_t i = 0; i < uniforms.size(); i++) {
				for (uint32_t m = 0; m < (uint32_t)compiler.get_type(ub.base_type_id).member_types.size(); m++) {
					if (compiler.type_struct_member_offset(compiler.get_type(ub.base_type_id), m) == uniforms[i].offset)
						compiler.set_member_name(ub.base_type_id, m, uniforms[i].name);
				}
			}
		}
	}
	if (!res.storage_buffers.empty() || !res.storage_images.empty()) {
		*error = "storage buffers / images are not supported";
		return false;
	}

	if (outputToLocation0 >= 0 && model == spv::ExecutionModelFragment) {
		// A pass with several render targets runs once per target (PPSSPP framebuffers have one color attachment):
		// the wanted output moves to location 0, the others make room for it and are ignored by the pipeline.
		for (const auto &o : res.stage_outputs) {
			const uint32_t loc = compiler.get_decoration(o.id, spv::DecorationLocation);
			const uint32_t want = (int)loc == outputToLocation0 ? 0 : ((int)loc < outputToLocation0 ? loc + 1 : loc);
			compiler.set_decoration(o.id, spv::DecorationLocation, want);
		}
	}

	*out = compiler.compile();

	if (target != ReshadeTarget::VULKAN) {
		// GLSL ES 3 and GLSL 3.3 don't allow an empty last `case` / `default` body, and SPIRV-Cross writes `case 5: { }`
		// for cases that just leave the switch.
		static const char *const kLabels[] = { "case ", "default:" };
		std::string &s = *out;
		for (const char *label : kLabels) {
			size_t pos = 0;
			while ((pos = s.find(label, pos)) != std::string::npos) {
				size_t colon = s.find(':', pos);
				size_t open = colon == std::string::npos ? colon : s.find_first_not_of(" \t\r\n", colon + 1);
				if (open != std::string::npos && s[open] == '{') {
					size_t close = s.find_first_not_of(" \t\r\n", open + 1);
					if (close != std::string::npos && s[close] == '}') {
						s.insert(close, "break; ");
						pos = close + 7;
						continue;
					}
				}
				pos = colon == std::string::npos ? pos + 1 : colon + 1;
			}
		}
	}
	return true;
}

}  // namespace

// Shader packs that went through a zip made on Linux can contain symbolic links stored as small text files whose content
// is the target path (`/home/x/reshade-shaders/Shaders/ReShade.fxh`). Replace those with an #include of the target's
// file name, which is what the link meant, so `#include "Reshade.fxh"` finds the real header.
static void FixSymlinkTextFiles(const std::string &dir) {
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file(ec) || entry.file_size(ec) > 400 || entry.file_size(ec) < 4)
			continue;
		const std::string ext = entry.path().extension().string();
		if (ext != ".fxh" && ext != ".fx" && ext != ".h")
			continue;
		FILE *f = fopen(entry.path().string().c_str(), "rb");
		if (!f)
			continue;
		char buf[512];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		std::string text(buf, n);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
			text.pop_back();
		if (text.empty() || (text[0] != '/' && text[0] != '.') || text.find('\n') != std::string::npos || text.find("#") != std::string::npos)
			continue;
		const size_t slash = text.find_last_of("/\\");
		const std::string target = slash == std::string::npos ? text : text.substr(slash + 1);
		if (target.empty() || target == entry.path().filename().string())
			continue;
		const std::string replacement = "#pragma once\n#include \"" + target + "\"\n";
		if (FILE *w = fopen(entry.path().string().c_str(), "wb")) {
			fwrite(replacement.data(), 1, replacement.size(), w);
			fclose(w);
		}
	}
}

bool ReshadeLoadMask(const std::string &path, ReshadeImage *out) {
	int w = 0, h = 0, n = 0;
	stbi_uc *px = stbi_load(path.c_str(), &w, &h, &n, 4);
	if (!px)
		return false;
	out->width = w;
	out->height = h;
	out->rgba.assign(px, px + (size_t)w * h * 4);
	stbi_image_free(px);
	return true;
}

bool ReshadeCompile(const std::string &fxPath, const std::string &includeDir, const std::string &textureDir,
	int width, int height, ReshadeTarget target, int glslVersion, ReshadeProgram *out) {
	*out = ReshadeProgram();
	const std::string fxDir = DirName(fxPath);
	FixSymlinkTextFiles(includeDir);
	if (fxDir != includeDir)
		FixSymlinkTextFiles(fxDir);

	reshadefx::preprocessor pp;
	pp.add_macro_definition("__RESHADE__", "50100");
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "1");
	pp.add_macro_definition("__RENDERER__", "0x20000");
	pp.add_macro_definition("__VENDOR__", "0");
	pp.add_macro_definition("__DEVICE__", "0");
	pp.add_macro_definition("__APPLICATION__", "0");
	pp.add_macro_definition("BUFFER_WIDTH", std::to_string(width));
	pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(height));
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
	pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
	pp.add_macro_definition("BUFFER_COLOR_DEPTH", "8");
	if (!includeDir.empty())
		pp.add_include_path(includeDir);
	pp.add_include_path(fxDir);
	if (!pp.append_file(fxPath)) {
		out->error = "cannot read " + fxPath + (pp.errors().empty() ? "" : ": " + pp.errors());
		return false;
	}
	if (pp.output().empty()) {
		out->error = "preprocessor: " + pp.errors();
		return false;
	}

	reshadefx::parser parser;
	std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(
		true /* vulkan semantics */, false /* debug info */, true /* uniforms -> spec constants */, true /* flip y */));
	if (!parser.parse(std::move(pp.output()), codegen.get())) {
		std::string e = parser.errors();
		if (e.size() > 400)
			e.resize(400);
		out->error = e.empty() ? "parse failed" : e;
		return false;
	}
	reshadefx::module mod;
	codegen->write_result(mod);
	if (mod.spirv.empty() || mod.techniques.empty()) {
		out->error = "no technique in the shader";
		return false;
	}

	// Textures: COLOR is the back buffer, DEPTH the depth buffer, `source` ones are files, the rest are render targets.
	enum class Kind { BackBuffer, Depth, Image, RenderTarget };
	struct TexInfo { Kind kind; int index; };
	std::map<std::string, TexInfo> texInfo;
	for (const auto &t : mod.textures) {
		if (t.semantic == "COLOR") {
			texInfo[t.unique_name] = { Kind::BackBuffer, -1 };
			continue;
		}
		if (t.semantic == "DEPTH") {
			texInfo[t.unique_name] = { Kind::Depth, -1 };
			continue;
		}
		const reshadefx::annotation *src = FindAnnotation(t.annotations, "source");
		if (!src) {
			ReshadeTexture rt;
			rt.width = (int)std::max(1u, t.width);
			rt.height = (int)std::max(1u, t.height);
			rt.levels = (int)std::max(1u, t.levels);
			switch (t.format) {
			case reshadefx::texture_format::r8:
			case reshadefx::texture_format::rg8:
			case reshadefx::texture_format::rgba8:
			case reshadefx::texture_format::rgb10a2:
				rt.format = ReshadeFormat::RGBA8;
				break;
			default:  // r16f, r32f, rg16, rg16f, rg32f, rgba16, rgba16f, rgba32f: half float keeps HDR values
				rt.format = ReshadeFormat::RGBA16F;
				break;
			}
			out->textures.push_back(rt);
			texInfo[t.unique_name] = { Kind::RenderTarget, (int)out->textures.size() - 1 };
			continue;
		}
		ReshadeImage img;
		if (!LoadImageFile(textureDir, fxDir, src->value.string_data, (int)t.width, (int)t.height, &img)) {
			// Missing lens dirt / noise textures are harmless, so keep going with black and say so.
			img.width = (int)std::max(1u, t.width);
			img.height = (int)std::max(1u, t.height);
			img.rgba.assign((size_t)img.width * img.height * 4, 0);
			for (size_t i = 3; i < img.rgba.size(); i += 4)
				img.rgba[i] = 255;
			if (!out->warning.empty())
				out->warning += ", ";
			out->warning += src->value.string_data;
		}
		out->images.push_back(std::move(img));
		texInfo[t.unique_name] = { Kind::Image, (int)out->images.size() - 1 };
	}

	// Uniforms left in the block (source annotated ones).
	out->uniformBufferSize = std::max(16u, (mod.total_uniform_size + 15u) & ~15u);
	for (size_t i = 0; i < mod.uniforms.size(); i++) {
		const auto &u = mod.uniforms[i];
		const reshadefx::annotation *s = FindAnnotation(u.annotations, "source");
		if (!s)
			continue;
		ReshadeUniform ru;
		ru.name = "rsu" + std::to_string(i);
		ru.glName = "rs." + ru.name;
		ru.source = s->value.string_data;
		ru.offset = u.offset;
		ru.components = std::max(1, (int)u.type.rows * (int)u.type.cols);
		ru.isFloat = u.type.base == reshadefx::type::t_float;
		ru.isUnsigned = u.type.base == reshadefx::type::t_uint;
		ru.isBool = u.type.base == reshadefx::type::t_bool;
		if (const auto *a = FindAnnotation(u.annotations, "min")) ru.minValue = a->value.as_float[0];
		if (const auto *a = FindAnnotation(u.annotations, "max")) ru.maxValue = a->value.as_float[0];
		if (const auto *a = FindAnnotation(u.annotations, "step")) ru.step = a->value.as_float[0];
		out->uniforms.push_back(ru);
	}

	// ReShade enables the techniques a preset lists. Without a preset, run the ones marked enabled=true, else the first
	// technique that writes the screen (with the texture-only helper techniques before it): files like DOF.fx hold
	// several alternative techniques that must not run together.
	std::vector<const reshadefx::pass_info *> list;
	for (const auto &t : mod.techniques) {
		const reshadefx::annotation *en = FindAnnotation(t.annotations, "enabled");
		if (!en || en->value.as_int[0] == 0)
			continue;
		for (const auto &p : t.passes)
			list.push_back(&p);
	}
	if (list.empty()) {
		// Helper techniques that only fill textures (an auto focus pass, say) belong to the first technique that
		// writes the screen, so take everything up to and including that one.
		for (const auto &t : mod.techniques) {
			bool writesScreen = false;
			for (const auto &p : t.passes) {
				list.push_back(&p);
				if (p.render_target_names[0].empty())
					writesScreen = true;
			}
			if (writesScreen)
				break;
		}
	}

	for (const reshadefx::pass_info *pi : list) {
		ReshadePass pass;
		// Render targets of the pass. With several the pass is run once per target (see Generate).
		std::vector<int> targets;
		for (int k = 0; k < 8 && !pi->render_target_names[k].empty(); k++) {
			auto rt = texInfo.find(pi->render_target_names[k]);
			if (rt == texInfo.end() || rt->second.kind != Kind::RenderTarget) {
				out->error = "pass " + pi->ps_entry_point + " has an unsupported render target";
				return false;
			}
			targets.push_back(rt->second.index);
		}
		if (targets.empty())
			out->backBufferWrites++;
		pass.renderTarget = targets.empty() ? -1 : targets[0];
		pass.clear = pi->clear_render_targets;

		// Which samplers do the vertex and pixel shader really use, and which texture slot does each get?
		std::map<uint32_t, int> samplerSlots;
		{
			std::vector<std::pair<std::string, spv::ExecutionModel>> stages = { { pi->vs_entry_point, spv::ExecutionModelVertex }, { pi->ps_entry_point, spv::ExecutionModelFragment } };
			std::map<std::pair<int, int>, int> slotOf;  // (kind, index) -> slot
			int slots = 0;
			for (auto &st : stages) {
				spirv_cross::CompilerGLSL c(mod.spirv);
				c.set_entry_point(st.first, st.second);
				spirv_cross::ShaderResources res = c.get_shader_resources(c.get_active_interface_variables());
				for (const auto &si : res.sampled_images) {
					uint32_t binding = c.get_decoration(si.id, spv::DecorationBinding);
					if (samplerSlots.count(binding))
						continue;
					const reshadefx::sampler_info *sinfo = nullptr;
					for (const auto &s2 : mod.samplers)
						if (s2.binding == binding) { sinfo = &s2; break; }
					if (!sinfo) {
						out->error = "sampler binding " + std::to_string(binding) + " is unknown";
						return false;
					}
					auto it = texInfo.find(sinfo->texture_name);
					if (it == texInfo.end()) {
						out->error = "sampler " + sinfo->unique_name + " has no texture";
						return false;
					}
					const TexInfo &ti = it->second;
					const bool readsItself = ti.kind == Kind::RenderTarget && std::find(targets.begin(), targets.end(), ti.index) != targets.end();
					const std::pair<int, int> key = { (int)ti.kind, ti.index };
					auto sl = slotOf.find(key);
					int slot;
					if (sl != slotOf.end()) {
						slot = sl->second;
					} else {
						slot = slots++;
						if (slot >= kReshadeMaxTextures) {
							out->error = "pass " + pi->ps_entry_point + " needs more than " + std::to_string(kReshadeMaxTextures) + " textures at once (PPSSPP has " + std::to_string(kReshadeMaxTextures) + " texture slots)";
							return false;
						}
						slotOf[key] = slot;
						pass.slotKind[slot] = ti.kind == Kind::BackBuffer ? ReshadeSlot::BACK_BUFFER : ti.kind == Kind::Depth ? ReshadeSlot::DEPTH :
							ti.kind == Kind::Image ? ReshadeSlot::IMAGE : ReshadeSlot::RENDER_TARGET;
						pass.slotIndex[slot] = ti.index;
						pass.selfRead[slot] = readsItself;
						ReshadeSampler &ss = pass.slotSampler[slot];
						const uint32_t f = (uint32_t)sinfo->filter;
						ss.minLinear = (f & 0x10) != 0;
						ss.magLinear = (f & 0x04) != 0;
						ss.mipLinear = (f & 0x01) != 0;
						auto wrap = [](reshadefx::texture_address_mode m) {
							return m == reshadefx::texture_address_mode::wrap ? 1 : m == reshadefx::texture_address_mode::mirror ? 2 : 0;
						};
						ss.wrapU = wrap(sinfo->address_u);
						ss.wrapV = wrap(sinfo->address_v);
					}
					samplerSlots[binding] = slot;
				}
			}
		}

		std::string error;
		if (!Generate(mod.spirv, pi->vs_entry_point, spv::ExecutionModelVertex, target, glslVersion, samplerSlots, out->uniforms, &pass.vs, &error)) {
			out->error = "SPIRV-Cross: " + error;
			return false;
		}
		const int variants = std::max<int>(1, (int)targets.size());
		for (int k = 0; k < variants; k++) {
			ReshadePass variant = pass;
			variant.renderTarget = targets.empty() ? -1 : targets[k];
			if (!Generate(mod.spirv, pi->ps_entry_point, spv::ExecutionModelFragment, target, glslVersion, samplerSlots, out->uniforms, &variant.fs, &error, targets.size() > 1 ? k : -1)) {
				out->error = "SPIRV-Cross: " + error;
				return false;
			}
			out->passes.push_back(std::move(variant));
		}
	}
	if (out->backBufferWrites == 0) {
		out->error = "the shader never writes the screen";
		return false;
	}
	if (out->passes.empty()) {
		out->error = "the shader has no passes";
		return false;
	}
	return true;
}

void ReshadeCompositeShaders(ReshadeTarget target, int glslVersion, std::string *vs, std::string *fs) {
	if (target == ReshadeTarget::VULKAN) {
		*vs =
			"#version 450\n"
			"layout(location = 0) out vec2 vUv;\n"
			"void main() {\n"
			"  vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
			"  vUv = uv;\n"
			"  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
			"}\n";
		*fs =
			"#version 450\n"
			"layout(location = 0) in vec2 vUv;\n"
			"layout(location = 0) out vec4 outColor;\n"
			"layout(set = 0, binding = 1) uniform sampler2D sampler0;\n"
			"layout(set = 0, binding = 2) uniform sampler2D sampler1;\n"
			"layout(set = 0, binding = 3) uniform sampler2D sampler2;\n"
			"void main() {\n"
			"  outColor = mix(texture(sampler0, vUv), texture(sampler1, vUv), texture(sampler2, vUv).r);\n"
			"}\n";
		return;
	}
	const std::string header = "#version " + std::to_string(glslVersion) + (target == ReshadeTarget::GLSL_ES3 ? " es\nprecision highp float;\n" : "\n");
	*vs = header +
		"out vec2 vUv;\n"
		"void main() {\n"
		"  vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
		"  vUv = uv;\n"
		"  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	*fs = header +
		"in vec2 vUv;\n"
		"out vec4 outColor;\n"
		"uniform sampler2D sampler0;\n"
		"uniform sampler2D sampler1;\n"
		"uniform sampler2D sampler2;\n"
		"void main() {\n"
		"  outColor = mix(texture(sampler0, vUv), texture(sampler1, vUv), texture(sampler2, vUv).r);\n"
		"}\n";
}

void ReshadeFillUniforms(const ReshadeProgram &prog, const ReshadeFrameState &state, uint8_t *buffer) {
	static uint32_t rng = 0x12345678u;
	for (const ReshadeUniform &u : prog.uniforms) {
		if (u.offset + 4 > prog.uniformBufferSize)
			continue;
		float v[4] = { 0, 0, 0, 0 };
		if (u.source == "frametime") {
			v[0] = state.frameTimeMs;
		} else if (u.source == "framecount") {
			v[0] = (float)state.frame;
		} else if (u.source == "timer") {
			v[0] = (float)state.timerMs;
		} else if (u.source == "date") {
			time_t now = time(nullptr);
			tm lt{};
#ifdef _WIN32
			localtime_s(&lt, &now);
#else
			localtime_r(&now, &lt);
#endif
			v[0] = (float)(lt.tm_year + 1900);
			v[1] = (float)(lt.tm_mon + 1);
			v[2] = (float)lt.tm_mday;
			v[3] = (float)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
		} else if (u.source == "pingpong") {
			float range = std::max(u.maxValue - u.minValue, 1e-6f);
			float ph = std::fmod((float)(state.timerMs / 1000.0) * std::max(u.step, 0.001f), 2.0f * range);
			v[0] = u.minValue + (ph < range ? ph : 2.0f * range - ph);
			v[1] = ph < range ? 1.0f : -1.0f;
		} else if (u.source == "random") {
			rng = rng * 1664525u + 1013904223u;
			v[0] = u.minValue + (u.maxValue - u.minValue) * (float)(rng >> 8) / 16777215.0f;
		}  // mousepoint, mousebutton, key, overlay_open, ...: stay 0.
		for (int i = 0; i < u.components && u.offset + (uint32_t)(i + 1) * 4 <= prog.uniformBufferSize; i++) {
			float x = i < 4 ? v[i] : 0.0f;
			if (u.isFloat) {
				memcpy(buffer + u.offset + i * 4, &x, 4);
			} else if (u.isUnsigned || u.isBool) {
				uint32_t k = u.isBool ? (x != 0.0f ? 1u : 0u) : (uint32_t)x;
				memcpy(buffer + u.offset + i * 4, &k, 4);
			} else {
				int32_t k = (int32_t)x;
				memcpy(buffer + u.offset + i * 4, &k, 4);
			}
		}
	}
}
