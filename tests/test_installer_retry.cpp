#include "../src/installer.h"
#include "../src/queue_store.h"
#include <cassert>
#include <atomic>
#include <iostream>
static std::atomic<int>registrations(0),starts(0),resumes(0);static std::atomic<bool>resumed(false),queryFails(false);
static bool reg(const InstallPackage&,int&id,std::string&){++registrations;id=55;return true;}
static bool start(int id,std::string&error){assert(id==55);++starts;error="Injected start failure";return false;}
static bool resume(int id,std::string&){assert(id==55);++resumes;resumed=true;return true;}
static bool poll(int id,int&percent,bool&complete,std::string&error){assert(id==55);if(queryFails){error="Injected query failure";return false;}complete=resumed;percent=resumed?100:0;return true;}
static void pauseMs(){struct timespec t={0,5000000};nanosleep(&t,0);}
static bool waitFor(PackageInstaller&i,InstallState state,std::vector<InstallPackage>&view){uint64_t stamp=0;bool busy=true;std::string error;for(int n=0;n<300;n++){i.snapshot(stamp,view,busy,error);if(!busy&&!view.empty()&&view[0].state==state)return true;pauseMs();}return false;}
int main(){
 char folder[]="/tmp/nullstore-installer-retry-XXXXXX";assert(mkdtemp(folder));std::string root=folder;mkdir((root+"/downloads").c_str(),0700);mkdir((root+"/downloads/1").c_str(),0700);
 std::string path=root+"/downloads/1/base.pkg",header(4096,0);header[0]=0x7f;header[1]='C';header[2]='N';header[3]='T';header[0x77]=0x1a;header.replace(0x40,36,"IV0000-TEST00001_00-TESTPACKAGE00001");header[0x436]=0x10;assert(durableReplace(path,header));
 InstallBackend api={reg,start,poll,resume};std::vector<InstallPackage>view;
 {PackageInstaller i(root,api);assert(i.start());i.scan(1);assert(waitFor(i,InstallState::Ready,view));i.submit(path);assert(waitFor(i,InstallState::Error,view));assert(view[0].taskId==55&&registrations==1&&starts==1);}
 // Reopen after a failed native start, then retry the same journaled task ID.
 {PackageInstaller i(root,api);assert(i.start());i.scan(1);assert(waitFor(i,InstallState::Error,view));queryFails=true;i.submit(path);assert(waitFor(i,InstallState::Error,view)&&view[0].note=="Injected query failure");assert(registrations==1&&resumes==0);queryFails=false;i.submit(path);assert(waitFor(i,InstallState::Complete,view));assert(view[0].taskId==55&&registrations==1&&starts==1&&resumes==1);}
 unlink(path.c_str());unlink((root+"/installs.tsv").c_str());unlink((root+"/installs.tsv.bak").c_str());rmdir((root+"/downloads/1").c_str());rmdir((root+"/downloads").c_str());rmdir(root.c_str());std::cout<<"Failed-start retry after relaunch preserves the task ID and never registers a duplicate.\n";
}
