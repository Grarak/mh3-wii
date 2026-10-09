#include "launcher_font.h"
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace VitaLauncher {
Font::Font() {
    ScePvfInitRec init{};init.maxNumFonts=1;
    init.allocFunc=[](void*,unsigned size)->void*{return std::malloc(size);};
    init.reallocFunc=[](void*,void* p,unsigned size)->void*{return std::realloc(p,size);};
    init.freeFunc=[](void*,void* p){std::free(p);};
    ScePvfError error=0;
    library=scePvfNewLib(&init,&error);
    if(!library || error)throw std::runtime_error("Cannot open the system font library");
    font=scePvfOpenDefaultLatinFontOnSharedMemory(library,&error);
    if(!font || error){scePvfDoneLib(library);library=nullptr;throw std::runtime_error("Cannot open the installed Latin system font");}
    scePvfSetResolution(library,72,72);
    scePvfSetCharSize(font,18,18);
}
Font::~Font(){if(font)scePvfClose(font);if(library)scePvfDoneLib(library);}
Font::Glyph& Font::Get(unsigned character) {
    if(character<32 || character>126)character='?';
    auto [it,fresh]=glyphs.try_emplace(character);
    if(!fresh)return it->second;
    ScePvfCharInfo info{};
    if(scePvfGetCharInfo(font,character,&info)<0){it->second.advance=10;return it->second;}
    std::array<uint8_t,64*64> bitmap{};
    ScePvfUserImageBufferRec image{};
    image.pixelFormat=SCE_PVF_USERIMAGE_DIRECT8;image.xPos64=8*64;image.yPos64=40*64;
    image.rect={64,64};image.bytesPerLine=64;image.buffer=bitmap.data();
    if(scePvfGetCharGlyphImage(font,character,&image)<0){it->second.advance=10;return it->second;}
    it->second.advance=info.glyphMetrics.horizontalAdvance64/64.f;
    if(it->second.advance<1)it->second.advance=6;
    // CPU-only glyph descriptions; all final vertices/indices are allocated
    // directly from the frame's CDRAM slot. No GPU buffer in main RAM.
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;) {
        if(bitmap[y*64+x]<96){++x;continue;}
        const unsigned start=x;while(x<64 && bitmap[y*64+x]>=96)++x;
        it->second.runs.push_back({float(start)-8,float(y)-40,float(x-start),1,0});
    }
    return it->second;
}
void Font::Text(std::vector<Rectangle>& rects,float x,float y,std::string_view text,uint32_t color,float maxWidth) {
    const float end=x+maxWidth;
    for(unsigned char c:text) {
        auto& glyph=Get(c);
        if(x+glyph.advance>end)break;
        for(const auto& run:glyph.runs)rects.push_back({x+run.x,y+18+run.y,run.w,run.h,color});
        x+=glyph.advance;
    }
}
}
