#pragma once
#include "download_paths.h"
#include "download_model.h"
#include "installer.h"
#include <stdint.h>
#include <cstring>
#include <cmath>
#define STBI_MAX_DIMENSIONS 4096
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>
struct Bitmap{int w=0,h=0;std::vector<unsigned char> p,a;};
struct Glyph{int w,h,x,y,advance;std::vector<unsigned char>b;};
struct Canvas{
    static const int W=1920,H=1080;
    std::vector<uint32_t> pixels;std::vector<unsigned char>font;stbtt_fontinfo fi;std::map<int,Glyph>glyphs;
    Canvas(const std::string&base):pixels(W*H){auto s=readFile(base+"/font.ttf");font.assign(s.begin(),s.end());if(font.empty()||!stbtt_InitFont(&fi,font.data(),0)){fprintf(stderr,"Font missing\n");exit(1);}}
    void rect(int x,int y,int w,int h,uint32_t c){int r=std::min(W,x+w),b=std::min(H,y+h);for(int j=std::max(0,y);j<b;++j)std::fill(pixels.begin()+j*W+std::max(0,x),pixels.begin()+j*W+std::max(std::max(0,x),r),0xff000000|c);}
    void blend(int x,int y,uint32_t c,int a){if(x<0||y<0||x>=W||y>=H)return;auto &d=pixels[y*W+x];int r=(((c>>16)&255)*a+((d>>16)&255)*(255-a))/255;int g=(((c>>8)&255)*a+((d>>8)&255)*(255-a))/255;int b=((c&255)*a+(d&255)*(255-a))/255;d=0xff000000|(r<<16)|(g<<8)|b;}
    void line(int x,int y,int xx,int yy,uint32_t color){int steps=std::max(abs(xx-x),abs(yy-y));for(int i=0;i<=steps;i++)blend(x+(xx-x)*i/std::max(1,steps),y+(yy-y)*i/std::max(1,steps),color,255);}
    void rounded(int x,int y,int w,int h,int radius,uint32_t color){for(int j=0;j<h;j++)for(int i=0;i<w;i++){int dx=i<radius?radius-i:i>=w-radius?i-(w-radius-1):0,dy=j<radius?radius-j:j>=h-radius?j-(h-radius-1):0;if(dx*dx+dy*dy<=radius*radius)blend(x+i,y+j,color,255);}}
    void region(const Bitmap&b,int sx,int sy,int sw,int sh,int x,int y,int w,int h){if(b.p.empty()||w<=0||h<=0||sw<=0||sh<=0)return;for(int j=0;j<h;j++)for(int i=0;i<w;i++){int dx=x+i,dy=y+j;if(dx<0||dy<0||dx>=W||dy>=H)continue;float fx=std::max(0.f,std::min(float(b.w-1),sx+(i+.5f)*sw/w-.5f)),fy=std::max(0.f,std::min(float(b.h-1),sy+(j+.5f)*sh/h-.5f));int x0=int(fx),y0=int(fy),x1=std::min(b.w-1,x0+1),y1=std::min(b.h-1,y0+1);float tx=fx-x0,ty=fy-y0;const unsigned char*p00=&b.p[(y0*b.w+x0)*3],*p10=&b.p[(y0*b.w+x1)*3],*p01=&b.p[(y1*b.w+x0)*3],*p11=&b.p[(y1*b.w+x1)*3];int rgb[3];for(int k=0;k<3;k++){float top=p00[k]*(1-tx)+p10[k]*tx,bottom=p01[k]*(1-tx)+p11[k]*tx;rgb[k]=int(top*(1-ty)+bottom*ty+.5f);}uint32_t color=(rgb[0]<<16)|(rgb[1]<<8)|rgb[2];if(b.a.empty())pixels[dy*W+dx]=0xff000000|color;else{float top=b.a[y0*b.w+x0]*(1-tx)+b.a[y0*b.w+x1]*tx,bottom=b.a[y1*b.w+x0]*(1-tx)+b.a[y1*b.w+x1]*tx;blend(dx,dy,color,int(top*(1-ty)+bottom*ty+.5f));}}}
    Glyph& glyph(unsigned cp,int size){int key=cp*128+size;auto it=glyphs.find(key);if(it!=glyphs.end())return it->second;Glyph g;float scale=stbtt_ScaleForPixelHeight(&fi,size);int adv,lsb;stbtt_GetCodepointHMetrics(&fi,cp,&adv,&lsb);g.advance=int(adv*scale);auto b=stbtt_GetCodepointBitmap(&fi,0,scale,cp,&g.w,&g.h,&g.x,&g.y);if(b){g.b.assign(b,b+g.w*g.h);stbtt_FreeBitmap(b,0);}return glyphs.emplace(key,g).first->second;}
    int text(const std::string&s,int x,int y,int size,uint32_t color,int maxW=1800){int start=x;for(size_t i=0;i<s.size();){unsigned cp=(unsigned char)s[i++];if(cp>=192){int n=cp<224?1:cp<240?2:3;cp&=n==1?31:n==2?15:7;while(n--&&i<s.size())cp=(cp<<6)|((unsigned char)s[i++]&63);}auto&g=glyph(cp,size);if(x+g.advance>start+maxW)break;for(int j=0;j<g.h;++j)for(int k=0;k<g.w;++k)if(g.b[j*g.w+k])blend(x+k+g.x,y+j+g.y,color,g.b[j*g.w+k]);x+=g.advance;}return x-start;}
    void lines(std::string s,int x,int y,int size,uint32_t c,int width,int count){for(int n=0;n<count&&!s.empty();++n){int used=0;size_t end=0,last=0;while(end<s.size()){unsigned cp=(unsigned char)s[end];size_t step=cp<128?1:cp<224?2:cp<240?3:4;unsigned code=cp;if(step>1){code&=step==2?31:step==3?15:7;for(size_t z=1;z<step&&end+z<s.size();++z)code=(code<<6)|((unsigned char)s[end+z]&63);}if(used+glyph(code,size).advance>width)break;used+=glyph(code,size).advance;if(s[end]==' ')last=end;end+=step;}if(!end)break;if(end<s.size()&&last)end=last;text(s.substr(0,end),x,y+n*(size+5),size,c,width);s=s.substr(end);while(!s.empty()&&s[0]==' ')s.erase(0,1);}}
    void image(const Bitmap&b,int x,int y,int w,int h){if(b.p.empty())return;float scale=std::max(float(w)/b.w,float(h)/b.h);int sw=std::max(1,int(w/scale)),sh=std::max(1,int(h/scale));region(b,(b.w-sw)/2,(b.h-sh)/2,sw,sh,x,y,w,h);}
};
inline Bitmap loadBitmap(const std::string&p){Bitmap b;int c;unsigned char*d=stbi_load(p.c_str(),&b.w,&b.h,&c,3);if(d){b.p.assign(d,d+b.w*b.h*3);stbi_image_free(d);}return b;}
inline Bitmap loadBitmapRGBA(const std::string&p){Bitmap b;int c;unsigned char*d=stbi_load(p.c_str(),&b.w,&b.h,&c,4);if(d){b.p.resize(size_t(b.w)*b.h*3);b.a.resize(size_t(b.w)*b.h);for(size_t i=0;i<b.a.size();++i){b.p[i*3]=d[i*4];b.p[i*3+1]=d[i*4+1];b.p[i*3+2]=d[i*4+2];b.a[i]=d[i*4+3];}stbi_image_free(d);}return b;}
inline Bitmap loadGreenCase(const std::string&p){Bitmap b=loadBitmapRGBA(p);for(size_t i=0;i<b.a.size();++i){unsigned char*r=&b.p[i*3];if(r[2]>r[0]*1.18f&&r[2]>r[1]*1.02f){int blue=r[2],green=r[1];r[0]=(unsigned char)std::min(255,int(r[0]/2+blue/32));r[1]=(unsigned char)std::min(255,std::max(green,int(blue*.90f)));r[2]=(unsigned char)std::min(255,green/3+blue/5);}}return b;}
inline Bitmap packedBitmap(const std::string&base,int id){Bitmap b;auto index=readFile(base+"/covers.idx");size_t at=0;unsigned long long offset=0;size_t length=0;bool found=false;while(at<index.size()){int key=0;if(sscanf(index.c_str()+at,"%d\t%llu\t%zu",&key,&offset,&length)==3&&key==id){found=true;break;}auto next=index.find('\n',at);if(next==std::string::npos)break;at=next+1;}if(!found||!length||length>3*1024*1024)return b;FILE*f=fopen((base+"/covers.pack").c_str(),"rb");if(!f)return b;std::vector<unsigned char>bytes(length);bool ok=!fseek(f,offset,SEEK_SET)&&fread(bytes.data(),1,length,f)==length;fclose(f);if(ok){int channels;auto p=stbi_load_from_memory(bytes.data(),bytes.size(),&b.w,&b.h,&channels,3);if(p){b.p.assign(p,p+b.w*b.h*3);stbi_image_free(p);}}return b;}
enum Action{UP,DOWN,LEFT,RIGHT,CONFIRM,BACK,SEARCH,FAVORITE,TAB_PREV,TAB_NEXT,SORT,REFRESH,PAGE_PREV,PAGE_NEXT,DOWNLOADS,SERVICE,SELECT_SOURCE,REMOVE_DOWNLOAD};
struct UI{
    Canvas c;std::string base,data,status="Bundled catalog | OPTIONS to refresh",query;Bitmap logo,caseFrame;std::vector<Game>games;std::vector<int>visible;std::set<int>favorites;std::map<int,Bitmap>covers;int selected=0,tab=0,sort=0,key=0;bool detail=false,keyboard=false,dirty=true;std::string notice;
    std::vector<DownloadRow>jobs;int jobSelected=0,sourceSelected=0;std::vector<TorrentLink>sources;
    bool sourcePicker=false,sourceLoading=false,downloadRequest=false,enqueueRequest=false;Game downloadGame;
    std::string sourceError;
    bool sourceFilterPicker=false,backgroundPromptShown=false;int sourceFilter=0,sourceChoice=0;
    bool servicePicker=false,serviceStartRequest=false,serviceStopRequest=false,serviceTestRequest=false;int serviceSelected=0;
    bool downloadLocationPicker=false,downloadLocationRequest=false;int downloadLocationSelected=0;std::string downloadBase,requestedDownloadBase;
    std::string serviceStatus="Connecting to background service...";

    bool removePicker=false,removeRequest=false,removeDeleteFiles=false;int removeJob=0,removeChoice=2;
    bool installPicker=false,installLoading=false,installScanRequest=false,installSubmitRequest=false;
    int installJob=0,installSelected=0;std::string installPath;std::vector<InstallPackage>packages;std::string installError;

    int selectedJob(){return jobs.empty()?0:jobs[std::min(jobSelected,int(jobs.size())-1)].id;}
    void beginDownload(){if(!current())return;downloadGame=*current();sourcePicker=true;sourceLoading=true;sourceSelected=0;sources.clear();sourceError.clear();downloadRequest=true;dirty=true;}

    UI(std::string b,std::string d):c(b),base(b),data(d),logo(loadBitmapRGBA(base+"/brand/nullstore-bag.png")),caseFrame(loadGreenCase(base+"/brand/ps4-case-frame.png")){downloadBase=readFile(data+"/download-location.txt");while(!downloadBase.empty()&&(downloadBase.back()=='\n'||downloadBase.back()=='\r'))downloadBase.pop_back();if(!validDownloadBase(data,downloadBase))downloadBase=data;for(int i=0;i<8;i++)if(downloadBase=="/mnt/usb"+std::to_string(i)+"/NULL_store")downloadLocationSelected=i+1;if(downloadBase==data)downloadLocationSelected=0;backgroundPromptShown=!readFile(data+"/background-tip-shown.txt").empty();games=mergedCatalog(base+"/catalog.tsv",data+"/catalog.tsv");std::string s=readFile(data+"/favorites.txt");size_t p=0;while(p<s.size()){favorites.insert(atoi(s.c_str()+p));auto e=s.find('\n',p);if(e==std::string::npos)break;p=e+1;}filter();}
    void filter(){visible.clear();for(size_t i=0;i<games.size();++i)if((!sourceFilter||sourceForURL(games[i].url)==sourceFilter)&&(tab!=1||favorites.count(games[i].id))&&(tab!=2||atoi(games[i].year.c_str())>=2024)&&lower(games[i].title).find(lower(query))!=std::string::npos)visible.push_back(i);std::map<int,int>ranks;int sourceRanks[5]={};for(int index:visible)ranks[index]=sourceRanks[sourceForURL(games[index].url)]++;std::stable_sort(visible.begin(),visible.end(),[&](int a,int b){auto&x=games[a];auto&y=games[b];if(sort==1)return lower(x.title)<lower(y.title);if(sort==2)return x.year>y.year;if(sort==3)return sizeMB(x.size)<sizeMB(y.size);return ranks[a]!=ranks[b]?ranks[a]<ranks[b]:sourceForURL(x.url)<sourceForURL(y.url);});selected=std::max(0,std::min(selected,int(visible.size())-1));dirty=true;}
    Game* current(){return visible.empty()?nullptr:&games[visible[selected]];}
    std::string downloadLocationName(int index)const{return index==0?"Internal storage":"USB "+std::to_string(index-1);}
    std::string downloadLocationPath(int index)const{return index==0?data:"/mnt/usb"+std::to_string(index-1)+"/NULL_store";}
    Bitmap& cover(Game&g){auto it=covers.find(g.id);if(it!=covers.end())return it->second;if(covers.size()>=20)covers.clear();auto b=loadBitmap(providerCoverPath(data,sourceForURL(g.url))+"/"+std::to_string(g.id)+".jpg");if(b.p.empty())b=loadBitmap(data+"/covers/"+std::to_string(g.id)+".jpg");if(b.p.empty())b=loadBitmap(base+"/covers/"+std::to_string(g.id)+".jpg");if(b.p.empty())b=packedBitmap(base,g.id);return covers.emplace(g.id,std::move(b)).first->second;}
    void favorite(){auto g=current();if(!g)return;if(favorites.count(g->id))favorites.erase(g->id);else favorites.insert(g->id);std::string s;for(int id:favorites)s+=std::to_string(id)+"\n";if(!writeFile(data+"/favorites.txt",s))notice="Could not save favorites. Check storage permissions.";filter();}
    void action(Action a){dirty=true;notice.clear();
        if(removePicker){if(a==BACK){removePicker=false;return;}if(a==UP||a==DOWN)removeChoice=(removeChoice+(a==UP?2:1))%3;if(a==CONFIRM){removeRequest=removeChoice!=2;removeDeleteFiles=removeChoice==1;removePicker=false;}return;}
        if(downloadLocationPicker){if(a==BACK){downloadLocationPicker=false;servicePicker=true;return;}if(a==UP)downloadLocationSelected=std::max(0,downloadLocationSelected-1);if(a==DOWN)downloadLocationSelected=std::min(8,downloadLocationSelected+1);if(a==CONFIRM){requestedDownloadBase=downloadLocationPath(downloadLocationSelected);downloadLocationRequest=true;downloadLocationPicker=false;}return;}
        if(a==REMOVE_DOWNLOAD&&tab==3&&!jobs.empty()){removeJob=selectedJob();removeChoice=2;removePicker=true;return;}
        if(sourceFilterPicker){if(a==BACK){sourceFilterPicker=false;return;}if(a==UP)sourceChoice=std::max(0,sourceChoice-1);if(a==DOWN)sourceChoice=std::min(3,sourceChoice+1);if(a==CONFIRM){sourceFilterPicker=false;sourceFilter=sourceChoice;selected=0;filter();}return;}
        if(a==SELECT_SOURCE&&!detail&&!keyboard){sourceFilterPicker=true;sourceChoice=sourceFilter;return;}
        if(servicePicker){if(a==BACK){servicePicker=false;return;}if(a==UP)serviceSelected=std::max(0,serviceSelected-1);if(a==DOWN)serviceSelected=std::min(4,serviceSelected+1);if(a==CONFIRM){if(serviceSelected==0)serviceStartRequest=true;if(serviceSelected==1&&serviceStatus.find("In-app")!=0)serviceStopRequest=true;if(serviceSelected==2)serviceTestRequest=true;if(serviceSelected==3){downloadLocationPicker=true;downloadLocationSelected=downloadBase==data?0:downloadLocationSelected;servicePicker=false;}if(serviceSelected==4){servicePicker=false;return;}servicePicker=false;}return;}
        if(installPicker){if(a==BACK){installPicker=false;return;}if(!packages.empty()){if(a==UP)installSelected=std::max(0,installSelected-1);if(a==DOWN)installSelected=std::min(int(packages.size())-1,installSelected+1);if(a==CONFIRM&&!installLoading)installSubmitRequest=true;}return;}
        if(a==DOWNLOADS){tab=tab==3?0:3;sourcePicker=false;detail=false;keyboard=false;return;}
        if(sourcePicker){if(a==BACK){sourcePicker=false;return;}if(!sources.empty()){if(a==UP)sourceSelected=std::max(0,sourceSelected-1);if(a==DOWN)sourceSelected=std::min(int(sources.size())-1,sourceSelected+1);if(a==CONFIRM)enqueueRequest=true;}return;}
        if(tab==3){if(a==SERVICE){servicePicker=true;serviceSelected=0;return;}if(a==SEARCH){if(jobs.empty()||jobs[std::min(jobSelected,int(jobs.size())-1)].state!=DownloadState::Complete){notice="Finish the download before installing its packages.";return;}int index=std::min(jobSelected,int(jobs.size())-1);installJob=jobs[index].id;installPath=validDownloadDir(data,installJob,jobs[index].outputDir)?jobs[index].outputDir:defaultDownloadDir(data,installJob);installSelected=0;packages.clear();installPicker=installLoading=installScanRequest=true;installError.clear();return;}if(a==BACK){tab=0;return;}if(a==TAB_PREV||a==TAB_NEXT){tab=a==TAB_NEXT?0:2;filter();return;}if(a==UP)jobSelected=std::max(0,jobSelected-1);if(a==DOWN)jobSelected=std::min(std::max(0,int(jobs.size())-1),jobSelected+1);return;}
if(a==FAVORITE&&!keyboard){favorite();return;}if(a==BACK){if(keyboard)keyboard=false;else if(detail)detail=false;else{query.clear();tab=0;filter();}return;}
        if(keyboard){if(a==LEFT)key=(key+39)%40;if(a==RIGHT)key=(key+1)%40;if(a==UP)key=(key+30)%40;if(a==DOWN)key=(key+10)%40;if(a==CONFIRM){const std::string keys="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 <OK";if(key<36&&query.size()<64)query+=keys[key];if(key==36&&query.size()<64)query+=' ';if(key==37&&!query.empty())query.pop_back();if(key==38)query.clear();if(key==39)keyboard=false;selected=0;filter();}return;}
        if(a==SEARCH){keyboard=true;detail=false;key=0;return;}if(detail){if(a==CONFIRM)beginDownload();return;}
        if(a==TAB_PREV||a==TAB_NEXT){tab=(tab+(a==TAB_NEXT?1:3))%4;selected=0;filter();return;}if(a==SORT){sort=(sort+1)%4;filter();return;}
        int n=visible.size();if(!n)return;if(a==LEFT)selected=std::max(0,selected-1);if(a==RIGHT)selected=std::min(n-1,selected+1);if(a==UP)selected=std::max(0,selected-6);if(a==DOWN)selected=std::min(n-1,selected+6);if(a==PAGE_PREV)selected=std::max(0,selected-12);if(a==PAGE_NEXT)selected=std::min(n-1,selected+12);if(a==CONFIRM)detail=true;
    }
    void drawCase(Game&g,int x,int y,int side){
        auto&b=cover(g);const int frameW=caseFrame.p.empty()?side*77/100:side*caseFrame.w/caseFrame.h,frameX=x+(side-frameW)/2,frameY=y;
        const int artX=frameX+frameW*25/1024,artY=y+side*183/1325,artW=frameW*(1000-25)/1024,artH=side*(1290-183)/1325;
        c.rounded(frameX-2,y+3,frameW+4,side,7,0x020905);c.rect(artX,artY,artW,artH,0x07100d);
        if(!b.p.empty()){
            int trim=0;auto blueRow=[&](int row){int blue=0,total=0;for(int i=0;i<b.w;i+=3){auto p=&b.p[(row*b.w+i)*3];if(p[2]>p[0]*1.35&&p[2]>p[1]*1.05)blue++;total++;}return total?blue*100/total:0;};
            if(blueRow(std::min(b.h-1,3))>30){int max=b.h*18/100;trim=b.h*7/100;for(int row=trim;row<max;row++)if(blueRow(row)<15){trim=row;break;}}
            float scale=std::max(float(artW)/b.w,float(artH)/(b.h-trim));int sw=std::min(b.w,int(artW/scale)),sh=std::min(b.h-trim,int(artH/scale));int sx=(b.w-sw)/2,sy=trim+(b.h-trim-sh)/3;c.region(b,sx,sy,sw,sh,artX,artY,artW,artH);
        }else{c.rect(artX,artY,artW,artH,0x0c2118);c.text("NULL_store",x+20,y+side/2,25,0xe7f3ed,side-30);}
        if(!caseFrame.p.empty())c.image(caseFrame,frameX,frameY,frameW,side);
        else{c.rounded(frameX,y,frameW,side,8,0x0b3020);c.rounded(frameX+2,y+2,frameW-4,side-4,6,0x286443);c.rect(frameX+4,y+4,frameW-8,side*12/100,0x0c4b32);c.text("PS4",frameX+13,y+26,18,0xf5fff9,60);}
    }
    void draw(){const uint32_t bg=0x06100d,panel=0x0b1915,muted=0xb2c2b9,white=0xe7f3ed,cyan=0x31b878,rule=0x20372c;c.rect(0,0,1920,1080,bg);
        // Quiet storefront header: flat, neutral colour keeps the catalogue artwork in focus.
        c.rect(315,0,1605,204,0x081511);c.rect(315,203,1605,1,0x1a2e25);c.rect(315,0,2,204,0x0f281d);
        c.rect(314,0,1,989,rule);c.image(logo,24,18,68,68);c.text("NULL_store",101,78,37,white,211);c.text("YOUR SYSTEM. YOUR RULES.",37,116,15,white,265);c.text("GAME LIBRARY",40,165,20,muted);
        const char*tabs[]={"All games","Favorites","2024 & newer","Downloads"};for(int i=0;i<4;++i){int y=247+i*65;if(i==tab){c.rounded(29,y-30,258,46,6,0x10241b);c.rect(29,y-30,3,46,cyan);}c.text(tabs[i],65,y,i==tab?27:26,i==tab?white:muted);}c.text("L1 / R1",69,555,20,cyan);c.text("Switch collection",69,590,22,muted);c.text("L3  Sources",69,676,20,cyan);c.text(sourceName(sourceFilter),69,713,25,white,225);
        c.text(std::to_string(games.size())+" games",69,899,25,white);c.text("3 catalog sources",69,935,21,muted);c.text("NULL_store /",352,65,20,cyan);c.text(tabs[tab],485,65,20,white);c.text(query.empty()?"Find your next game.":"Search: "+query,352,126,46,white,1220);const char*sorts[]={"Recently added","Title A-Z","Release year","Smallest first"};c.rounded(1662,25,236,42,5,0x0d2018);c.rect(1662,25,236,1,rule);c.rect(1662,66,236,1,rule);c.text("SORT  "+std::string(sorts[sort]),1676,52,18,muted,208);c.text(std::to_string(visible.size())+" games   |   "+status,352,173,21,muted,1510);
        if(visible.empty()){c.text("No games found",352,400,42,white);c.text(tab==1?"Press SQUARE on a game to save it here.":"Press TRIANGLE to change your search, or CIRCLE to reset.",352,455,25,muted);}else{int start=selected/12*12;for(int i=start;i<std::min(start+12,int(visible.size()));++i){auto&g=games[visible[i]];int k=i-start,x=352+(k%6)*257,y=214+(k/6)*362;bool active=i==selected;if(active)c.rounded(x-2,y-2,238,332,6,cyan);c.rounded(x,y,234,328,5,panel);drawCase(g,x,y,234);c.text(g.year.empty()?"PS4":g.year,x+11,y+257,19,white);c.text(sourceName(sourceForURL(g.url)),x+73,y+257,16,cyan,150);if(favorites.count(g.id))c.text("*",x+218,y+257,18,cyan);c.lines(g.title,x+11,y+282,20,white,212,2);c.text(g.size,x+11,y+322,18,muted);}
        c.text("Page "+std::to_string(selected/12+1)+" / "+std::to_string((visible.size()+11)/12),352,965,22,muted);}
        c.rect(0,989,1920,91,bg);c.rect(0,989,1920,1,0x334b42);for(int x=0;x<1920;x+=13)if((x*7)%17<3)c.rect(x,1065+(x%3)*3,7,2,0x124833);
        // Each screen gets a single controller legend. Downloads previously
        // painted its own legend on top of this one, doubling every label.
        if(tab!=3){
            c.rounded(352,1011,26,26,13,0x6affe0);c.rounded(354,1013,22,22,11,bg);c.text("X",358,1031,22,0x6affe0);c.text("Details",392,1032,22,white);
            c.line(500,1012,487,1035,0x6affe0);c.line(487,1035,513,1035,0x6affe0);c.line(513,1035,500,1012,0x6affe0);c.text("Search",525,1032,22,white);c.rounded(623,1012,25,25,12,white);c.rounded(625,1014,21,21,10,bg);c.rect(630,1018,11,12,white);c.rect(632,1020,7,8,bg);c.text("Save",665,1032,22,white);
            c.text("L2 / R2",747,1032,22,white);c.text("Page",837,1032,22,white);c.rounded(908,1012,32,24,3,muted);c.rounded(910,1014,28,20,2,bg);for(int y=1017;y<1033;y+=5)for(int x=914;x<938;x+=5)c.rect(x,y,1,1,muted);c.text("TOUCHPAD",956,1032,20,white);c.text("Sort",1082,1032,22,white);
            c.rounded(1137,1012,26,24,3,white);c.rounded(1139,1014,22,20,2,bg);c.text("R3",1141,1030,16,white);c.text("Downloads",1180,1032,22,white);c.rounded(1320,1012,65,24,3,white);c.rounded(1322,1014,61,20,2,bg);c.text("OPTIONS",1326,1030,15,white);c.text("Refresh",1403,1032,22,white);
        }else{
            c.text("X  Pause / Resume",352,1034,21,white,230);c.text("TRIANGLE  Install",604,1034,21,white,230);c.text("SQUARE  Cancel",873,1034,21,white,225);c.text("L3  Service",1128,1034,21,white,182);c.text("OPTIONS  Remove",1355,1034,21,white,240);c.text("CIRCLE  Back",1620,1034,21,white,200);
        }
        if(detail&&current()){auto&g=*current();c.rect(285,210,1590,770,0x063b29);c.rect(285,210,7,770,cyan);auto&b=cover(g);if(!b.p.empty())c.image(b,330,255,410,560);c.text(std::string(sourceName(sourceForURL(g.url)))+" / GAME DETAILS",795,287,22,cyan);c.lines(g.title,795,365,49,white,1000,3);c.text(g.year+"    /    "+g.size+"    /    PlayStation 4",795,552,29,muted);c.text(favorites.count(g.id)?"Saved to favorites":"SQUARE  Save to favorites",795,621,27,cyan);c.rect(795,661,980,66,cyan);c.text("X  Download",821,706,34,bg);c.text("L3  Open source page     R3  Downloads",795,765,27,muted);c.text("CIRCLE  Return to catalog",795,808,24,muted);c.lines(g.url,795,850,23,muted,990,3);}
        if(tab==3){
            // Clear the underlying catalogue title as well; its tall glyphs
            // were peeking above the old panel edge on this screen.
            c.rect(285,0,1635,989,bg);c.rect(314,0,1,989,rule);c.text("NULL_store /",352,65,20,cyan);c.text("Downloads",485,65,20,white);
            c.text("Downloads",352,156,46,white);c.text("TRANSFER QUEUE  /  UP TO 2 ACTIVE DOWNLOADS",352,199,18,cyan,720);c.text(serviceStatus,1090,199,19,muted,740);
            if(jobs.empty()){c.text("Your download queue is empty",352,395,42,white);c.text("Open a game and select Download to add its torrent.",352,453,28,muted);}
            jobSelected=std::max(0,std::min(jobSelected,int(jobs.size())-1));int start=jobSelected/5*5;
            for(int i=start;i<std::min(start+5,int(jobs.size()));++i){auto&j=jobs[i];int y=235+(i-start)*139;bool active=i==jobSelected;c.rect(352,y,1478,124,active?0x124833:panel);if(active)c.rect(352,y,5,124,cyan);c.text(j.title,377,y+33,28,white,1178);c.text(stateName(j.state),1580,y+33,24,j.state==DownloadState::Failed?0xffa5a5:cyan,220);
                double fraction=j.total?std::min(1.,double(j.done)/j.total):0;c.rect(377,y+51,1428,7,0x050d0b);c.rect(377,y+51,int(1428*fraction),7,cyan);
                std::string stats="Torrent: "+bytesText(j.done)+" / "+(j.total?bytesText(j.total):"size pending")+"    "+std::to_string(int(fraction*100))+"%    "+bytesText((uint64_t)j.rate)+"/s    "+std::to_string(j.peers)+" peers";c.text(stats,377,y+85,22,white,1408);c.text(j.error.empty()?j.phase:j.error,377,y+113,21,j.error.empty()?muted:0xffa5a5,1408);
            }
            c.text("New downloads: "+downloadLocationName(downloadLocationSelected),352,974,23,muted,1400);
        }
        if(sourcePicker){c.rect(385,235,1210,690,0x09291f);c.text("Download",435,299,43,white);c.lines(downloadGame.title,435,352,30,cyan,1110,2);
            if(sourceLoading)c.text("Reading torrent links from the game page...",435,450,29,white);
            else if(!sourceError.empty())c.lines(sourceError,435,453,29,0xffa5a5,1100,4);
            else {int start=sourceSelected/5*5;for(int i=start;i<std::min(start+5,int(sources.size()));++i){int y=421+(i-start)*78;c.rect(435,y,1110,64,i==sourceSelected?cyan:0x124833);c.text(sources[i].title,455,y+43,27,i==sourceSelected?bg:white,1070);}}
            c.text("X  Add to downloads     CIRCLE  Back",435,877,26,white);
        }
        if(keyboard){c.rect(385,250,1210,670,0x09291f);c.text("Search games",435,310,40,white);c.rect(435,340,1110,65,0x050d0b);c.text(query+"_",457,387,35,white,1060);for(int i=0;i<40;++i){int x=435+(i%10)*112,y=443+(i/10)*87;c.rect(x,y,99,70,i==key?cyan:0x124833);std::string label=i<26?std::string(1,'A'+i):i<36?std::string(1,'0'+i-26):i==36?"Space":i==37?"Back":i==38?"Clear":"Done";c.text(label,x+9,y+45,i<36?33:21,i==key?bg:white,90);}c.text("D-pad  Move     X  Select     CIRCLE  Close",435,860,26,muted);}
        if(installPicker){c.rect(340,210,1300,770,0x09291f);c.text("Install downloaded packages",390,275,42,white);c.text("Choose a package. Install the base game before updates or DLC.",390,325,25,muted);
            if(installLoading)c.text("Checking downloaded PKG files...",390,450,30,white);
            else if(packages.empty())c.lines(installError,390,440,29,0xffa5a5,1200,5);
            else {installSelected=std::max(0,std::min(installSelected,int(packages.size())-1));int start=installSelected/4*4;for(int i=start;i<std::min(start+4,int(packages.size()));++i){auto&p=packages[i];int y=360+(i-start)*115;bool selected=i==installSelected;c.rect(390,y,1200,101,selected?0x124833:0x124833);if(selected)c.rect(390,y,4,101,cyan);c.text(p.name,410,y+35,27,white,1150);c.text(p.titleId+"   /   "+bytesText(p.bytes),410,y+64,22,cyan,1150);c.text(installStatusText(p),410,y+91,21,muted,1150);}}
            if(!installError.empty()&&!packages.empty())c.lines(installError,390,858,23,0xffa5a5,1200,2);
            c.text("X  Install selected package     CIRCLE  Close",390,948,27,white);
        }
        if(servicePicker){c.rect(285,210,1590,785,bg);c.rect(385,250,1210,670,0x09291f);c.text("Background downloads / 1.0",435,315,43,white);c.lines("The separate service keeps transfers running after you close NULL_store. Keep the console awake.",435,380,29,muted,1110,2);const char*options[]={(serviceStatus.find("In-app")==0?"Install background service (recommended)":"Start / Install service"),(serviceStatus.find("In-app")==0?"Continue downloading in NULL_store":"Stop service; use in-app downloads"),"Test torrent connection (one piece only)","Change download location","Back"};for(int i=0;i<5;++i){int y=425+i*78;c.rect(435,y,1110,62,i==serviceSelected?cyan:0x124833);c.text(options[i],460,y+42,26,i==serviceSelected?bg:white,1050);}c.text("Current location: "+downloadLocationName(downloadLocationSelected),435,837,23,muted,1080);c.text("X  Select     CIRCLE  Close",435,882,25,white);}
        if(downloadLocationPicker){c.rect(285,210,1590,785,bg);c.rect(385,230,1210,735,0x09291f);c.text("Choose download location",435,292,42,white);c.text("New downloads will use this location. Existing jobs stay where they are.",435,330,23,muted,1100);for(int i=0;i<9;i++){int y=350+i*54;c.rect(435,y,1110,46,i==downloadLocationSelected?cyan:0x124833);c.text(downloadLocationName(i),455,y+32,22,i==downloadLocationSelected?bg:white,1060);}c.text("USB destinations are checked for write access when selected.",435,860,22,muted,1080);c.text("X  Use location     CIRCLE  Back",435,900,25,white);}
        if(sourceFilterPicker){c.rect(385,235,1210,690,0x09291f);c.text("Catalog sources",435,305,43,white);for(int i=0;i<4;i++){int y=330+i*80;c.rect(435,y,1110,65,i==sourceChoice?cyan:0x124833);c.text(sourceName(i),460,y+43,29,i==sourceChoice?bg:white,1050);}c.text("X  Choose source     CIRCLE  Back",435,877,27,white);}
        if(removePicker){c.rect(385,300,1210,560,0x09291f);c.text("Remove download?",435,370,42,white);c.lines("Choose whether to keep or permanently delete this download's files.",435,430,28,muted,1100,2);const char*choices[]={"Remove from Downloads / keep files","Remove and delete downloaded files","Back"};for(int i=0;i<3;i++){int y=540+i*85;c.rect(435,y,1110,65,i==removeChoice?cyan:0x124833);c.text(choices[i],455,y+43,28,i==removeChoice?bg:white,1060);}}
        if(!notice.empty()){c.rect(310,930,1530,65,0x563448);c.text(notice,335,973,26,white,1480);}dirty=false;
    }
};
