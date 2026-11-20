#include "../src/download_manager.h"
#include <ctime>
#include <unistd.h>
#include <iostream>
static std::string metainfo,tracker;
static bool fetch(const std::string&url,std::string&out,size_t,const std::string&){out=readFile(url.find("info_hash=")!=std::string::npos?tracker:metainfo);return !out.empty();}
int main(int argc,char**argv){if(argc<5)return 3;metainfo=argv[2];tracker=argv[3];std::string root=argv[1];DownloadManager manager(root,fetch);uint64_t stamp=0;std::vector<DownloadRow>jobs;manager.snapshot(stamp,jobs);if(jobs.empty()){Game g;g.id=773;g.title="Generated restart test";g.url="https://ps4torrent.net/773-test.html";TorrentLink link={"test.torrent","https://ps4torrent.net/index.php?do=download&id=775"};std::string why;if(!manager.enqueue(g,link,why))return 4;}if(!manager.start())return 5;for(int i=0;i<4000;i++){manager.snapshot(stamp,jobs);for(auto&j:jobs){if(std::string(argv[4])=="rate"&&j.rate>0&&j.done==0){std::cout<<"Rate before first verified piece: "<<j.rate<<" bytes/s\n";return 0;}if(j.done>=32768&&j.state==DownloadState::Active){if(std::string(argv[4])=="crash")_exit(0);if(std::string(argv[4])=="close")return 0;}if(j.state==DownloadState::Complete){std::cout<<"Complete after launch/relaunch\n";return 0;}if(j.state==DownloadState::Failed){std::cerr<<j.error<<"\n";return 1;}}struct timespec delay={0,5000000};nanosleep(&delay,0);}return 6;}
