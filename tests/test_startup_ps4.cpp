#define PORTAL_HOST
#include "../src/startup_ps4.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

static std::vector<OrbisSysModuleInternal> loaded;
static OrbisSysModuleInternal failure=ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT;
static bool rejectModule=false;

extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal module){
    loaded.push_back(module);
    return rejectModule&&module==failure?0x80020008u:0;
}

int main(){
    // A system-profile title starts without VideoOut. Success must guarantee
    // that it was loaded before main reaches Display::init().
    assert(loadStartupModules());
    assert(std::find(loaded.begin(),loaded.end(),ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT)!=loaded.end());
    assert(std::find(loaded.begin(),loaded.end(),ORBIS_SYSMODULE_INTERNAL_PAD)!=loaded.end());
    assert(std::find(loaded.begin(),loaded.end(),ORBIS_SYSMODULE_INTERNAL_USER_SERVICE)!=loaded.end());
    assert(std::find(loaded.begin(),loaded.end(),ORBIS_SYSMODULE_INTERNAL_NETCTL)!=loaded.end());
    const auto required=loaded;
    for(auto module:required){
        loaded.clear();
        failure=module;
        rejectModule=true;
        assert(!loadStartupModules());
        assert(loaded.back()==module);
        rejectModule=false;
        loaded.clear();
        assert(loadStartupModules()&&loaded==required);
    }
    std::cout<<"Startup dependencies load before UI use; failed loads stop startup and permit retry.\n";
}
