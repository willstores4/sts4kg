#pragma once
#include "providers.h"
enum class DownloadState {Queued, Active, Pausing, Paused, Complete, Failed, Cancelled};
inline const char* stateName(DownloadState s){switch(s){case DownloadState::Queued:return "Queued";case DownloadState::Active:return "Downloading";case DownloadState::Pausing:return "Pausing";case DownloadState::Paused:return "Paused";case DownloadState::Complete:return "Complete";case DownloadState::Failed:return "Failed";default:return "Cancelled";}}
struct TorrentLink {std::string title,url;};
inline bool isDiagnosticTorrent(const std::string&url){return url=="https://webtorrent.io/torrents/big-buck-bunny.torrent";}
struct DownloadRow {int id=0,gameId=0;std::string title,url,page,phase,error,infoHash,outputDir;bool removed=false,deleteFiles=false;DownloadState state=DownloadState::Queued;uint64_t done=0,total=0;double rate=0;unsigned peers=0;};
inline std::vector<TorrentLink> torrentLinks(const std::string&html,int source=1){std::vector<TorrentLink>v;std::set<std::string>seen;size_t p=0;std::string title;
    while((p=html.find('<',p))!=std::string::npos){auto e=html.find('>',p);if(e==std::string::npos)break;auto tag=html.substr(p,e-p+1);
        if(hasClass(tag,"dl__title")){auto end=html.find("</",e+1);title=plain(html.substr(e+1,end-e-1));}
        if(tag.size()>2&&tag[1]=='a'&&(isspace((unsigned char)tag[2])||tag[2]=='>')){auto url=resolveSourceURL(attr(tag,"href"),source);auto path=lower(url);bool download=hasClass(tag,"dl__btn")||hasClass(tag,"download_button_like_stat")||path.find("do=download")!=std::string::npos||path.find(".torrent")!=std::string::npos;
            if(download&&sourceForURL(url)==source&&attr(tag,"data-gt-gated")!="1"&&attr(tag,"data-gt-allowed")!="0"&&seen.insert(url).second){auto end=html.find("</a>",e+1);auto label=title.empty()?plain(html.substr(e+1,end-e-1)):title;v.push_back({label.empty()?"Torrent file "+std::to_string(v.size()+1):label,url});}}
        p=e+1;
    }return v;
}
inline std::string bytesText(uint64_t n){const char*u[]={"B","KB","MB","GB","TB"};double value=n;int i=0;while(value>=1024&&i<4){value/=1024;++i;}char s[48];snprintf(s,sizeof s,i?"%.1f %s":"%.0f %s",value,u[i]);return s;}
