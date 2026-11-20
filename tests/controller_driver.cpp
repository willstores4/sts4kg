#include "../src/download_controller.h"
#include <sys/stat.h>
#include <ctime>
#include <iostream>
static std::string metainfo,tracker;
static void sleepMs(int n){timespec t={n/1000,(n%1000)*1000000};nanosleep(&t,0);}
static bool fetch(const std::string&url,std::string&out,size_t,const std::string&){out=readFile(url.find("info_hash=")!=std::string::npos?tracker:metainfo);return !out.empty();}
int main(int argc,char**argv){if(argc<5)return 1;metainfo=argv[2];tracker=argv[3];mkdir(argv[1],0700);DownloadController controller(argv[1],fetch);if(!controller.start())return 2;sleepMs(1500);Game g;g.id=773;g.title="Generated optional service test";g.url="https://ps4torrent.net/773-test.html";TorrentLink link={"generated.torrent","https://ps4torrent.net/index.php?do=download&id=775"};std::string why;if(!controller.enqueue(g,link,why)){std::cerr<<why;return 3;}bool attempted=false;uint64_t stamp=0;std::vector<DownloadRow>jobs;for(int i=0;i<3000;i++){controller.snapshot(stamp,jobs);if(!jobs.empty()){if(std::string(argv[4])=="setup-failure"&&!attempted&&jobs[0].state==DownloadState::Active){controller.setup();attempted=true;}if(std::string(argv[4])=="enqueue-exit"){std::cout<<controller.connectionStatus()<<"\n";return 0;}if(jobs[0].state==DownloadState::Complete&&controller.connectionStatus().find("Setting up")==std::string::npos){std::cout<<controller.connectionStatus()<<"\n";return 0;}if(jobs[0].state==DownloadState::Failed){std::cerr<<jobs[0].error;return 4;}}sleepMs(10);}return 5;}
