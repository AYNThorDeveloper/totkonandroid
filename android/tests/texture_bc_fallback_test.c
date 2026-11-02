#include "../nvn_vk_texture_stage.h"
#include "../../games/totk/third_party/d2r_gfx/gfx/nvn/include/nvn.h"
#include "../../games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/* The format table also defines a device query; this host test never calls it. */
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physical, VkFormat format,
                                         VkFormatProperties *properties) {
    (void)physical;
    (void)format;
    *properties = (VkFormatProperties){0};
}

int main(void) {
    nvn_texdesc desc = {
        .target = NVN_TEXTURE_TARGET_2D_ARRAY,
        .format = NVN_FORMAT_RGBA_DXT1,
        .width = 4, .height = 4, .depth = 2, .levels = 2,
        .swizzle_r = NVN_TEXTURE_SWIZZLE_R,
        .swizzle_g = NVN_TEXTURE_SWIZZLE_G,
        .swizzle_b = NVN_TEXTURE_SWIZZLE_B,
        .swizzle_a = NVN_TEXTURE_SWIZZLE_A,
    };
    nvn_texlayout layout;
    nvn_tex_layout(&desc, &layout);
    assert(layout.size && layout.size <= SIZE_MAX);
    uint8_t *source = calloc(1, (size_t)layout.size);
    assert(source);
    const uint8_t red[8] = {0x00, 0xf8, 0x00, 0xf8, 0, 0, 0, 0};
    const uint8_t blue[8] = {0x1f, 0x00, 0x1f, 0x00, 0, 0, 0, 0};
    for (unsigned mip = 0; mip < 2; ++mip) {
        assert(nvn_swizzle(&desc, mip, 0, red, 8, source) == 0);
        assert(nvn_swizzle(&desc, mip, 1, blue, 8, source) == 0);
    }

    NvnVkTextureStage direct = {0};
    assert(nvn_vk_texture_stage_with_bc_fallback(
        &desc, source, (size_t)layout.size, 1, &direct) == NVN_VK_STAGE_OK);
    assert(direct.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    assert(direct.region_count == 4);
    nvn_vk_texture_stage_destroy(&direct);

    NvnVkTextureStage fallback = {0};
    assert(nvn_vk_texture_stage_with_bc_fallback(
        &desc, source, (size_t)layout.size, 0, &fallback) == NVN_VK_STAGE_OK);
    assert(fallback.format == VK_FORMAT_R8G8B8A8_UNORM);
    assert(fallback.region_count == 4);
    for (unsigned i = 0; i < fallback.region_count; ++i) {
        const VkBufferImageCopy *region = &fallback.regions[i];
        assert(region->bufferOffset % 4 == 0);
        assert(region->imageSubresource.mipLevel == i / 2);
        assert(region->imageSubresource.baseArrayLayer == i % 2);
        assert(region->imageExtent.width == (i / 2 ? 2u : 4u));
        size_t offset = (size_t)region->bufferOffset;
        assert(offset + 4 <= fallback.data_size);
        const uint8_t *pixel = (const uint8_t *)fallback.data + offset;
        if (i % 2 == 0) {
            assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
        } else {
            assert(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255);
        }
    }
    nvn_vk_texture_stage_destroy(&fallback);
    free(source);

    desc.target = NVN_TEXTURE_TARGET_2D;
    desc.depth = 1;
    desc.levels = 1;
    const int formats[2] = {NVN_FORMAT_RGTC1_UNORM, NVN_FORMAT_RGTC2_UNORM};
    const VkFormat expected[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
    const uint8_t blocks[16] = {127, 127, 0, 0, 0, 0, 0, 0,
                                63, 63, 0, 0, 0, 0, 0, 0};
    for (unsigned i = 0; i < 2; ++i) {
        desc.format = formats[i];
        nvn_tex_layout(&desc, &layout);
        source = calloc(1, (size_t)layout.size);
        assert(source);
        assert(nvn_swizzle(&desc, 0, 0, blocks, i ? 16 : 8, source) == 0);
        assert(nvn_vk_texture_stage_with_bc_fallback(
            &desc, source, (size_t)layout.size, 0, &fallback) == NVN_VK_STAGE_OK);
        assert(fallback.format == expected[i]);
        assert(fallback.region_count == 1);
        assert(((uint8_t *)fallback.data)[0] == 127);
        if (i) assert(((uint8_t *)fallback.data)[1] == 63);
        nvn_vk_texture_stage_destroy(&fallback);
        free(source);
    }
    /* Packed HDR/10-bit texels must survive staging bit-for-bit, including
     * non-power-of-two sizes and mip offsets. Metal uses these NVN layouts. */
    const int packed[] = {NVN_FORMAT_RGB10A2, NVN_FORMAT_RGB10A2UI, NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGB9E5F};
    const VkFormat packed_vk[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    for (unsigned f = 0; f < 4; f++) {
        desc.format=packed[f]; desc.width=5; desc.height=3; desc.levels=3;
        nvn_tex_layout(&desc,&layout); source=calloc(1,(size_t)layout.size); assert(source);
        uint32_t texels[15];
        for (unsigned mip=0;mip<3;mip++) {
            unsigned w=5u>>mip; if(!w)w=1; unsigned h=3u>>mip;if(!h)h=1;
            for(unsigned j=0;j<w*h;j++) texels[j]=0x781e03c0u ^ (mip*16+j);
            assert(!nvn_swizzle(&desc,mip,0,texels,w*4,source));
        }
        NvnVkTextureStage staged;
        assert(nvn_vk_texture_stage(&desc,source,(size_t)layout.size,&staged)==NVN_VK_STAGE_OK);
        assert(staged.format==packed_vk[f] && staged.region_count==3);
        for(unsigned mip=0;mip<3;mip++) {
            VkBufferImageCopy *r=&staged.regions[mip];
            const uint32_t *pixels=(const uint32_t *)((const uint8_t *)staged.data+r->bufferOffset);
            for(unsigned j=0;j<r->imageExtent.width*r->imageExtent.height;j++) assert(pixels[j]==(0x781e03c0u^(mip*16+j)));
        }
        nvn_vk_texture_stage_destroy(&staged);free(source);
    }
    {   /* a packaged 3D texture whose first layout byte exceeds a 2D block height (Thor, 23 Oct: BC4 64x64x16) stages */
        nvn_texdesc d3 = {.target = NVN_TEXTURE_TARGET_3D, .format = NVN_FORMAT_RGTC1_UNORM, .width = 64, .height = 64, .depth = 16,
            .levels = 1, .swizzle_r = NVN_TEXTURE_SWIZZLE_R, .swizzle_g = NVN_TEXTURE_SWIZZLE_G,
            .swizzle_b = NVN_TEXTURE_SWIZZLE_B, .swizzle_a = NVN_TEXTURE_SWIZZLE_A};
        d3.pkg_block_h = 0x21;
        nvn_texlayout l3; nvn_tex_layout(&d3, &l3);
        uint8_t *src3 = calloc(1, (size_t)l3.size); assert(src3);
        NvnVkTextureStage s3;
        assert(nvn_vk_texture_stage_plan(&d3, src3, (size_t)l3.size, &s3) == NVN_VK_STAGE_OK);
        assert(s3.region_count == 1 && s3.regions[0].imageExtent.depth == 16);
        nvn_vk_texture_stage_destroy(&s3);
        d3.target = NVN_TEXTURE_TARGET_2D; d3.depth = 1;   /* a 2D block height above 5 is still refused */
        assert(nvn_vk_texture_stage_plan(&d3, src3, (size_t)l3.size, &s3) == NVN_VK_STAGE_INVALID_ARGUMENT);
        free(src3);
    }
    puts("texture staging: pass (BC fallbacks, packed HDR/10-bit mips, packaged 3D)");
    return 0;
}
