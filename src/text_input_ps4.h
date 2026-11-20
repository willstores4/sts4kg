#pragma once
#include <string>
#include <cstdint>
class NativeTextInput {
    bool ready=false,opened=false;
    unsigned openingFrames=0;
    alignas(4) char16_t buffer[129]={};
public:
    enum Result {Pending,Accepted,Cancelled,Failed};
    ~NativeTextInput();
    bool isOpen()const{return opened;}
    bool begin(int user,const std::string&initial,std::string&error);
    Result poll(std::string&text,std::string&error);
};
