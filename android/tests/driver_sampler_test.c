#include <assert.h>
#include <math.h>
#include "../nvn_vk_driver_bank.h"
#include "../nvn_vk_sampler.h"
NvkState nvk_S;
static uint64_t u64(const uint8_t *p) { uint64_t x; memcpy(&x,p,8); return x; }
static uint32_t u32(const uint8_t *p) { uint32_t x; memcpy(&x,p,4); return x; }
int main(void) {
    uint8_t bytes[0xA00]; uint32_t grid[3]={13,17,19};
    nvk_S.ssbo[0][15].addr=0x123456789abcdef0ull; nvk_S.ssbo[0][15].size=4096;
    nvk_driver_bank_fill(bytes,0,0,grid);
    assert(u64(bytes+0x110+15*16)==0x123456789abcdef0ull && u32(bytes+0x110+15*16+8)==4096);
    float point; memcpy(&point,bytes+0x610,4); assert(point==2047.9375f);
    nvk_S.ubo[5][3].addr=0x8899aabbccddeeffull; nvk_S.ubo[5][3].size=128;
    nvk_driver_bank_fill(bytes,0,5,grid);
    assert(u64(bytes+0x210+3*16)==0x8899aabbccddeeffull && u32(bytes+0x210+3*16+8)==128);
    assert(!memcmp(bytes+0x200,grid,12));
    nvk_S.tex[1][127]=0x123; nvk_S.samp[1][127]=0x456; nvk_S.tex[1][3]=0x789;
    nvk_driver_bank_fill(bytes,2,1,grid);
    assert(u32(bytes+0x168+127*8)==0x123 && u32(bytes+0x568+127*8)==0x456 && u32(bytes+0x20+3*8)==0x789);
    samp_par p={.minf=NVN_MIN_FILTER_LINEAR,.magf=NVN_MAG_FILTER_LINEAR,.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,
        .wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,.lod_max=1000,.aniso=1};
    VkSamplerCreateInfo ci; VkSamplerReductionModeCreateInfo reduction; uint32_t requirements;
    assert(nvn_vk_sampler_map(&p,&ci,&reduction,&requirements)==NVN_VK_SAMPLER_OK);
    assert(ci.minLod==0 && ci.maxLod==0.25f && ci.mipmapMode==VK_SAMPLER_MIPMAP_MODE_NEAREST);
    p.minf=NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR;
    assert(nvn_vk_sampler_map(&p,&ci,&reduction,&requirements)==NVN_VK_SAMPLER_OK);
    assert(ci.maxLod==1000 && ci.mipmapMode==VK_SAMPLER_MIPMAP_MODE_LINEAR);
    puts("driver banks / samplers: pass (c0 tables, compute grid, c2 slots, non-mip filter)");
}
