// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.


// Postprocessing shader manager

#include <string>
#include <vector>
#include <algorithm>

#include "Common/Log.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/File/DirListing.h"
#include "Common/File/VFS/VFS.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Common/File/FileUtil.h"
#include "Core/Config.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/GPU/thin3d.h"
#include "Common/StringUtils.h"

#include "Core/System.h"
#include "GPU/Common/PostShader.h"

static std::vector<ShaderInfo> shaderInfo;
// Okay, not really "post" shaders, but related.
static std::vector<TextureShaderInfo> textureShaderInfo;

static Draw::GPUVendor VendorFromString(const std::string &vendor) {
	// TODO: This should probably be a function somewhere.
	if (vendor == "ARM") {
		return Draw::GPUVendor::VENDOR_ARM;
	} else if (vendor == "Qualcomm") {
		return Draw::GPUVendor::VENDOR_QUALCOMM;
	} else if (vendor == "IMGTEC") {
		return Draw::GPUVendor::VENDOR_IMGTEC;
	} else if (vendor == "NVIDIA") {
		return Draw::GPUVendor::VENDOR_NVIDIA;
	} else if (vendor == "AMD") {
		return Draw::GPUVendor::VENDOR_AMD;
	} else if (vendor == "Broadcom") {
		return Draw::GPUVendor::VENDOR_BROADCOM;
	} else if (vendor == "Apple") {
		return Draw::GPUVendor::VENDOR_APPLE;
	} else if (vendor == "Intel") {
		return Draw::GPUVendor::VENDOR_INTEL;
	} else if (vendor == "Mesa") {
		return Draw::GPUVendor::VENDOR_MESA;
	} else if (vendor == "Vivante") {
		return Draw::GPUVendor::VENDOR_VIVANTE;
	}
	return Draw::GPUVendor::VENDOR_UNKNOWN;
}

// Scans the directories for shader ini files and collects info about all the shaders found.

void LoadPostShaderInfo(Draw::DrawContext *draw, const std::vector<Path> &directories) {
	std::vector<ShaderInfo> notVisible;

	Draw::GPUVendor gpuVendor = Draw::GPUVendor::VENDOR_UNKNOWN;
	if (draw) {
		gpuVendor = draw->GetDeviceCaps().vendor;
	}

	shaderInfo.clear();
	textureShaderInfo.clear();

	auto appendShader = [&](const ShaderInfo &info) {
		auto beginErase = std::remove(shaderInfo.begin(), shaderInfo.end(), info.name);
		if (beginErase != shaderInfo.end()) {
			shaderInfo.erase(beginErase, shaderInfo.end());
		}
		shaderInfo.push_back(info);
	};

	auto appendTextureShader = [&](const TextureShaderInfo &info) {
		auto beginErase = std::remove(textureShaderInfo.begin(), textureShaderInfo.end(), info.name);
		if (beginErase != textureShaderInfo.end()) {
			textureShaderInfo.erase(beginErase, textureShaderInfo.end());
		}
		textureShaderInfo.push_back(info);
	};

	for (size_t d = 0; d < directories.size(); d++) {
		std::vector<File::FileInfo> fileInfo;
		g_VFS.GetFileListing(directories[d].c_str(), &fileInfo, "ini:");

		if (fileInfo.empty()) {
			File::GetFilesInDir(directories[d], &fileInfo, "ini:");
		}

		for (size_t f = 0; f < fileInfo.size(); f++) {
			IniFile ini;
			bool success = false;
			if (fileInfo[f].isDirectory)
				continue;

			Path name = fileInfo[f].fullName;
			Path path = directories[d];
			// Hack around Android VFS path bug. really need to redesign this.
			if (name.ToString().substr(0, 7) == "assets/")
				name = Path(name.ToString().substr(7));
			if (path.ToString().substr(0, 7) == "assets/")
				path = Path(path.ToString().substr(7));

			if (ini.LoadFromVFS(g_VFS, name.ToString()) || ini.Load(fileInfo[f].fullName)) {
				success = true;
				// vsh load. meh.
			}

			if (!success)
				continue;

			// Alright, let's loop through the sections and see if any is a shader.
			for (size_t i = 0; i < ini.Sections().size(); i++) {
				Section &section = *(ini.Sections()[i].get());
				std::string shaderType = "render";
				section.Get("Type", &shaderType);

				std::vector<std::string> vendorBlacklist;
				section.Get("VendorBlacklist", &vendorBlacklist);
				bool skipped = false;
				for (auto &item : vendorBlacklist) {
					const Draw::GPUVendor blacklistedVendor = VendorFromString(item);
					if (blacklistedVendor == gpuVendor && blacklistedVendor != Draw::GPUVendor::VENDOR_UNKNOWN) {
						skipped = true;
						break;
					}
				}
				if (skipped) {
					continue;
				}

				if (section.HasKey("Fragment") && section.HasKey("Vertex") &&
					(strncasecmp(shaderType.c_str(), "render", shaderType.size()) == 0 ||
					 strncasecmp(shaderType.c_str(), "StereoToMono", shaderType.size()) == 0)) {
					// Valid shader!
					ShaderInfo info{};
					std::string temp;
					info.section = section.name();
					info.name = section.name();
					info.visible = true;
					section.Get("Name", &info.name);
					section.Get("Parent", &info.parent);
					section.Get("Visible", &info.visible);
					temp.clear();
					section.Get("Fragment", &temp);
					info.fragmentShaderFile = path / temp;
					temp.clear();
					section.Get("Vertex", &temp);
					info.vertexShaderFile = path / temp;
					section.Get("OutputResolution", &info.outputResolution);
					section.Get("Upscaling", &info.isUpscalingFilter);
					section.Get("SSAA", &info.SSAAFilterLevel);
					if (info.SSAAFilterLevel < 0 || info.SSAAFilterLevel > 8) {
						// It multiplies the render resolution, and shader inis come from downloads -
						// the neighbouring texture-shader "Scale" is bounded for the same reason.
						WARN_LOG(Log::G3D, "Ignoring out-of-range SSAA level %d in shader '%s'", info.SSAAFilterLevel, info.section.c_str());
						info.SSAAFilterLevel = 0;
					}
					section.Get("60fps", &info.requires60fps);
					section.Get("UsePreviousFrame", &info.usePreviousFrame);

					if (info.parent == "Off")
						info.parent.clear();

					if (strncasecmp(shaderType.c_str(), "stereotomono", shaderType.size()) == 0) {
						info.isStereo = true;
						info.isUpscalingFilter = false;
						info.parent.clear();
					}

					for (size_t i = 0; i < ARRAY_SIZE(info.settings); ++i) {
						auto &setting = info.settings[i];
						setting.name.clear();
						setting.value = 0.0;
						setting.minValue = -1.0f;
						setting.maxValue = 1.0f;
						setting.step = 0.01f;
						section.Get(StringFromFormat("SettingName%d", i + 1).c_str(), &setting.name);
						section.Get(StringFromFormat("SettingDefaultValue%d", i + 1).c_str(), &setting.value);
						section.Get(StringFromFormat("SettingMinValue%d", i + 1).c_str(), &setting.minValue);
						section.Get(StringFromFormat("SettingMaxValue%d", i + 1).c_str(), &setting.maxValue);
						section.Get(StringFromFormat("SettingStep%d", i + 1).c_str(), &setting.step);
					}

					// Let's ignore shaders we can't support. TODO: Not a very good check
					if (gl_extensions.IsGLES && !gl_extensions.GLES3) {
						bool requiresIntegerSupport = false;
						section.Get("RequiresIntSupport", &requiresIntegerSupport);
						if (requiresIntegerSupport)
							continue;
					}

					if (info.visible) {
						appendShader(info);
					} else {
						notVisible.push_back(info);
					}
				} else if (section.HasKey("Compute") && strncasecmp(shaderType.c_str(), "texture", shaderType.size()) == 0) {
					// This is a texture shader.
					TextureShaderInfo info{};
					std::string temp;
					info.section = section.name();
					info.name = section.name();
					info.scaleFactor = 0;
					section.Get("Name", &info.name);
					section.Get("Scale", &info.scaleFactor);
					bool hidden = false;
					section.Get("Hidden", &info.hidden);
					std::string cbufferFilename;
					if (section.Get("ConstantBuffer", &cbufferFilename)) {
						Path cbufferPath = path / cbufferFilename;
						info.constantBuffer = cbufferPath;
					}
					if (section.Get("Compute", &temp)) {
						info.computeShaderFile = path / temp;
						info.computeShaderFiles.push_back(info.computeShaderFile);
						for (int computeIndex = 2; computeIndex <= 4; ++computeIndex) {
							temp.clear();
							if (section.Get(StringFromFormat("Compute%d", computeIndex).c_str(), &temp)) {
								info.computeShaderFiles.push_back(path / temp);
							}
						}
						if (info.scaleFactor >= 2 && info.scaleFactor < 8) {
							appendTextureShader(info);
						}
					} else {
						ERROR_LOG(Log::G3D, "Compute field missing for compute shader");
					}
				} else if (!section.name().empty()) {
					WARN_LOG(Log::G3D, "Unrecognized shader type '%s' or invalid shader in section '%s'", shaderType.c_str(), section.name().c_str());
				}
			}
		}
	}

	// Sort shaders alphabetically.
	std::sort(shaderInfo.begin(), shaderInfo.end());
	std::sort(textureShaderInfo.begin(), textureShaderInfo.end());

	ShaderInfo off{};
	off.visible = true;
	off.name = "Off";
	off.section = "Off";
	for (size_t i = 0; i < ARRAY_SIZE(off.settings); ++i) {
		off.settings[i].name.clear();
		off.settings[i].value = 0.0f;
		off.settings[i].minValue = -1.0f;
		off.settings[i].maxValue = 1.0f;
		off.settings[i].step = 0.01f;
	}

	TextureShaderInfo textureOff{};
	textureOff.name = "Off";
	textureOff.section = "Off";
	textureShaderInfo.insert(textureShaderInfo.begin(), textureOff);

	// We always want the not visible ones at the end.  Makes menus easier.
	shaderInfo.reserve(notVisible.size() + 1);
	shaderInfo.insert(shaderInfo.begin(), off);
	for (const auto &info : notVisible) {
		appendShader(info);
	}
}

Path GetReshadeShadersDir() {
	return g_Config.internalDataDirectory / "reshade" / "Shaders";
}

Path GetReshadeTexturesDir() {
	return g_Config.internalDataDirectory / "reshade" / "Textures";
}

static bool HasExtension(std::string_view name, std::initializer_list<const char *> exts) {
	size_t dot = name.find_last_of('.');
	if (dot == std::string_view::npos)
		return false;
	std::string ext(name.substr(dot + 1));
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
	for (const char *e : exts)
		if (ext == e)
			return true;
	return false;
}

// ReShade.fxh and ReShadeUI.fxh are included by nearly every .fx, so they travel with the app.
static void EnsureReshadeCoreFiles() {
	const Path dir = GetReshadeShadersDir();
	for (const char *name : { "ReShade.fxh", "ReShadeUI.fxh" }) {
		const Path dest = dir / name;
		if (File::Exists(dest))
			continue;
		size_t size = 0;
		uint8_t *data = g_VFS.ReadFile((std::string("shaders/reshade/") + name).c_str(), &size);
		if (!data)
			continue;
		File::CreateFullPath(dir);
		File::WriteDataToFile(false, data, size, dest);
		delete[] data;
	}
}

// Adds every .fx in `dir` (and one level of subfolders) to the shader list.
static void ScanReshadeDir(const Path &dir, const std::string &prefix, int depth, std::vector<ShaderInfo> *out) {
	std::vector<File::FileInfo> files;
	if (!File::GetFilesInDir(dir, &files, nullptr))
		return;
	for (const File::FileInfo &f : files) {
		if (f.isDirectory) {
			if (depth < 1)
				ScanReshadeDir(f.fullName, prefix + f.name + "/", depth + 1, out);
			continue;
		}
		if (!HasExtension(f.name, { "fx" }))
			continue;
		ShaderInfo info{};
		info.reshadeFile = f.fullName;
		info.section = "reshade/" + prefix + f.name;
		info.name = prefix + f.name.substr(0, f.name.find_last_of('.')) + " (ReShade)";
		info.visible = true;
		info.outputResolution = false;
		for (size_t i = 0; i < ARRAY_SIZE(info.settings); ++i) {
			info.settings[i].name.clear();
			info.settings[i].value = 0.0f;
			info.settings[i].minValue = -1.0f;
			info.settings[i].maxValue = 1.0f;
			info.settings[i].step = 0.01f;
		}
		out->push_back(info);
	}
}

// Scans the directories for shader ini files and collects info about all the shaders found.
void ReloadAllPostShaderInfo(Draw::DrawContext *draw) {
	std::vector<Path> directories;
	directories.push_back(Path("shaders"));  // For VFS
	directories.push_back(GetSysDirectory(DIRECTORY_CUSTOM_SHADERS));
	LoadPostShaderInfo(draw, directories);

	// ReShade FX shaders. They go after the ini shaders, before the hidden ones, and keep the list sorted.
	EnsureReshadeCoreFiles();
	std::vector<ShaderInfo> reshade;
	ScanReshadeDir(GetReshadeShadersDir(), "", 0, &reshade);
	if (!reshade.empty()) {
		std::sort(reshade.begin(), reshade.end());
		for (const ShaderInfo &info : reshade) {
			auto beginErase = std::remove(shaderInfo.begin(), shaderInfo.end(), info.name);
			shaderInfo.erase(beginErase, shaderInfo.end());
			// Insert after "Off" and the visible ini shaders: before the first not visible one.
			auto pos = std::find_if(shaderInfo.begin(), shaderInfo.end(), [](const ShaderInfo &i) { return !i.visible; });
			shaderInfo.insert(pos, info);
		}
	}
}

static bool ImportReshadeEntry(const std::string &name, const uint8_t *data, size_t size, int *effects, int *includes, int *textures) {
	// Keep sub-paths below Shaders/ and Textures/, otherwise sort by extension. Never leave the reshade folder.
	std::string path = name;
	std::replace(path.begin(), path.end(), '\\', '/');
	std::vector<std::string> parts;
	SplitString(path, '/', parts);
	std::vector<std::string> clean;
	for (const std::string &part : parts) {
		if (part == "..")
			return false;
		if (!part.empty() && part != ".")
			clean.push_back(part);
	}
	if (clean.empty())
		return false;

	int shadersIndex = -1, texturesIndex = -1;
	for (int i = 0; i < (int)clean.size() - 1; i++) {
		if (equalsNoCase(clean[i], "Shaders")) shadersIndex = i;
		if (equalsNoCase(clean[i], "Textures")) texturesIndex = i;
	}
	const std::string &leaf = clean.back();
	const bool isFx = HasExtension(leaf, { "fx" });
	const bool isInclude = HasExtension(leaf, { "fxh", "h", "inc" });
	const bool isImage = HasExtension(leaf, { "png", "jpg", "jpeg", "bmp", "tga" });
	if (!isFx && !isInclude && !isImage)
		return false;

	Path dest;
	if (texturesIndex >= 0 && texturesIndex > shadersIndex) {
		dest = GetReshadeTexturesDir();
		for (size_t i = texturesIndex + 1; i < clean.size(); i++) dest = dest / clean[i];
	} else if (shadersIndex >= 0) {
		dest = GetReshadeShadersDir();
		for (size_t i = shadersIndex + 1; i < clean.size(); i++) dest = dest / clean[i];
	} else {
		dest = (isImage ? GetReshadeTexturesDir() : GetReshadeShadersDir()) / leaf;
	}
	File::CreateFullPath(dest.NavigateUp());
	if (!File::WriteDataToFile(false, data, size, dest))
		return false;
	if (isFx) (*effects)++;
	else if (isImage) (*textures)++;
	else (*includes)++;
	return true;
}

static void ImportReshadeZipDir(ZipFileReader *zip, const std::string &dir, int depth, int *effects, int *includes, int *textures) {
	std::vector<File::FileInfo> listing;
	if (!zip->GetFileListing(dir, &listing, nullptr) || depth > 8)
		return;
	for (const File::FileInfo &f : listing) {
		const std::string full = dir.empty() ? f.name : dir + f.name;
		if (f.isDirectory) {
			ImportReshadeZipDir(zip, full + "/", depth + 1, effects, includes, textures);
			continue;
		}
		size_t size = 0;
		uint8_t *data = zip->ReadFile(full, &size);
		if (!data)
			continue;
		ImportReshadeEntry(full, data, size, effects, includes, textures);
		delete[] data;
	}
}

bool ImportReshadeFiles(const Path &source, std::string *message) {
	int effects = 0, includes = 0, textures = 0;

	// Content URIs (Android's file picker) don't have a usable extension, so try it as a zip first.
	std::unique_ptr<ZipFileReader> zip(ZipFileReader::Create(source, "", false));
	if (zip) {
		ImportReshadeZipDir(zip.get(), "", 0, &effects, &includes, &textures);
	} else {
		std::string data;
		if (!File::ReadBinaryFileToString(source, &data)) {
			*message = "Couldn't read the file";
			return false;
		}
		ImportReshadeEntry(source.GetFilename(), (const uint8_t *)data.data(), data.size(), &effects, &includes, &textures);
	}
	if (effects + includes + textures == 0) {
		*message = "No ReShade shaders (.fx, .fxh) or textures found";
		return false;
	}
	*message = StringFromFormat("%d shaders, %d include files, %d textures", effects, includes, textures);
	return true;
}

void RemoveUnknownPostShaders(std::vector<std::string> *names) {
	for (auto iter = names->begin(); iter != names->end(); ) {
		if (GetPostShaderInfo(*iter) == nullptr) {
			iter = names->erase(iter);
		} else {
			++iter;
		}
	}
}

const ShaderInfo *GetPostShaderInfo(std::string_view name) {
	for (size_t i = 0; i < shaderInfo.size(); i++) {
		if (shaderInfo[i].section == name)
			return &shaderInfo[i];
	}
	return nullptr;
}

std::vector<const ShaderInfo *> GetPostShaderChain(const std::string &name) {
	std::vector<const ShaderInfo *> backwards;
	const ShaderInfo *shaderInfo = GetPostShaderInfo(name);
	while (shaderInfo) {
		backwards.push_back(shaderInfo);

		if (!shaderInfo->parent.empty()) {
			shaderInfo = GetPostShaderInfo(shaderInfo->parent);
		} else {
			shaderInfo = nullptr;
		}
		auto dup = std::find(backwards.begin(), backwards.end(), shaderInfo);
		if (dup != backwards.end()) {
			// Don't loop forever.
			break;
		}
	}

	if (!backwards.empty())
		std::reverse(backwards.begin(), backwards.end());
	// Not backwards anymore.
	return backwards;
}

std::vector<const ShaderInfo *> GetFullPostShadersChain(const std::vector<std::string> &names) {
	std::vector<const ShaderInfo *> fullChain;
	for (const auto &shaderName : names) {
		const auto &shaderChain = GetPostShaderChain(shaderName);
		fullChain.insert(fullChain.end(), shaderChain.begin(), shaderChain.end());
	}
	return fullChain;
}

bool PostShaderChainRequires60FPS(const std::vector<const ShaderInfo *> &chain) {
	for (auto shaderInfo : chain) {
		if (shaderInfo->requires60fps)
			return true;
	}
	return false;
}

const std::vector<ShaderInfo> &GetAllPostShaderInfo() {
	return shaderInfo;
}

const TextureShaderInfo *GetTextureShaderInfo(std::string_view name) {
	for (auto &info : textureShaderInfo) {
		if (info.section == name) {
			return &info;
		}
	}
	return nullptr;
}
const std::vector<TextureShaderInfo> &GetAllTextureShaderInfo() {
	return textureShaderInfo;
}

void FixPostShaderOrder(std::vector<std::string> *names) {
	// There's one rule only that we enforce - only one shader can use UsePreviousFrame,
	// and it has to be the last one. So we simply remove any we find from the list,
	// and then append it to the end if there is one.
	std::string prevFrameShader;
	for (auto iter = names->begin(); iter != names->end(); ) {
		const ShaderInfo *info = GetPostShaderInfo(*iter);
		if (info) {
			if (info->usePreviousFrame) {
				prevFrameShader = *iter;
				iter = names->erase(iter++);
				continue;
			}
		}
		++iter;
	}

	if (!prevFrameShader.empty()) {
		names->push_back(prevFrameShader);
	}
}
