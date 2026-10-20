// ReShade FX (.fx) shaders as post-processing passes.
//
// ReshadeCompile() runs the vendored reshadefx front end (ext/reshadefx, the copy vkBasalt ships) on a .fx file and
// turns every pass of its techniques into a vertex + fragment GLSL pair for the active backend via SPIRV-Cross, so
// PresentationCommon can build them like any other post shader. This file does not depend on thin3d, only on the
// target language, which keeps it testable on its own.
//
// What a pass may use: up to eight textures at once (kReshadeMaxTextures, the texture slots of the post shader pipelines): the
// back buffer (ReShade::BackBuffer), the game's depth buffer (ReShade::DepthBuffer), textures loaded from files
// (`source = "lut.png"`) and the ReShade textures other passes render into. Render-target textures can be RGBA8 or
// RGBA16F (every float format becomes RGBA16F) and have mipmaps. Uniforms with a `source` annotation (frametime,
// framecount, timer, pingpong, random, date) are updated per frame, all others run on their declared defaults
// (reshadefx turns them into specialization constants). Rejected with an error message: passes with more than one
// render target, more than eight textures, and storage images.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

constexpr int kReshadeMaxTextures = 8;  // must not exceed Draw::MAX_TEXTURE_SLOTS

enum class ReshadeTarget {
	GLSL_ES3,      // OpenGL ES 3.x, uniforms as plain uniforms, samplers named sampler0..7.
	GLSL_DESKTOP,  // Desktop OpenGL 3.3+, same conventions.
	VULKAN,        // Vulkan GLSL 450, uniform block at set 0 binding 0, samplers at set 0 bindings 1..8.
};

enum class ReshadeFormat {
	RGBA8,
	RGBA16F,
};

// A render-target texture of the effect.
struct ReshadeTexture {
	int width = 1;
	int height = 1;
	int levels = 1;  // mip levels, regenerated after every pass that renders into it
	ReshadeFormat format = ReshadeFormat::RGBA8;
};

// Sampler state of a ReShade sampler.
struct ReshadeSampler {
	bool minLinear = true;
	bool magLinear = true;
	bool mipLinear = true;
	int wrapU = 0;  // 0 clamp, 1 repeat, 2 mirror
	int wrapV = 0;
};

enum class ReshadeSlot {
	NONE,
	BACK_BUFFER,
	IMAGE,         // ReshadeProgram::images[index]
	RENDER_TARGET, // ReshadeProgram::textures[index]
	DEPTH,         // the depth buffer of the displayed framebuffer
};

struct ReshadeImage {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;  // width * height * 4
};

// One member of the uniform block: a `source` annotated uniform.
struct ReshadeUniform {
	std::string name;    // name used in the generated GLSL (a member of the uniform "rs" on GL)
	std::string glName;  // "rs." + name: what glGetUniformLocation wants
	std::string source;  // "frametime", "framecount", "timer", "pingpong", "random", "date", ...
	uint32_t offset = 0;
	int components = 1;
	bool isFloat = true;  // int/uint/bool members are only supported on Vulkan (GL cannot set them through the uniform desc)
	bool isUnsigned = false;
	bool isBool = false;
	float minValue = 0.0f;
	float maxValue = 1.0f;
	float step = 1.0f;
};

struct ReshadePass {
	std::string vs;
	std::string fs;
	ReshadeSlot slotKind[kReshadeMaxTextures];
	int slotIndex[kReshadeMaxTextures];
	ReshadeSampler slotSampler[kReshadeMaxTextures];
	ReshadePass() {
		for (int i = 0; i < kReshadeMaxTextures; i++) {
			slotKind[i] = ReshadeSlot::NONE;
			slotIndex[i] = -1;
		}
	}
	// The pass reads the texture it renders into (a feedback pass): the slot has to see a copy made before the pass.
	bool selfRead[kReshadeMaxTextures] = {};
	int renderTarget = -1;  // index into ReshadeProgram::textures, -1 = writes the (next) back buffer
	bool clear = false;     // ClearRenderTargets
};

struct ReshadeProgram {
	std::vector<ReshadePass> passes;
	std::vector<ReshadeImage> images;
	std::vector<ReshadeTexture> textures;  // render targets
	int backBufferWrites = 0;              // passes that write the back buffer (the last one is the effect's output)
	std::vector<ReshadeUniform> uniforms;
	uint32_t uniformBufferSize = 16;  // bytes, multiple of 16
	std::string error;                // set when ReshadeCompile fails
	std::string warning;              // texture files that were not found (the textures stay black)
};

struct ReshadeFrameState {
	double timerMs = 0.0;   // since the effect was built
	float frameTimeMs = 0.0f;
	uint32_t frame = 0;
};

// Compiles `fxPath`. `includeDir` is where ReShade.fxh and friends live, `textureDir` is searched for `source` images.
// width / height are what BUFFER_WIDTH / BUFFER_HEIGHT expand to. glslVersion is only used for the GL targets (300 for
// ES 3.0, 330 for desktop). Blocking and slow-ish (it parses the whole shader), call it when the effect is chosen.
bool ReshadeCompile(const std::string &fxPath, const std::string &includeDir, const std::string &textureDir,
	int width, int height, ReshadeTarget target, int glslVersion, ReshadeProgram *out);

// Vertex and fragment shader of the pass that puts the original image back where the UI mask is white:
//   out = mix(effect (texture slot 0), original (slot 1), mask.r (slot 2))
void ReshadeCompositeShaders(ReshadeTarget target, int glslVersion, std::string *vs, std::string *fs);

// Loads a PNG/JPG/BMP/TGA as RGBA8 at its own size (the UI mask, see PresentationCommon).
bool ReshadeLoadMask(const std::string &path, ReshadeImage *out);

// Fills the uniform block (prog.uniformBufferSize bytes) for the current frame.
void ReshadeFillUniforms(const ReshadeProgram &prog, const ReshadeFrameState &state, uint8_t *buffer);
