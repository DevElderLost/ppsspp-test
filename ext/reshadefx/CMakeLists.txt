# ReShade FX front end (preprocessor + parser + SPIR-V codegen), the copy vkBasalt vendors (BSD-3 / zlib, see
# LICENSE.md and LICENSE.vkBasalt.txt). GPU/Common/ReshadeEffect.cpp turns a .fx file into per-pass GLSL with it.
cmake_minimum_required(VERSION 3.10)
project(reshadefx CXX)

add_library(reshadefx STATIC
	effect_codegen_spirv.cpp
	effect_expression.cpp
	effect_lexer.cpp
	effect_parser.cpp
	effect_preprocessor.cpp
	effect_symbol_table.cpp
)
set_target_properties(reshadefx PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
# <spirv.hpp> and <GLSL.std.450.h> come from the SPIRV-Cross checkout.
target_include_directories(reshadefx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../SPIRV-Cross)
if(NOT MSVC)
	target_compile_options(reshadefx PRIVATE -w)
endif()
