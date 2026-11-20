#pragma once
#include <string>

inline std::string defaultDownloadDir(const std::string&root,int id){return root+"/downloads/"+std::to_string(id);}
inline bool validDownloadBase(const std::string&root,const std::string&base){
    if(base==root)return true;
    for(int i=0;i<8;i++)if(base=="/mnt/usb"+std::to_string(i)+"/NULL_store")return true;
    return false;
}
inline bool validDownloadDir(const std::string&root,int id,const std::string&dir){
    if(id<=0)return false;
    if(dir==defaultDownloadDir(root,id))return true;
    for(int i=0;i<8;i++)if(dir=="/mnt/usb"+std::to_string(i)+"/NULL_store/downloads/"+std::to_string(id))return true;
    return false;
}
inline bool pathInsideDownloadDir(const std::string&root,int id,const std::string&dir,const std::string&path){
    if(!validDownloadDir(root,id,dir)||path.compare(0,dir.size()+1,dir+"/")!=0)return false;
    if(path.size()<=dir.size()+1)return false;
    std::string tail=path.substr(dir.size()+1);
    if(tail==".."||tail=="."||tail.find("/../")!=std::string::npos||tail.find("/./")!=std::string::npos)return false;
    return tail.compare(0,3,"../")!=0&&tail.compare(0,2,"./")!=0&&tail.size()>=3&&tail.substr(tail.size()-3)!="/..";
}
