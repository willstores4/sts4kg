#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cctype>
struct Game { int id=0; std::string title,year,size,url,image; };
inline std::string readFile(const std::string&p) { FILE*f=fopen(p.c_str(),"rb"); if(!f)return ""; std::string s; char b[8192]; size_t n; while((n=fread(b,1,sizeof b,f)))s.append(b,n); fclose(f); return s; }
inline bool writeFile(const std::string&p,const std::string&s) { FILE*f=fopen((p+".tmp").c_str(),"wb");if(!f)return false; bool ok=fwrite(s.data(),1,s.size(),f)==s.size(); if(fclose(f))ok=false; if(ok)ok=rename((p+".tmp").c_str(),p.c_str())==0;return ok; }
inline std::string utf8(unsigned c) {std::string o;if(c<128)o+=char(c);else if(c<2048){o+=char(192|(c>>6));o+=char(128|(c&63));}else if(c<65536){o+=char(224|(c>>12));o+=char(128|((c>>6)&63));o+=char(128|(c&63));}else if(c<=0x10ffff){o+=char(240|(c>>18));o+=char(128|((c>>12)&63));o+=char(128|((c>>6)&63));o+=char(128|(c&63));}return o;}
inline std::string decode(std::string s) {
    size_t p=0; while((p=s.find('&',p))!=std::string::npos){auto e=s.find(';',p);if(e==std::string::npos||e-p>12){++p;continue;}auto k=s.substr(p+1,e-p-1);std::string r;
    if(k=="amp")r="&";else if(k=="quot")r="\"";else if(k=="apos"||k=="#039")r="'";else if(k=="lt")r="<";else if(k=="gt")r=">";else if(k=="nbsp")r=" ";else if(k.size()>1&&k[0]=='#')r=utf8(strtoul(k.c_str()+(k[1]=='x'||k[1]=='X'?2:1),0,(k[1]=='x'||k[1]=='X')?16:10));else{++p;continue;}s.replace(p,e-p+1,r);p+=r.size();}return s;
}
inline std::string attr(const std::string&t,const std::string&key) {
    size_t p=0;while((p=t.find(key,p))!=std::string::npos){size_t k=p+key.size();if((p&& !isspace((unsigned char)t[p-1]))||(k<t.size()&&!isspace((unsigned char)t[k])&&t[k]!='=')){p=k;continue;}while(k<t.size()&&isspace((unsigned char)t[k]))++k;if(k>=t.size()||t[k++]!='='){p=k;continue;}while(k<t.size()&&isspace((unsigned char)t[k]))++k;if(k>=t.size())return "";char q=t[k]; if(q=='\"'||q=='\''){auto e=t.find(q,++k);return decode(t.substr(k,e-k));}auto e=t.find_first_of(" >\t\r\n",k);return decode(t.substr(k,e-k));}return "";
}
inline bool hasClass(const std::string&t,const std::string&c){return (" "+attr(t,"class")+" ").find(" "+c+" ")!=std::string::npos;}
inline std::string plain(const std::string&s){std::string o;bool tag=false;for(char c:s){if(c=='<')tag=true;else if(c=='>')tag=false;else if(!tag)o+=c;}o=decode(o);for(char&c:o)if(c=='\n'||c=='\r'||c=='\t')c=' ';auto a=o.find_first_not_of(' ');if(a==std::string::npos)return "";return o.substr(a,o.find_last_not_of(' ')-a+1);}
inline std::string absolute(const std::string&u){if(u.empty())return "";if(u[0]=='/')return "https://ps4torrent.net"+u;return u;}
inline int sourceForURL(const std::string&u){if(u.compare(0,8,"https://"))return 0;auto end=u.find_first_of("/?#",8);auto host=u.substr(8,end-8);if(host=="ps4torrent.net"||host=="www.ps4torrent.net")return 1;if(host=="gametorrent.org"||host=="www.gametorrent.org")return 2;if(host=="love-games1.net"||host=="www.love-games1.net")return 3;return 0;}
inline std::string sourceOrigin(int source){return source==1?"https://ps4torrent.net":source==2?"https://gametorrent.org":source==3?"https://love-games1.net":"";}
inline bool sourceURL(const std::string&u){return sourceForURL(u)!=0;}

inline std::vector<Game> parseCards(const std::string&s){std::vector<Game>v;size_t p=0;while((p=s.find("<a",p))!=std::string::npos){auto e=s.find('>',p);if(e==std::string::npos)break;auto tag=s.substr(p,e-p+1);p=e+1;if(!hasClass(tag,"item"))continue;auto end=s.find("</a>",p);if(end==std::string::npos)break;Game g;g.url=absolute(attr(tag,"href"));if(sourceForURL(g.url)!=1)continue;g.id=atoi(g.url.c_str()+23);if(g.id<=0)continue;auto b=s.substr(p,end-p);auto i=b.find("<img");if(i!=std::string::npos)g.image=absolute(attr(b.substr(i,b.find('>',i)-i+1),"src"));size_t d=0;while((d=b.find('<',d))!=std::string::npos){auto z=b.find('>',d);if(z==std::string::npos)break;auto t=b.substr(d,z-d+1);if(hasClass(t,"item__title")){auto q=b.find("</div>",z);g.title=plain(b.substr(z+1,q-z-1));}d=z+1;}
    size_t x=0;for(int k=0;k<2;++k){x=b.find("<span",x);if(x==std::string::npos)break;x=b.find('>',x);auto z=b.find("</span>",x);if(z==std::string::npos)break;(k?g.size:g.year)=plain(b.substr(x+1,z-x-1));x=z+7;}
    if(!g.title.empty()&&sourceURL(g.image))v.push_back(g);p=end+4;}return v;}
inline int lastPage(const std::string&s){int n=1;size_t p=0;while((p=s.find("/page/",p))!=std::string::npos){int a=atoi(s.c_str()+p+6);if(a>n&&a<=500)n=a;p+=6;}return n;}
inline std::string serialize(const std::vector<Game>&v){std::string s;for(auto&g:v){s+=std::to_string(g.id);for(auto x:{g.title,g.year,g.size,g.url,g.image}){for(char&c:x)if(c=='\t'||c=='\r'||c=='\n')c=' ';s+='\t';s+=x;}s+='\n';}return s;}
inline std::vector<Game> loadCatalog(const std::string&s){std::vector<Game>v;size_t p=0;while(p<s.size()){auto e=s.find('\n',p);if(e==std::string::npos)e=s.size();std::string line=s.substr(p,e-p);p=e+1;std::vector<std::string>f;size_t a=0;for(;;){auto b=line.find('\t',a);f.push_back(line.substr(a,b-a));if(b==std::string::npos)break;a=b+1;}if(f.size()!=6)continue;Game g;g.id=atoi(f[0].c_str());g.title=f[1];g.year=f[2];g.size=f[3];g.url=f[4];g.image=f[5];if(g.id>0&&!g.title.empty()&&sourceURL(g.url)&&sourceURL(g.image))v.push_back(g);}return v;}
inline std::string lower(std::string s){for(char&c:s)if((unsigned char)c<128)c=tolower((unsigned char)c);return s;}
inline double sizeMB(std::string s){std::replace(s.begin(),s.end(),',','.');double n=atof(s.c_str());return lower(s).find("gb")!=std::string::npos?n*1024:n;}
