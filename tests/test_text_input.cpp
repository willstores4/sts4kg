#include "../src/text_input_ps4.cpp"
#include <cassert>
#include <iostream>
static int moduleResult=0,commonResult=0,openResult=0,readResult=0,commonCalls=0,openCalls=0,termCalls=0,abortCalls=0;
static bool busy=false;static OrbisDialogStatus status=ORBIS_DIALOG_STATUS_RUNNING;static OrbisDialogResultStatus endStatus=ORBIS_DIALOG_OK;static char16_t*destination=nullptr;
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal){return (uint32_t)moduleResult;}
extern "C" int32_t sceSysmoduleLoadModule(OrbisSysModule){return moduleResult;}
extern "C" int32_t sceCommonDialogInitialize(){++commonCalls;return commonResult;}
extern "C" bool sceCommonDialogIsUsed(){return busy;}
extern "C" int32_t sceImeDialogInit(const OrbisImeDialogSetting*p,OrbisImeSettingsExtended*extra){
    ++openCalls;assert(!extra&&p->userId==7&&p->maxTextLength==128);
    assert(imeDecode(reinterpret_cast<const char16_t*>(p->title),64)=="Buscar homebrew");destination=reinterpret_cast<char16_t*>(p->inputTextBuffer);return openResult;
}
extern "C" OrbisDialogStatus sceImeDialogGetStatus(){return status;}
extern "C" int32_t sceImeDialogGetResult(OrbisDialogResult*r){r->endstatus=endStatus;return readResult;}
extern "C" int32_t sceImeDialogTerm(){++termCalls;return 0;}
extern "C" void sceImeDialogAbort(){++abortCalls;}
int main(){
    const std::string unicode="Pokémon / Игры / 日本語 / 🎮";char16_t text[129]={};imeEncode(unicode,text,129);assert(imeDecode(text,129)==unicode);
    char16_t small[3]={};imeEncode("🎮x",small,3);assert(small[0]==0xd83c&&small[1]==0xdfae&&small[2]==0);imeEncode("🎮",small,2);assert(small[0]==0);
    char16_t invalid[]={0xd800,u'x',0xdc00,0};assert(imeDecode(invalid,4)=="�x�");imeEncode(std::string("\xed\xa0\x80"),text,129);assert(imeDecode(text,129)=="�");
    std::string value="previous query",error;NativeTextInput input;
    assert(!input.begin(-1,value,error)&&openCalls==0);moduleResult=-1;assert(!input.begin(7,value,error)&&openCalls==0);moduleResult=0;
    assert(input.begin(7,unicode,error)&&input.isOpen()&&imeDecode(destination,129)==unicode&&commonCalls==1);
    status=ORBIS_DIALOG_STATUS_NONE;assert(input.poll(value,error)==NativeTextInput::Pending&&input.isOpen());status=ORBIS_DIALOG_STATUS_RUNNING;
    assert(!input.begin(7,"another",error)&&openCalls==1);assert(input.poll(value,error)==NativeTextInput::Pending&&value=="previous query");
    imeEncode("New 🎮 search",destination,129);status=ORBIS_DIALOG_STATUS_STOPPED;assert(input.poll(value,error)==NativeTextInput::Accepted&&value=="New 🎮 search"&&!input.isOpen()&&termCalls==1);
    assert(input.begin(7,value,error)&&commonCalls==1);imeEncode("discard this",destination,129);endStatus=ORBIS_DIALOG_CANCEL;assert(input.poll(value,error)==NativeTextInput::Cancelled&&value=="New 🎮 search");
    assert(input.begin(7,value,error));readResult=-9;assert(input.poll(value,error)==NativeTextInput::Failed&&!input.isOpen()&&value=="New 🎮 search");readResult=0;
    busy=true;assert(!input.begin(7,value,error));busy=false;openResult=-8;assert(!input.begin(7,value,error)&&!input.isOpen());openResult=0;
    status=ORBIS_DIALOG_STATUS_RUNNING;{NativeTextInput owned;assert(owned.begin(7,value,error));}assert(abortCalls==1);
    std::cout<<"Native PS4 keyboard lifecycle, UTF-16/UTF-8, surrogate boundaries, confirmation, cancellation and failures passed.\n";
}
