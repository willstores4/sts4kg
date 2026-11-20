#include "../src/download_manager.h"
#include "../src/queue_store.h"
#include <cassert>
#include <atomic>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <iostream>
static std::atomic<bool> fetching(false);
static void delay(int ms){struct timespec ts={ms/1000,(ms%1000)*1000000};nanosleep(&ts,0);}
static std::string identityMeta;
static bool duplicateFetch(const std::string&,std::string&out,size_t,const std::string&){out=identityMeta;return true;}
static bool fetch(const std::string&url,std::string&out,size_t,const std::string&referer){if(url.find("do=download")!=std::string::npos){assert(sourceURL(referer));fetching=true;delay(600);out="not a torrent";return true;}out=readFile("tests/fixtures/game.html");return !out.empty();}
int main(){auto links=torrentLinks(readFile("tests/fixtures/game.html"));assert(links.size()==1);assert(links[0].url=="https://ps4torrent.net/index.php?do=download&id=775");assert(links[0].title=="Sugar Rush Rescue.torrent");assert(torrentLinks("<a class='dl__btn' href='https://evil.example/file.torrent'>No</a>").empty());
    auto several=torrentLinks("<div class='dl__title'>One</div><a class='dl__btn' href='/index.php?do=download&amp;id=1'>Get</a><div class='dl__title'>Two</div><a href='/uploads/two.torrent'>Get</a>");assert(several.size()==2&&several[0].url.find("&id=1")!=std::string::npos&&several[1].title=="Two");
    char dir[]="/tmp/portal-download-tests-XXXXXX";assert(mkdtemp(dir));std::string root=dir;Game g;g.id=773;g.title="Generated catalog fixture";g.url="https://ps4torrent.net/773-sugar-rush-rescue.html";uint64_t stamp=0;std::vector<DownloadRow>jobs;std::string why;
    {DownloadManager m(root,fetch);assert(m.enqueue(g,links[0],why));assert(!m.enqueue(g,links[0],why));assert(m.snapshot(stamp,jobs)&&jobs.size()==1);m.remove(jobs[0].id);assert(m.snapshot(stamp,jobs)&&jobs.empty());assert(m.enqueue(g,links[0],why));assert(m.snapshot(stamp,jobs)&&jobs.size()==1&&jobs[0].id==1);m.toggle(jobs[0].id);assert(m.snapshot(stamp,jobs)&&jobs[0].state==DownloadState::Paused);}
    {DownloadManager m(root,fetch);stamp=0;assert(m.snapshot(stamp,jobs)&&jobs[0].state==DownloadState::Paused);assert(m.start());m.toggle(jobs[0].id);
        for(int i=0;i<100&&!fetching;i++)delay(10);assert(fetching);m.discover(g);std::vector<TorrentLink>found;std::string title,error;bool ready=false;for(int i=0;i<30;i++){if(m.takeSources(found,title,error)){ready=true;break;}delay(10);}assert(ready&&error.empty()&&found.size()==1); // discovery remains responsive during an active transfer fetch
        bool failed=false;for(int i=0;i<150;i++){m.snapshot(stamp,jobs);if(jobs[0].state==DownloadState::Failed){failed=true;break;}delay(10);}assert(failed&&!jobs[0].error.empty());m.cancel(jobs[0].id);assert(m.snapshot(stamp,jobs)&&jobs[0].state==DownloadState::Cancelled);m.toggle(jobs[0].id);m.cancel(jobs[0].id);
    }
    {DownloadManager m(root,fetch);stamp=0;assert(m.snapshot(stamp,jobs)&&jobs[0].state==DownloadState::Cancelled);}
    // Startup preserves user intent: active/queued resume; manual pause and errors do not.
    std::string states;for(int state=0;state<7;++state)states+=std::to_string(state+1)+"\t773\tFixture\thttps://ps4torrent.net/index.php?do=download&id="+std::to_string(state+1)+"\t"+std::to_string(state)+"\t0\t0\t\thttps://ps4torrent.net/773-test.html\n";
    assert(saveQueue(root+"/downloads.tsv",states));
    {DownloadManager m(root,fetch);stamp=0;m.snapshot(stamp,jobs);assert(jobs.size()==7);assert(jobs[0].state==DownloadState::Queued&&jobs[1].state==DownloadState::Queued);assert(jobs[2].state==DownloadState::Paused&&jobs[3].state==DownloadState::Paused);assert(jobs[4].state==DownloadState::Complete&&jobs[5].state==DownloadState::Failed&&jobs[6].state==DownloadState::Cancelled);}
    assert(saveQueue(root+"/downloads.tsv",states));assert(writeFile(root+"/downloads.tsv","PORTAL_QUEUE_V2\tbroken\n"));
    {DownloadManager m(root,fetch);stamp=0;m.snapshot(stamp,jobs);assert(jobs.size()==7&&jobs[0].error.find("Recovered")!=std::string::npos);}
    identityMeta="d4:infod6:lengthi1e4:name1:x12:piece lengthi1e6:pieces20:aaaaaaaaaaaaaaaaaaaaee";char hash[41];assert(torrent_identity((const unsigned char*)identityMeta.data(),identityMeta.size(),hash)==0);
    std::string canonical="1\t773\tOriginal torrent\thttps://ps4torrent.net/original.torrent\t3\t0\t1\t\thttps://ps4torrent.net/773-test.html\t"+std::string(hash)+"\t0\n";assert(saveQueue(root+"/downloads.tsv",canonical));
    {DownloadManager m(root,duplicateFetch);Game other=g;other.id=1000000137;other.url="https://gametorrent.org/category/playstation-4/137-test.html";TorrentLink duplicate={"Same torrent from another source","https://gametorrent.org/duplicate.torrent"};assert(m.enqueue(other,duplicate,why));assert(m.start());for(int i=0;i<100;i++){stamp=0;m.snapshot(stamp,jobs);if(jobs.size()==1)break;delay(10);}assert(jobs.size()==1&&jobs[0].id==1&&jobs[0].state==DownloadState::Paused);struct stat st;assert(stat((root+"/downloads/2").c_str(),&st)!=0);}
    std::string legacy="1\t773\tOlder queued alias\thttps://ps4torrent.net/older.torrent\t0\t0\t1\t\thttps://ps4torrent.net/773-test.html\t\t0\n2\t773\tExisting known torrent\thttps://gametorrent.org/known.torrent\t3\t0\t1\t\thttps://gametorrent.org/category/playstation-4/137-test.html\t"+std::string(hash)+"\t0\n";assert(saveQueue(root+"/downloads.tsv",legacy));
    {DownloadManager m(root,duplicateFetch);assert(m.start());for(int i=0;i<100;i++){stamp=0;m.snapshot(stamp,jobs);if(jobs.size()==1)break;delay(10);}assert(jobs.size()==1&&jobs[0].id==2&&jobs[0].state==DownloadState::Paused);struct stat st;assert(stat((root+"/downloads/1").c_str(),&st)!=0);}
    // Delete-files removes payload, metainfo and both committed/temporary resume
    // sidecars, after the worker has stopped using them.
    assert(writeFile(root+"/torrents/2.torrent",identityMeta));assert(writeFile(root+"/torrents/2.torrent.resume","checkpoint"));assert(writeFile(root+"/torrents/2.torrent.resume.tmp","interrupted checkpoint"));
    {DownloadManager m(root,duplicateFetch);assert(m.start());m.remove(2,true);bool gone=false;for(int i=0;i<100;i++){stamp=0;m.snapshot(stamp,jobs);if(jobs.empty()){gone=true;break;}delay(10);}assert(gone);struct stat st;assert(stat((root+"/torrents/2.torrent").c_str(),&st)!=0);assert(stat((root+"/torrents/2.torrent.resume").c_str(),&st)!=0);assert(stat((root+"/torrents/2.torrent.resume.tmp").c_str(),&st)!=0);}
    // Shared queue and backup stay readable under a restrictive daemon umask.
    mode_t priorMask=umask(0077);assert(writeFile(root+"/downloads.tsv.tmp","stale temporary"));
    assert(saveQueue(root+"/downloads.tsv",canonical,0644));assert(saveQueue(root+"/downloads.tsv",canonical,0644));umask(priorMask);
    struct stat queueStat;assert(stat((root+"/downloads.tsv").c_str(),&queueStat)==0&&(queueStat.st_mode&0777)==0644);assert(stat((root+"/downloads.tsv.bak").c_str(),&queueStat)==0&&(queueStat.st_mode&0777)==0644);
    assert(chmod((root+"/downloads.tsv").c_str(),0600)==0);{DownloadManager m(root,fetch);assert(m.startupError().empty());assert(stat((root+"/downloads.tsv").c_str(),&queueStat)==0&&(queueStat.st_mode&0777)==0644);}
    remove((root+"/downloads.tsv.tmp").c_str());
    // An inaccessible primary must not be replaced by an empty or stale queue.
    if(getuid()!=0){assert(chmod((root+"/downloads.tsv").c_str(),0000)==0);{DownloadManager m(root,fetch);assert(!m.startupError().empty()&&!m.start());assert(!m.enqueue(g,links[0],why)&&why==m.startupError());}assert(!saveQueue(root+"/downloads.tsv",states,0644));assert(chmod((root+"/downloads.tsv").c_str(),0644)==0);assert(loadQueue(root+"/downloads.tsv")==canonical);}
    remove((root+"/downloads.tsv.bak").c_str());assert(writeFile(root+"/downloads.tsv","corrupt queue kept for recovery"));{DownloadManager m(root,fetch);assert(!m.start()&&!m.enqueue(g,links[0],why));}assert(readFile(root+"/downloads.tsv")=="corrupt queue kept for recovery");
    remove((root+"/downloads.tsv").c_str());rmdir((root+"/torrents").c_str());rmdir((root+"/downloads").c_str());rmdir(root.c_str());std::cout<<"Portal torrent links, queue persistence, pause/resume, duplicate prevention, async discovery and failure handling passed.\n";
}
