#include "../src/ui.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
int main(){Canvas c("assets");c.rect(0,0,512,512,0x050d0b);c.rounded(28,28,456,456,28,0x063b29);c.rounded(32,32,448,448,25,0x050d0b);for(int y=60;y<450;y+=17)for(int x=45;x<470;x+=13)if((x*17+y*23)%19<3)c.rect(x,y,5,2,0x124833);c.rect(72,111,110,88,0x00ff66);c.rect(78,117,98,76,0x050d0b);c.text(">_",87,179,64,0x00ff66);c.text("NULL",67,308,112,0xe7f3ed);c.text("_store",72,382,74,0x00ff66);c.text("YOUR SYSTEM. YOUR RULES.",73,439,21,0xe7f3ed);std::vector<unsigned char>rgb(512*512*3);for(int y=0;y<512;y++)for(int x=0;x<512;x++){auto p=c.pixels[y*1920+x];int i=(y*512+x)*3;rgb[i]=p>>16;rgb[i+1]=p>>8;rgb[i+2]=p;}return !stbi_write_png("sce_sys/icon0.png",512,512,3,rgb.data(),0);}
