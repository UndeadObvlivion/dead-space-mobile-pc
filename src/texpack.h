// Texture packs.
//
// Every texture the game uploads is identified by a hash of its decoded pixels (its GL name changes from
// run to run; its content does not). <game folder>/mods/textures/<any pack>/ may hold, for a hash H:
//   H.png      replacement colour texture (any size)
#pragma once
#include <string>
#include <vector>

#include "memory.h"

namespace texpack {

struct Image {
    int w = 0, h = 0;
    std::vector<u8> px;  // RGBA
};

// Scans <base>/mods/textures/*/ (packs in name order, later ones win). `dump`: also write every texture
// the game uploads to <base>/mods/textures/_dump/<hash>.png, for pack makers.
void init(const std::string& base, bool packs_enabled, bool dump);
bool packs_enabled();
int pack_files();  // files found (0 = no pack installed)

u32 hash(const u8* rgba, int w, int h);

// Called with each level-0 image the game uploads (RGBA). Notes its hash for the GL texture, dumps it if
// asked, and returns the pack's replacement colour image, or null.
const Image* on_upload(unsigned tex, const u8* rgba, int w, int h);
void on_delete(unsigned tex);
u32 hash_of(unsigned tex);  // 0 = unknown

bool load_png(const std::string& path, Image& out);
bool save_png(const std::string& path, const u8* rgba, int w, int h);

}  // namespace texpack
