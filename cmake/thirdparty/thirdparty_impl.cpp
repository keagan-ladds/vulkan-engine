// Implementation translation unit for single-header libraries.
// Each of these headers contains its implementation behind a macro; defining
// the macro in exactly ONE .cpp file compiles the implementation once.

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
