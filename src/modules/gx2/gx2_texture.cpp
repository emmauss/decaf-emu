#include "gx2_format.h"
#include "gx2_texture.h"
#include "gpu/latte_registers.h"
#include "gpu/pm4_writer.h"

void
GX2InitTextureRegs(GX2Texture *texture)
{
   auto word0 = texture->regs.word0.value();
   auto word1 = texture->regs.word1.value();
   auto word4 = texture->regs.word4.value();
   auto word5 = texture->regs.word5.value();
   auto word6 = texture->regs.word6.value();

   // Minimum values
   if (!texture->viewNumMips) {
      texture->viewNumMips = 1;
   }

   if (!texture->viewNumSlices) {
      texture->viewNumSlices;
   }

   if (!texture->surface.width) {
      texture->surface.width = 1;
   }

   if (!texture->surface.height) {
      texture->surface.height = 1;
   }

   if (!texture->surface.depth) {
      texture->surface.depth = 1;
   }

   if (!texture->surface.mipLevels) {
      texture->surface.mipLevels = 1;
   }

   // Word 0
   word0.value = 0;
   word0.DIM = static_cast<latte::SQ_TEX_DIM>(texture->surface.dim & 0x7);
   word0.TILE_MODE = texture->surface.tileMode;

   if (texture->surface.use & GX2SurfaceUse::DepthBuffer) {
      word0.TILE_TYPE = 1;
   } else {
      word0.TILE_TYPE = 0;
   }

   auto formatType = static_cast<latte::SQ_DATA_FORMAT>(texture->surface.format & latte::FMT_MASK);
   auto elemSize = 4u;

   if (formatType < latte::FMT_BC1 || formatType > latte::FMT_BC5) {
      elemSize = 1;
   }

   word0.PITCH = ((elemSize * texture->surface.pitch) / 8) - 1;
   word0.TEX_WIDTH = texture->surface.width - 1;

   // Word 1
   word1.value = 0;
   word1.TEX_HEIGHT = texture->surface.height - 1;

   if (texture->surface.dim == GX2SurfaceDim::TextureCube) {
      word1.TEX_DEPTH = (texture->surface.depth / 6) - 1;
   } else if (texture->surface.dim == GX2SurfaceDim::Texture3D ||
              texture->surface.dim == GX2SurfaceDim::Texture2DMSAAArray ||
              texture->surface.dim == GX2SurfaceDim::Texture2DArray ||
              texture->surface.dim == GX2SurfaceDim::Texture1DArray) {
      word1.TEX_DEPTH = texture->surface.depth - 1;
   } else {
      word1.TEX_DEPTH = 0;
   }

   word1.DATA_FORMAT = formatType;

   // Word 4
   auto formatComp = latte::SQ_FORMAT_COMP_UNSIGNED;

   if (texture->surface.format & GX2AttribFormatFlags::SIGNED) {
      formatComp = latte::SQ_FORMAT_COMP_SIGNED;
   }

   word4.value = 0;
   word4.FORMAT_COMP_X = formatComp;
   word4.FORMAT_COMP_Y = formatComp;
   word4.FORMAT_COMP_Z = formatComp;
   word4.FORMAT_COMP_W = formatComp;

   if (texture->surface.format & GX2AttribFormatFlags::SCALED) {
      word4.NUM_FORMAT_ALL = latte::SQ_NUM_FORMAT_SCALED;
   } else if (texture->surface.format & GX2AttribFormatFlags::SIGNED) {
      word4.NUM_FORMAT_ALL = latte::SQ_NUM_FORMAT_INT;
   }

   if (texture->surface.format & GX2AttribFormatFlags::DEGAMMA) {
      word4.FORCE_DEGAMMA = 1;
   }

   word4.ENDIAN_SWAP = static_cast<latte::SQ_ENDIAN>(GX2GetSurfaceSwap(texture->surface.format));
   word4.REQUEST_SIZE = 2;

   word4.DST_SEL_X = static_cast<latte::SQ_SEL>((texture->compMap >> 24) & 0x7);
   word4.DST_SEL_Y = static_cast<latte::SQ_SEL>((texture->compMap >> 16) & 0x7);
   word4.DST_SEL_Z = static_cast<latte::SQ_SEL>((texture->compMap >> 8) & 0x7);
   word4.DST_SEL_W = static_cast<latte::SQ_SEL>(texture->compMap & 0x7);
   word4.BASE_LEVEL = texture->viewFirstMip;

   // Word 5
   word5.LAST_LEVEL = texture->viewFirstMip + texture->viewNumMips - 1;
   word5.BASE_ARRAY = texture->viewFirstSlice;
   word5.LAST_ARRAY = texture->viewFirstSlice + texture->viewNumSlices - 1;

   if (texture->surface.dim == GX2SurfaceDim::TextureCube && word1.TEX_DEPTH) {
      word5.YUV_CONV = 1;
   }

   // Word 6
   word6.MAX_ANISO_RATIO = 4;
   word6.PERF_MODULATION = 7;
   word6.TYPE = latte::SQ_TEX_VTX_VALID_TEXTURE;

   // Update big endian register in texture
   texture->regs.word0 = word0;
   texture->regs.word1 = word1;
   texture->regs.word4 = word4;
   texture->regs.word5 = word5;
   texture->regs.word6 = word6;
}

void
GX2SetPixelTexture(GX2Texture *texture, uint32_t unit)
{
   pm4::write(pm4::SetResourceTexture {
      (unit * 7) + latte::Register::SQ_TEX_RESOURCE_WORD0_0,
      texture->regs.word0,
      texture->regs.word1,
      texture->surface.image,
      texture->surface.mipmaps,
      texture->regs.word4,
      texture->regs.word5,
      texture->regs.word6,
   });
}