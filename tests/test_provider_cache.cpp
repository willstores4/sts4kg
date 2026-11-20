#define PORTAL_HOST
#include "../src/providers.h"
#include <cassert>
#include <unistd.h>
#include <iostream>
int main(){char temp[]="/tmp/nullstore-cache-XXXXXX";assert(mkdtemp(temp));std::string root=temp;std::vector<Game>all=loadCatalog(readFile("assets/catalog.tsv"));for(int source=1;source<=3;source++){std::vector<Game>part;for(auto&g:all)if(sourceForURL(g.url)==source)part.push_back(g);assert(saveProviderCache(root,source,part,true,1000));assert(providerCacheFresh(root,source,1100));assert(!providerCacheFresh(root,source,1000+6*60*60));assert(!providerCacheFresh(root,source,999));assert(providerCached(root,source).size()==part.size());}auto mixed=mergedCatalog("assets/catalog.tsv",root+"/catalog.tsv");assert(mixed.size()==all.size());auto first=providerCached(root,1);assert(!saveProviderCache(root,2,first));assert(!saveProviderCache(root,1,{}));assert(saveProviderCache(root,1,first,false));assert(writeFile(providerCachePath(root,1),"corrupt"));assert(providerCached(root,1).size()==first.size());assert(providerCached(root,2).size()>1000);std::cout<<"Per-provider cache isolation, expiry, clock rollback, checksum recovery and offline merge passed.\n";}
