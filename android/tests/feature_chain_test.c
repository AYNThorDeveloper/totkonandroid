#include "../nvn_vk_features.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    for (int view_on = 0; view_on < 2; ++view_on) for (int local_on = 0; local_on < 2; ++local_on) {
        VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR local = {
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
        VkPhysicalDeviceImage2DViewOf3DFeaturesEXT view = {
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT};
        VkPhysicalDeviceIndexTypeUint8FeaturesEXT required = {
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT,
            .pNext=nvk_optional_feature_chain(&view,&local,view_on,local_on)};
        unsigned seen_view=0,seen_local=0,n=0;
        for (VkBaseOutStructure *p=required.pNext;p;p=p->pNext) {
            assert(++n<=2);
            if(p->sType==view.sType){seen_view++;view.image2DViewOf3D=VK_TRUE;}
            else {assert(p->sType==local.sType);seen_local++;local.dynamicRenderingLocalRead=VK_TRUE;}
        }
        assert(seen_view==(unsigned)view_on && seen_local==(unsigned)local_on);
        assert(!!local.dynamicRenderingLocalRead==local_on);
    }
    puts("optional feature chain: pass (all four combinations, including local read without 2D-of-3D)");
}
