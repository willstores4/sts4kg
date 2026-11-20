#include "../src/download_controller.h"
#include "../src/queue_store.h"
#include <cassert>
#include <iostream>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
extern char**environ;
static bool fetch(const std::string&,std::string&,size_t,const std::string&){return false;}
static void delay(){timespec t={0,20000000};nanosleep(&t,0);}
static void checkRows(DownloadController&controller,const std::string&root,DownloadState second){
    uint64_t stamp=0;std::vector<DownloadRow>jobs;assert(controller.snapshot(stamp,jobs));assert(jobs.size()==2);
    assert(jobs[0].id==8&&jobs[0].state==DownloadState::Complete&&jobs[0].done==16&&jobs[0].total==16&&jobs[0].outputDir==root+"/downloads/8");
    assert(jobs[1].id==12&&jobs[1].state==second&&jobs[1].done==4&&jobs[1].total==32&&jobs[1].outputDir=="/mnt/usb0/NULL_store/downloads/12");
}
static void awaitMode(DownloadController&controller,const std::string&root,const std::string&mode,DownloadState second){
    for(int n=0;n<500;n++){checkRows(controller,root,second);if(controller.connectionStatus().find(mode)==0)return;delay();}assert(!"Queue owner did not switch");
}
int main(int argc,char**argv){
    assert(argc==2);char folder[]="/tmp/nullstore-handoff-XXXXXX";assert(mkdtemp(folder));std::string root=folder;
    std::string rows="8\t773\tComplete generated fixture\thttps://ps4torrent.net/8.torrent\t4\t16\t16\t\thttps://ps4torrent.net/8.html\t\t0\t0\t"+root+"/downloads/8\n12\t774\tPaused generated fixture\thttps://ps4torrent.net/12.torrent\t3\t4\t32\t\thttps://ps4torrent.net/12.html\t\t0\t0\t/mnt/usb0/NULL_store/downloads/12\n";
    assert(saveQueue(root+"/downloads.tsv",rows));assert(mkdir((root+"/downloads").c_str(),0700)==0);assert(mkdir((root+"/downloads/8").c_str(),0700)==0);assert(writeFile(root+"/downloads/8/fixture.bin","generated sample"));
    {
        DownloadController controller(root,fetch);assert(controller.start());
        for(int n=0;n<500&&controller.connectionStatus().find("In-app")!=0;n++)delay();checkRows(controller,root,DownloadState::Paused);
        DownloadState second=DownloadState::Paused;
        for(int cycle=0;cycle<2;cycle++){
            controller.setup();int probe=open((root+"/queue-owner.lock").c_str(),O_RDWR);assert(probe>=0);bool released=false;
            for(int n=0;n<200;n++){checkRows(controller,root,second);if(flock(probe,LOCK_EX|LOCK_NB)==0){flock(probe,LOCK_UN);released=true;break;}delay();}close(probe);assert(released);
            pid_t service=-1;char*args[]={argv[1],const_cast<char*>(root.c_str()),nullptr};assert(posix_spawn(&service,argv[1],nullptr,nullptr,args,environ)==0);
            awaitMode(controller,root,"Background service connected",second);
            if(cycle==0){controller.cancel(12);bool cancelled=false;for(int n=0;n<300;n++){uint64_t stamp=0;std::vector<DownloadRow>jobs;controller.snapshot(stamp,jobs);assert(jobs.size()==2);if(jobs[1].state==DownloadState::Cancelled){cancelled=true;break;}delay();}assert(cancelled);second=DownloadState::Cancelled;}
            controller.stopService();awaitMode(controller,root,"In-app downloads",second);int status=-1;assert(waitpid(service,&status,0)==service&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
            assert(readFile(root+"/downloads/8/fixture.bin")=="generated sample");struct stat st;assert(stat((root+"/downloads.tsv").c_str(),&st)==0&&(st.st_mode&0777)==0644);
        }
    }
    for(const char*name:{"downloads.tsv","downloads.tsv.bak","queue-owner.lock","service-stopped.txt","downloads/8/fixture.bin"})unlink((root+"/"+name).c_str());rmdir((root+"/downloads/8").c_str());rmdir((root+"/downloads").c_str());rmdir((root+"/torrents").c_str());rmdir(root.c_str());
    std::cout<<"Repeated app/service handoffs preserve the list, IDs, states, progress, custom paths and downloaded files.\n";
}
