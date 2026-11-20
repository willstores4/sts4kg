#include "ui.h"
#include "display_ps4.h"
#include "download_controller.h"
#include "network_ps4.h"
#include "path_info.h"
#include "text_input_ps4.h"
#include "startup_ps4.h"
#include "data_root.h"
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
// The bundled OpenOrbis declarations for these exports omit their arguments.
extern "C" int launchBrowser(const char*,void*) __asm__("sceSystemServiceLaunchWebBrowser");
static std::string DATA;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static bool working=false,ready=false;
static std::string message;
static std::vector<Game> refreshed;
static void status(const std::string&s){pthread_mutex_lock(&lock);message=s;pthread_mutex_unlock(&lock);}
static bool get(const std::string&url,std::string&out,size_t cap=4*1024*1024){return sourceURL(url)&&portalNetworkFetch(url,out,cap,"");}
static int refreshSource=0;static bool refreshForced=false;
static void publishCatalogue(const std::vector<Game>&games){writeFile(DATA+"/catalog.tsv",serialize(games));pthread_mutex_lock(&lock);refreshed=games;ready=true;pthread_mutex_unlock(&lock);}
static void* refresh(void*){auto next=mergedCatalog("/app0/assets/catalog.tsv",DATA+"/catalog.tsv");int updated=0,cached=0,unavailable=0;int chosen=refreshSource;bool force=refreshForced;
 for(int source=1;source<=3;source++){if(chosen&&source!=chosen)continue;
  if(!force&&providerCacheFresh(DATA,source)){cached++;continue;}
  int pages=source==1?2:1;bool ok=true;std::vector<Game>part;std::set<int>ids;
  for(int page=1;page<=pages;page++){status(std::string("Refreshing ")+sourceName(source)+" "+std::to_string(page)+"/"+std::to_string(pages));std::string html;if(!get(listingURL(source,page),html)){ok=false;break;}auto cards=parseSourceCards(html,source);if(cards.empty()){ok=false;break;}pages=std::max(pages,sourceLastPage(html,source));for(auto&g:cards)if(ids.insert(g.id).second){for(auto&old:next)if(old.id==g.id){if(g.image.empty())g.image=old.image;if(g.year.empty())g.year=old.year;break;}part.push_back(g);}sceKernelUsleep(350000);}
  if(ok&&saveProviderCache(DATA,source,part)){next.erase(std::remove_if(next.begin(),next.end(),[&](const Game&g){return sourceForURL(g.url)==source;}),next.end());next.insert(next.end(),part.begin(),part.end());updated++;publishCatalogue(next);}else unavailable++;
 }
 if(updated){std::set<int>packagedArt;auto index=readFile("/app0/assets/covers.idx");for(size_t p=0;p<index.size();){packagedArt.insert(atoi(index.c_str()+p));auto end=index.find('\n',p);if(end==std::string::npos)break;p=end+1;}int artwork=0;
  for(auto&g:next){int source=sourceForURL(g.url);if(chosen&&source!=chosen)continue;auto folder=providerCoverPath(DATA,source);mkdir(folder.c_str(),0777);auto filename=std::to_string(g.id)+".jpg";struct stat st;if(packagedArt.count(g.id)||stat((folder+"/"+filename).c_str(),&st)==0||stat((DATA+"/covers/"+filename).c_str(),&st)==0)continue;
   std::string bytes;if(sourceURL(g.image)&&portalNetworkFetch(g.image,bytes,3*1024*1024,"")){int w,h,c;if(stbi_info_from_memory((const unsigned char*)bytes.data(),bytes.size(),&w,&h,&c)&&w<=2048&&h<=2048)writeFile(folder+"/"+filename,bytes);}sceKernelUsleep(200000);if(++artwork%20==0){std::vector<Game>part;for(auto&row:next)if(sourceForURL(row.url)==source)part.push_back(row);saveProviderCache(DATA,source,part,false);publishCatalogue(next);status("Caching provider cover art | Catalogue ready to browse");}
  }
  for(int source=1;source<=3;source++){std::vector<Game>part;for(auto&g:next)if(sourceForURL(g.url)==source)part.push_back(g);if(!part.empty()&&(chosen==0||chosen==source))saveProviderCache(DATA,source,part,false);}publishCatalogue(next);
 }
 status(std::to_string(updated)+" updated / "+std::to_string(cached)+" cached / "+std::to_string(unavailable)+" unavailable | Saved catalogues retained");pthread_mutex_lock(&lock);working=false;pthread_mutex_unlock(&lock);return 0;
}
static void startRefresh(int source=0,bool force=false){pthread_mutex_lock(&lock);if(working){pthread_mutex_unlock(&lock);return;}refreshSource=source;refreshForced=force;working=true;pthread_mutex_unlock(&lock);pthread_t th;if(pthread_create(&th,0,refresh,0)==0)pthread_detach(th);else{pthread_mutex_lock(&lock);working=false;message="Could not start refresh. Retry with OPTIONS.";pthread_mutex_unlock(&lock);}}
static bool checkDownloadLocation(const std::string&base,std::string&why){if(!validDownloadBase(DATA,base)){why="Invalid download location.";return false;}if(base!=DATA&&mkdir(base.c_str(),0777)&&errno!=EEXIST){why="USB destination is unavailable or read-only (mkdir errno "+std::to_string(errno)+").";return false;}std::string downloads=base+"/downloads";if(mkdir(downloads.c_str(),0777)&&errno!=EEXIST){why="Cannot create the downloads folder here (errno "+std::to_string(errno)+").";return false;}struct stat st;if(portalPathInfo(downloads.c_str(),&st)||!S_ISDIR(st.st_mode)){why="The selected downloads path is not a readable directory.";return false;}std::string probe=downloads+"/.nullstore-write-test-"+std::to_string(getpid());int fd=open(probe.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0){why="Cannot write to this location (errno "+std::to_string(errno)+"). Choose another drive.";return false;}const char mark[]="NULL_store";bool wrote=write(fd,mark,sizeof(mark)-1)==(ssize_t)(sizeof(mark)-1);int saved=errno;if(close(fd)&&wrote){wrote=false;saved=errno;}unlink(probe.c_str());if(!wrote){why="Cannot write to this location (errno "+std::to_string(saved)+"). Choose another drive.";return false;}struct statvfs fs;if(statvfs(base.c_str(),&fs)==0){uint64_t unit=fs.f_frsize?fs.f_frsize:fs.f_bsize;why="Writable. "+bytesText((uint64_t)fs.f_bavail*unit)+" free."; }else why="Writable. Free-space details unavailable (errno "+std::to_string(errno)+").";return true;}

int main(){DATA=nullStoreDataRoot();mkdir(DATA.c_str(),0777);if(!loadStartupModules())return 1;mkdir((DATA+"/covers").c_str(),0777);UI ui("/app0/assets",DATA);DownloadController downloads(DATA,portalNetworkFetch);bool downloadsReady=downloads.start();uint64_t downloadVersion=0;Display display;if(!display.init()){installLog("Startup display initialization failed");return 1;}installLog("Startup display ready");scePadInit();OrbisUserServiceInitializeParams param={};param.priority=ORBIS_KERNEL_PRIO_FIFO_LOWEST;sceUserServiceInitialize(&param);int user=-1;sceUserServiceGetInitialUser(&user);int pad=scePadOpen(user,0,0,0);uint32_t prev=0;int held=0;NativeTextInput textInput;bool keyboardFallback=false,waitPadRelease=false;PackageInstaller installer(DATA,ps4InstallBackend());bool installerReady=installer.start();uint64_t installVersion=0;for(int source=1;source<=3;source++){if(providerCached(DATA,source).empty()){std::vector<Game>part;for(auto&g:ui.games)if(sourceForURL(g.url)==source)part.push_back(g);if(!part.empty())saveProviderCache(DATA,source,part,false);}}
    startRefresh();
    for(int frame=0;;++frame){OrbisPadData p={};if(pad<0||scePadReadState(pad,&p)<0||!p.connected){if(ui.notice.empty()){ui.notice="Connect a controller to browse the catalog.";ui.dirty=true;}if(frame%120==0){sceUserServiceGetInitialUser(&user);if(pad>=0)scePadClose(pad);pad=scePadOpen(user,0,0,0);}prev=0;}else{if(ui.notice=="Connect a controller to browse the catalog."){ui.notice.clear();ui.dirty=true;}uint32_t b=p.buttons;if(p.leftStick.x<55)b|=ORBIS_PAD_BUTTON_LEFT;if(p.leftStick.x>200)b|=ORBIS_PAD_BUTTON_RIGHT;if(p.leftStick.y<55)b|=ORBIS_PAD_BUTTON_UP;if(p.leftStick.y>200)b|=ORBIS_PAD_BUTTON_DOWN;uint32_t edge=b&~prev;held=b==prev?held+1:0;if(held>24&&held%7==0)edge|=b&0xf0;prev=b;if(textInput.isOpen()||waitPadRelease){edge=0;held=0;if(!textInput.isOpen()&&b==0)waitPadRelease=false;}
        struct Bind{uint32_t b;Action a;};const Bind binds[]={{ORBIS_PAD_BUTTON_UP,UP},{ORBIS_PAD_BUTTON_DOWN,DOWN},{ORBIS_PAD_BUTTON_LEFT,LEFT},{ORBIS_PAD_BUTTON_RIGHT,RIGHT},{ORBIS_PAD_BUTTON_CIRCLE,BACK},{ORBIS_PAD_BUTTON_TRIANGLE,SEARCH},{ORBIS_PAD_BUTTON_L1,TAB_PREV},{ORBIS_PAD_BUTTON_R1,TAB_NEXT},{ORBIS_PAD_BUTTON_TOUCH_PAD,SORT},{ORBIS_PAD_BUTTON_L2,PAGE_PREV},{ORBIS_PAD_BUTTON_R2,PAGE_NEXT},{ORBIS_PAD_BUTTON_R3,DOWNLOADS}};for(auto&x:binds)if(edge&x.b)ui.action(x.a);if(edge&ORBIS_PAD_BUTTON_CROSS){if(ui.tab==3&&!ui.sourcePicker&&!ui.installPicker&&!ui.servicePicker&&!ui.downloadLocationPicker&&!ui.keyboard&&!ui.removePicker)downloads.toggle(ui.selectedJob());else ui.action(CONFIRM);}if(edge&ORBIS_PAD_BUTTON_SQUARE){if(ui.tab==3&&!ui.installPicker&&!ui.servicePicker&&!ui.downloadLocationPicker&&!ui.keyboard&&!ui.removePicker)downloads.cancel(ui.selectedJob());else ui.action(FAVORITE);}
        if(edge&ORBIS_PAD_BUTTON_L3&&ui.detail&&!ui.sourcePicker&&!ui.keyboard&&!ui.servicePicker&&ui.current()){int r=launchBrowser(ui.current()->url.c_str(),0);if(r<0)ui.notice="Browser could not open. The game URL is shown below.";ui.dirty=true;}
        if(edge&ORBIS_PAD_BUTTON_L3&&ui.tab!=3&&!ui.detail&&!ui.keyboard&&!ui.sourcePicker)ui.action(SELECT_SOURCE);
        if(edge&ORBIS_PAD_BUTTON_L3&&ui.tab==3&&!ui.installPicker&&!ui.servicePicker&&!ui.downloadLocationPicker)ui.action(SERVICE);
        if(edge&ORBIS_PAD_BUTTON_OPTIONS&&ui.tab==3&&!ui.installPicker&&!ui.servicePicker&&!ui.downloadLocationPicker){ui.action(REMOVE_DOWNLOAD);}else if(edge&ORBIS_PAD_BUTTON_OPTIONS&&!ui.keyboard&&!ui.servicePicker&&!ui.downloadLocationPicker)startRefresh(ui.sourceFilter,true);}
        if(!ui.keyboard)keyboardFallback=false;
        if(ui.keyboard&&!keyboardFallback&&!textInput.isOpen()){std::string error;ui.keyboard=false;if(!textInput.begin(user,ui.query,error)){ui.keyboard=true;keyboardFallback=true;ui.notice=error;}ui.dirty=true;}
        if(textInput.isOpen()){std::string text,error;auto result=textInput.poll(text,error);if(result!=NativeTextInput::Pending){waitPadRelease=true;held=0;ui.dirty=true;if(result==NativeTextInput::Accepted){ui.query=text;ui.selected=0;ui.filter();}else if(result==NativeTextInput::Failed)ui.notice=error;}}
        if(ui.removeRequest){ui.removeRequest=false;downloads.remove(ui.removeJob,ui.removeDeleteFiles);ui.notice=ui.removeDeleteFiles?"Stopping the transfer, then deleting its downloaded files…":"Removed from Downloads. Files are kept.";ui.dirty=true;}
        if(ui.downloadLocationRequest){ui.downloadLocationRequest=false;std::string why;if(checkDownloadLocation(ui.requestedDownloadBase,why)){if(writeFile(DATA+"/download-location.txt",ui.requestedDownloadBase+"\n")){ui.downloadBase=ui.requestedDownloadBase;ui.notice="Download location saved. "+why;}else ui.notice="Could not save download location; check internal storage.";}else ui.notice=why;if(ui.downloadBase==DATA)ui.downloadLocationSelected=0;else for(int i=0;i<8;i++)if(ui.downloadBase=="/mnt/usb"+std::to_string(i)+"/NULL_store")ui.downloadLocationSelected=i+1;ui.dirty=true;}
        if(ui.serviceStartRequest){ui.serviceStartRequest=false;downloads.setup();}
        if(ui.serviceStopRequest){ui.serviceStopRequest=false;downloads.stopService();}
        if(ui.serviceTestRequest){ui.serviceTestRequest=false;Game game;game.id=1999000001;game.title="Connection test: Big Buck Bunny (public domain)";game.url="https://ps4torrent.net/";TorrentLink link={"WebTorrent test (one verified piece)","https://webtorrent.io/torrents/big-buck-bunny.torrent"};if(downloadsReady&&downloads.enqueue(game,link,ui.notice))ui.notice="Added Big Buck Bunny test. It stops after the first verified piece.";else if(!downloadsReady)ui.notice="Download worker is unavailable. Restart NULL_store and retry.";ui.tab=3;ui.dirty=true;}
        auto connection=downloads.connectionStatus();if(connection!=ui.serviceStatus){ui.serviceStatus=connection;if(ui.tab==3)ui.dirty=true;}
        if(downloads.takeNotice(ui.notice))ui.dirty=true;
        if(ui.installScanRequest){ui.installScanRequest=false;if(installerReady)installer.scan(ui.installJob,ui.installPath);else{ui.installLoading=false;ui.installError="Installer worker could not start. Restart NULL_store.";ui.dirty=true;}}
        if(ui.installSubmitRequest){ui.installSubmitRequest=false;if(ui.installSelected>=0&&ui.installSelected<int(ui.packages.size()))installer.submit(ui.packages[ui.installSelected].path);}
        if(installer.snapshot(installVersion,ui.packages,ui.installLoading,ui.installError)&&ui.installPicker)ui.dirty=true;
        if(ui.downloadRequest){ui.downloadRequest=false;if(downloadsReady)downloads.discover(ui.downloadGame);else{ui.sourceLoading=false;ui.sourceError="Download service could not start. Restart NULL_store and retry.";ui.dirty=true;}}
        std::vector<TorrentLink> found;std::string sourceTitle,sourceError;
        if(downloads.takeSources(found,sourceTitle,sourceError)&&ui.sourcePicker){ui.sources=found;ui.sourceLoading=false;ui.sourceError=sourceError;ui.dirty=true;if(found.size()==1&&sourceError.empty())ui.enqueueRequest=true;}
        if(ui.enqueueRequest){ui.enqueueRequest=false;if(ui.sourceSelected>=0&&ui.sourceSelected<int(ui.sources.size())){bool queued=downloads.enqueue(ui.downloadGame,ui.sources[ui.sourceSelected],ui.notice);ui.sourcePicker=false;if(queued&&!ui.backgroundPromptShown&&downloads.connectionStatus().find("In-app")==0){ui.backgroundPromptShown=true;writeFile(DATA+"/background-tip-shown.txt","Shown\n");ui.servicePicker=true;ui.serviceSelected=0;}ui.dirty=true;}}
        if(downloads.snapshot(downloadVersion,ui.jobs)&&ui.tab==3)ui.dirty=true;
        pthread_mutex_lock(&lock);if(!message.empty()&&message!=ui.status){ui.status=message;ui.covers.clear();ui.dirty=true;}if(ready){ui.games=refreshed;ui.covers.clear();ui.filter();ready=false;}pthread_mutex_unlock(&lock);if(ui.dirty)ui.draw();display.flip(ui.c,frame);
    }
}
