// Implementation translation unit for single-header libraries.
// Each of these headers contains its implementation behind a macro; defining
// the macro in exactly ONE .cpp file compiles the implementation once.

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

// Ogg Vorbis playback (cooked audio is .ogg): miniaudio decodes Vorbis with
// its bundled stb_vorbis, declared before and implemented after miniaudio.
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
