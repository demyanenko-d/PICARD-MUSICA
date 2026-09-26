#pragma once

// soundsinth::formats::ByteSource поверх протокола с хостом. pump - функция вызывающего, продвигает
// протокол на шаг (на плате заодно обслуживает карту); колбэки протокола
// зовутся из неё, новый запрос куска из них делать нельзя.
// Окно одно, 4096 байт, в буфере приёма протокола без копии: хост пишет в порт
// данных только по запросу платы, новый запрос делает этот класс.

#include <cstdint>

#include "core/formats/byte_source.h"
#include "player/protocol/host_protocol.h"

namespace player::load {

class BusByteSource {
public:
    using PumpFn = void (*)(void* user);

    // pump/pump_user - вызывается в цикле ожидания ответа, должен продвинуть
    // протокол хотя бы на шаг и вернуться, не блокируя.
    BusByteSource(player::protocol::HostProtocol& protocol, uint32_t file_length, PumpFn pump, void* pump_user)
        : protocol_(protocol), file_length_(file_length), pump_(pump), pump_user_(pump_user) {}

    soundsinth::formats::ByteSource as_byte_source() {
        return soundsinth::formats::ByteSource{this, &read_fn, &seek_fn, &size_fn};
    }

    // Передаётся владельцем колбэков протокола (набор Callbacks один на весь
    // протокол, BusByteSource сам их не регистрирует).
    void on_reset() { aborted_ = true; }

    bool aborted() const { return aborted_; }

private:
    static uint32_t read_fn(void* self, void* dst, uint32_t n);
    static bool seek_fn(void* self, uint32_t offset);
    static uint32_t size_fn(void* self);

    // Запрашивает окно [offset, offset+n) и крутит pump_ до ответа или aborted_.
    // Повторов нет: кадр сверяет хост, данные окна не проверяются; после
    // исчерпания попыток и при сбросе - окно пустое.
    void request_window(uint32_t offset, uint32_t n);

    // Окно - байты в буфере приёма протокола, без копии. Годно до следующего
    // request_file_chunk(), который делает только request_window, сперва
    // обнулив window_len_.
    const uint8_t* window_ = nullptr;
    uint32_t window_base_ = 0;
    uint32_t window_len_ = 0;

    player::protocol::HostProtocol& protocol_;
    uint32_t file_length_;
    PumpFn pump_;
    void* pump_user_;

    uint32_t pos_ = 0;

    // Пишет on_reset из poll() внутри pump_ - тот же поток, что ждёт окна.
    bool aborted_ = false;
};

} // namespace player::load
