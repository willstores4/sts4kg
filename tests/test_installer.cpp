#include "../src/installer.h"
#include "../src/queue_store.h"
#include <cassert>
#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
static std::string root;
static std::atomic<int> registered(0),started(0),polled(0);
static void pauseMs(int n){struct timespec ts={n/1000,(n%1000)*1000000};nanosleep(&ts,0);}
static bool reg(const InstallPackage&p,int&id,std::string&){assert(p.contentId=="IV0000-TEST00001_00-TESTPACKAGE00001");id=42;++registered;return true;}
static bool start(int id,std::string&){assert(id==42);std::string payload=loadQueue(root+"/installs.tsv");assert(payload.find("\t42\t")!=std::string::npos);++started;return true;}
static bool poll(int id,int&percent,bool&complete,std::string&){assert(id==42);++polled;percent=100;complete=true;return true;}
static void put64(std::string&s,int pos,uint64_t n){for(int i=7;i>=0;i--){s[pos+i]=n&255;n>>=8;}}
static void put32(std::string&s,int pos,uint32_t n){for(int i=3;i>=0;i--){s[pos+i]=n&255;n>>=8;}}
static std::string package(){std::string s(0x1000,0);s[0]=0x7f;s[1]='C';s[2]='N';s[3]='T';s[0x77]=0x1a;s.replace(0x40,36,"IV0000-TEST00001_00-TESTPACKAGE00001");put64(s,0x430,s.size());return s;}
int main(){char dir[]="/tmp/portal-installer-tests-XXXXXX";assert(mkdtemp(dir));root=dir;mkdir((root+"/downloads").c_str(),0700);mkdir((root+"/downloads/5").c_str(),0700);auto path=root+"/downloads/5/base.pkg";assert(durableReplace(path,package()));InstallPackage pkg;std::string error;assert(inspectPackage(path,pkg,error)&&pkg.bytes==4096&&pkg.titleId=="TEST00001");auto bad=package();put64(bad,0x430,9000);assert(durableReplace(root+"/bad.pkg",bad));assert(!inspectPackage(root+"/bad.pkg",pkg,error));assert(symlink(path.c_str(),(root+"/downloads/5/link.pkg").c_str())==0);assert(!inspectPackage(root+"/downloads/5/link.pkg",pkg,error));error.clear();auto files=scanPackages(root,5,error);assert(files.size()==1&&files[0].path==path);assert(storageInstallPath("/data/portal-store/a.pkg")=="/user/data/portal-store/a.pkg");
    // Real console game sizes: sparse fixtures exercise 64-bit file metadata
    // without writing gigabytes of payload or bypassing declared-size checks.
    for(uint64_t length:{3608805376ULL,2321219584ULL,12ULL*1024*1024*1024}){auto header=package();put64(header,0x430,length);auto large=root+"/large.pkg";int fd=open(large.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0&&write(fd,header.data(),header.size())==(ssize_t)header.size()&&ftruncate(fd,(off_t)length)==0);close(fd);assert(inspectPackage(large,pkg,error)&&pkg.bytes==length);unlink(large.c_str());}
    for(uint32_t type:{0x1au,0x1bu,0x1cu,0x1eu}){auto header=package();put32(header,0x74,type);auto classified=root+"/classified.pkg";assert(durableReplace(classified,header));assert(inspectPackage(classified,pkg,error)&&pkg.contentType==type&&pkg.patch==(type==0x1e));unlink(classified.c_str());}
    for(uint32_t flag:{0x00100000u,0x40000000u,0x41000000u,0x60000000u}){auto header=package();put32(header,0x78,flag);auto classified=root+"/classified.pkg";assert(durableReplace(classified,header));assert(inspectPackage(classified,pkg,error)&&pkg.patch&&pkg.contentFlags==flag);unlink(classified.c_str());}
    InstallBackend backend={reg,start,poll};
    {PackageInstaller installer(root,backend);assert(installer.start());installer.scan(5);uint64_t version=0;bool loading=true;std::vector<InstallPackage>view;for(int i=0;i<100&&loading;i++){installer.snapshot(version,view,loading,error);pauseMs(10);}assert(!loading&&view.size()==1);installer.submit(path);installer.submit(path);bool complete=false;for(int i=0;i<300;i++){installer.snapshot(version,view,loading,error);if(!view.empty()&&view[0].state==InstallState::Complete){complete=true;break;}pauseMs(10);}assert(complete&&registered==1&&started==1&&polled>=1);}
    {PackageInstaller installer(root,backend);assert(installer.start());installer.scan(5);uint64_t version=0;bool loading=true;std::vector<InstallPackage>view;for(int i=0;i<100&&loading;i++){installer.snapshot(version,view,loading,error);pauseMs(10);}assert(view.size()==1&&view[0].state==InstallState::Complete&&view[0].taskId==42);installer.submit(path);pauseMs(30);assert(registered==1&&started==1);}
    // A still-running owned system task is reattached, not registered a second time.
    auto saved=loadQueue(root+"/installs.tsv");auto point=saved.find("\t42\t4\t100\t");assert(point!=std::string::npos);saved.replace(point,10,"\t42\t3\t40\t");assert(saveQueue(root+"/installs.tsv",saved));
    {PackageInstaller installer(root,backend);assert(installer.start());for(int i=0;i<150&&polled<2;i++)pauseMs(10);assert(polled>=2&&registered==1&&started==1);}
    unlink(path.c_str());unlink((root+"/downloads/5/link.pkg").c_str());unlink((root+"/bad.pkg").c_str());unlink((root+"/installs.tsv").c_str());unlink((root+"/installs.tsv.bak").c_str());rmdir((root+"/downloads/5").c_str());rmdir((root+"/downloads").c_str());rmdir(root.c_str());std::cout<<"PKG validation, symlink rejection, installer task journaling, duplicate prevention and relaunch reattachment passed.\n";
}
