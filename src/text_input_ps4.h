#pragma once
#include <cstdint>
#include <string>

class NativeTextInput {
    bool ready = false, opened = false;
    unsigned openingFrames = 0;
    alignas(4) char16_t buffer[129] = {};
    alignas(4) char16_t titleBuf[64] = {};
    alignas(4) char16_t placeholderBuf[64] = {};

public:
    enum Result { Pending, Accepted, Cancelled, Failed };
    ~NativeTextInput();
    bool isOpen() const { return opened; }
    bool begin(int user, const std::string& initial, std::string& error,
               const std::string& title = "Buscar homebrew",
               const std::string& placeholder = "Nome do homebrew",
               bool urlMode = false);
    Result poll(std::string& text, std::string& error);
};
