#pragma once

// Кадр ответа хосту на двух портах чтения (протокол v2): слова ответа на все
// байты команды, индекс чтения порта данных, порядок публикации.
//
// Приёмник Sink подставляет платформа:
//   Sink::kCmd, Sink::kDat - порты статуса и данных;
//   set(port, word)        - слово ответа порта;
//   encode(byte)           - слово ответа по байту;
//   publish_fence()        - записи до вызова видны хосту раньше записей после.
//
// Счётчиков состоявшихся чтений нет: плата не считает, сколько байт хост
// вычитал (инвариант протокола v2), отсюда бесплатное перечитывание кадра.

#include <cstdint>

#include "platform/compiler.h"
#include "player/protocol/host_protocol.h"

namespace player::protocol::host_frame {

// Слов на одно больше длины кадра: последнее "пусто", на переборе индекс
// упирается в него без отдельной ветки.
inline constexpr uint8_t kWords = HostProtocol::kArgBytes + 1;

// Вооружение: аргументы и слово порта данных пишутся раньше, статус
// публикуется последним. Пока хост не увидел статус, аргументы ему не нужны;
// увидел - они уже готовы.
template <typename Sink, typename Index>
SOUNDSINTH_ALWAYS_INLINE void arm(Sink& sink, uint32_t (&words)[kWords], Index& pos, uint8_t code,
                                  const uint8_t* args, uint8_t n) {
    for (uint8_t i = 0; i < kWords - 1; ++i) {
        words[i] = sink.encode(i < n ? args[i] : 0u);
    }
    words[kWords - 1] = sink.encode(0u);
    pos = 0;
    sink.set(Sink::kDat, words[0]);
    sink.publish_fence();
    sink.set(Sink::kCmd, sink.encode(code));
}

// Статус пуст, слова кадра не трогаются.
template <typename Sink>
SOUNDSINTH_ALWAYS_INLINE void hide(Sink& sink) {
    sink.set(Sink::kCmd, sink.encode(HostProtocol::kStNone));
}

// Байт команды хоста: статус снимается сразу, до разбора. Хост начал писать
// команду - значит текущую он забрал; если ждать разбора, он успеет опросить
// статус ещё раз и исполнить ту же команду дважды. Протокол - ссылкой на
// указатель: он читается после снятия статуса, регистр под него не держится.
template <typename Sink>
SOUNDSINTH_ALWAYS_INLINE void command_byte(Sink& sink, HostProtocol* const& protocol, uint8_t data) {
    hide(sink);
    protocol->on_command_byte(data);
}

// Чтение порта данных состоялось: индекс - на следующее слово, на последнем
// ("пусто") стоит.
template <typename Sink, typename Index>
SOUNDSINTH_ALWAYS_INLINE void read_done(Sink& sink, const uint32_t (&words)[kWords], Index& pos) {
    uint8_t p = pos;
    if (p < kWords - 1) ++p;
    pos = p;
    sink.set(Sink::kDat, words[p]);
}

} // namespace player::protocol::host_frame
