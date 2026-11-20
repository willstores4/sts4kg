#include "../src/installer.h"
#include "../src/queue_store.h"
#include <atomic>
#include <cassert>
#include <iostream>

static std::string root;
static std::atomic<int> registrations(0),starts(0),progress(-1);
static std::atomic<bool> complete(false),queryFailure(false);
static bool reg(const InstallPackage&,int& id,std::string&){
    ++registrations;
    id=77;
    // Simulate a journal write failure after the PS4 accepts the task.
    assert(mkdir((root+"/installs.tsv").c_str(),0700)==0);
    return true;
}
static bool start(int id,std::string&){assert(id==77);++starts;return true;}
static bool poll(int id,int& percent,bool& done,std::string& error){
    assert(id==77);
    if(queryFailure){error="Temporary query failure";return false;}
    percent=progress;done=complete;return true;
}
static void pauseMs(){timespec delay={0,10000000};nanosleep(&delay,nullptr);}
static void waitFor(PackageInstaller& installer,InstallState state,int percent,bool warning){
    uint64_t version=0;std::vector<InstallPackage> view;bool busy=true;std::string error;
    for(int attempt=0;attempt<400;++attempt){
        installer.snapshot(version,view,busy,error);
        if(!busy&&!view.empty()&&view[0].state==state&&view[0].percent==percent&&(warning?!error.empty():error.empty()))return;
        pauseMs();
    }
    assert(false&&"Installer failed to reach expected state");
}
int main(){
    char folder[]="/tmp/nullstore-progress-XXXXXX";assert(mkdtemp(folder));root=folder;
    assert(mkdir((root+"/downloads").c_str(),0700)==0);
    assert(mkdir((root+"/downloads/1").c_str(),0700)==0);
    std::string path=root+"/downloads/1/base.pkg",header(4096,0);
    header[0]=0x7f;header[1]='C';header[2]='N';header[3]='T';header[0x77]=0x1a;
    header.replace(0x40,36,"IV0000-TEST00001_00-TESTPACKAGE00001");header[0x436]=0x10;
    assert(durableReplace(path,header));
    InstallBackend backend={reg,start,poll};
    {
        PackageInstaller installer(root,backend);assert(installer.start());installer.scan(1);
        waitFor(installer,InstallState::Ready,-1,false);installer.submit(path);
        waitFor(installer,InstallState::Installing,-1,true);
        assert(starts==1&&registrations==1);
        installer.submit(path);assert(registrations==1);
        // Rescanning clears the duplicate-action notice.
        installer.scan(1);
        progress=35;waitFor(installer,InstallState::Installing,35,true);
        queryFailure=true;waitFor(installer,InstallState::Unavailable,35,true);
        queryFailure=false;waitFor(installer,InstallState::Installing,35,true);
        complete=true;waitFor(installer,InstallState::Complete,100,true);
        assert(rmdir((root+"/installs.tsv").c_str())==0);
        waitFor(installer,InstallState::Complete,100,false);
        assert(loadQueue(root+"/installs.tsv").find("\t77\t4\t100\t")!=std::string::npos);
    }
    // Earlier builds recorded a bookkeeping failure as a failed system task.
    // Reopen that record and recover its progress without a new registration.
    std::string saved=loadQueue(root+"/installs.tsv");
    auto state=saved.find("\t77\t4\t100\tInstalled");
    assert(state!=std::string::npos);
    saved.replace(state,std::string("\t77\t4\t100\tInstalled").size(),
                  "\t77\t5\t0\tSystem task registered, but its recovery record could not be saved. Check Notifications.");
    assert(saveQueue(root+"/installs.tsv",saved));
    {
        PackageInstaller installer(root,backend);assert(installer.start());installer.scan(1);
        waitFor(installer,InstallState::Complete,100,false);
        installer.submit(path);pauseMs();assert(registrations==1&&starts==1);
    }
    InstallPackage package;package.state=InstallState::Installing;
    assert(installStatusText(package)=="Installing");
    package.percent=0;assert(installStatusText(package)=="Installing");
    package.percent=35;assert(installStatusText(package)=="Installing  35%");
    package.state=InstallState::Complete;assert(installStatusText(package)=="Installed");
    package.state=InstallState::Error;package.note="Failed";assert(installStatusText(package)=="Failed");
    unlink(path.c_str());unlink((root+"/installs.tsv").c_str());unlink((root+"/installs.tsv.bak").c_str());
    rmdir((root+"/downloads/1").c_str());rmdir((root+"/downloads").c_str());rmdir(root.c_str());
    std::cout<<"Journal failures preserve tracking; persistence and progress queries recover; percentages reflect reported progress.\n";
}
