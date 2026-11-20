#pragma once
#include "install_log.h"
#include <orbis/Sysmodule.h>

// The installer SELF profile does not preload the regular title's UI modules.
// Load dependencies before creating the display, input or download workers.
inline bool loadStartupModules(){
    struct Module { OrbisSysModuleInternal id; const char* name; };
    const Module modules[]={
        {ORBIS_SYSMODULE_INTERNAL_SYSCORE,"SysCore"},
        {ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE,"SystemService"},
        {ORBIS_SYSMODULE_INTERNAL_USER_SERVICE,"UserService"},
        {ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT,"VideoOut"},
        {ORBIS_SYSMODULE_INTERNAL_PAD,"Pad"},
        {ORBIS_SYSMODULE_INTERNAL_NET,"Net"},
        {ORBIS_SYSMODULE_INTERNAL_NETCTL,"NetCtl"}
    };
    installLog("NULL_store 1.0 startup: loading application dependencies");
    for(const auto& module:modules){
        int result=(int)sceSysmoduleLoadModuleInternal(module.id);
        installLog("Startup module %s id=0x%08X result=0x%08X",
                   module.name,(unsigned)module.id,(unsigned)result);
        if(result!=0){
            installLog("Startup stopped before using unavailable module %s",module.name);
            return false;
        }
    }
    installLog("Startup dependencies ready");
    return true;
}
