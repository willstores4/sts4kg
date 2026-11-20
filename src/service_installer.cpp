#include "display_ps4.h"
#include "data_root.h"
#include "service_start_ps4.h"
#include "service_protocol.h"
#include "network_ps4.h"
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <pthread.h>
#include <sys/stat.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;static bool busy=false;static std::string result="Press X to install or start the optional service.";
static void* install(void*){std::string error;bool ok=portalServiceInstallAndLaunch(error);pthread_mutex_lock(&mutex);result=ok?"Service connected. You can close this setup app and use NULL_store.":error;busy=false;pthread_mutex_unlock(&mutex);return 0;}
int main(){mkdir(nullStoreDataRoot(),0777);portalNetworkInit();Display display;if(!display.init())return 1;Canvas canvas("/app0/assets");const auto logo=loadBitmapRGBA("/app0/assets/brand/nullstore-bag.png");OrbisUserServiceInitializeParams params={};params.priority=256;sceUserServiceInitialize(&params);int user=-1;sceUserServiceGetInitialUser(&user);scePadInit();int pad=scePadOpen(user,0,0,0);uint32_t previous=0;std::string shown;bool wasBusy=false;
 for(int frame=0;;++frame){OrbisPadData p={};if(pad>=0&&scePadReadState(pad,&p)>=0&&p.connected){auto edge=p.buttons&~previous;previous=p.buttons;pthread_mutex_lock(&mutex);bool working=busy;pthread_mutex_unlock(&mutex);if(!working&&(edge&ORBIS_PAD_BUTTON_CROSS)){pthread_mutex_lock(&mutex);busy=true;result="Installing background service...";pthread_mutex_unlock(&mutex);pthread_t thread;if(pthread_create(&thread,0,install,0)==0)pthread_detach(thread);else{pthread_mutex_lock(&mutex);busy=false;result="Could not start setup worker. Restart this app.";pthread_mutex_unlock(&mutex);}}else if(!working&&(edge&ORBIS_PAD_BUTTON_CIRCLE))return sceSystemServiceLoadExec("exit",0);}
    pthread_mutex_lock(&mutex);auto text=result;bool working=busy;pthread_mutex_unlock(&mutex);if(frame==0||shown!=text||wasBusy!=working){shown=text;wasBusy=working;canvas.rect(0,0,1920,1080,0x050d0b);canvas.image(logo,115,103,95,95);canvas.text("NULL_store",225,183,62,0xe7f3ed);canvas.text("Background service setup 1.0",120,342,68,0xe7f3ed);canvas.lines("NULL_store downloads work without this service while the app is open. Install the service to keep downloads running after closing NULL_store.",120,438,37,0xb3c9c1,1660,3);canvas.rect(120,637,1680,160,0x063b29);canvas.lines(text,155,699,34,0x00ff66,1605,3);canvas.text(working?"Please wait for setup to finish.":"X  Install / Start service       CIRCLE  Close",120,936,34,0xe7f3ed);}display.flip(canvas,frame);
 }
}
