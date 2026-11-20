#include "../src/providers.h"
#include <iostream>
int main(int argc,char**argv){if(argc<3)return 1;int source=atoi(argv[1]);auto html=readFile(argv[2]);auto games=parseSourceCards(html,source);std::cout<<sourceLastPage(html,source)<<"\n"<<serialize(games);return games.empty()?2:0;}
