#pragma once
#include <string>
bool portalNetworkInit();
bool portalNetworkFetch(const std::string&,std::string&,size_t,const std::string&);
void portalNetworkTrace(const char*,...);
