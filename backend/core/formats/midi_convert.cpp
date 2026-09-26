#include "core/formats/midi_convert.h"

#include <cmath>
#include <cstdio>
#include <new>
#include <type_traits>

namespace soundsinth::formats::midi {

// Громкость ячейки: velocity, CC7 и CC11 - все три по квадрату (40 lg), как
// модуляторы SF2 по умолчанию и рекомендация GM. 64 - только при velocity 127
// и полных контроллерах.
// Колонка - round(p^2 * 64 / 127^6), p = velocity * cc7 * cc11, потолок 64.
// С округлением: внизу шкалы шаг крупный (1 -> 2 - это 6 дБ), отбрасывание
// опускает тихие места на 1-1.5 дБ.
//
// Порогами, а не формулой: p^2 не влезает в 32 бита, а 64-битного деления у
// M33 нет - вызов библиотеки на каждую выписанную громкость.
// kVolumeSteps[k - 1] - наименьшее p с громкостью не меньше k; таблица сверена
// с формулой перебором всех p от 0 до 127^3.
// clang-format off
constexpr uint32_t kVolumeSteps[64] = {
    181054, 313594, 404848, 479022, 543160, 600486, 652797, 701216,
    746502, 789193, 829690, 868301, 905266, 940780, 975002, 1008062,
    1040072, 1071126, 1101304, 1130677, 1159307, 1187246, 1214542, 1241239,
    1267373, 1292979, 1318088, 1342727, 1366922, 1390696, 1414071, 1437066,
    1459698, 1481985, 1503941, 1525582, 1546920, 1567967, 1588736, 1609236,
    1629479, 1649474, 1669228, 1688752, 1708053, 1727138, 1746014, 1764689,
    1783168, 1801457, 1819563, 1837490, 1855244, 1872829, 1890251, 1907514,
    1924622, 1941580, 1958390, 1975058, 1991586, 2007977, 2024237, 2040366,
};
// clang-format on

uint8_t cell_volume(uint8_t velocity, uint8_t cc7, uint8_t cc11) {
    const uint32_t p = static_cast<uint32_t>(velocity) * cc7 * cc11; // 0..127^3
    if (p == 0) return 0;
    uint32_t lo = 0, hi = 64;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (kVolumeSteps[mid] <= p) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    // Минимум 1 - только тихой ноте, а не заглушённому каналу: CC7 или CC11
    // = 0 - тишина, как у SF2 (-96 дБ), а не 1/64 колонки.
    return static_cast<uint8_t>(lo == 0 ? 1 : lo);
}

// Скорость вибрато для CC1. Фаза идёт по 64 шагам и продвигается на
// vibrato_speed за тик, значит частота = тиков_в_секунду * speed / 64.
// Цель 5 Гц - столько даёт freqVibLFO банка
// (медиана -851 абсолютный цент). Нибл, поэтому 1..15.
// Считается по текущему темпу трекера: при SetTempo по ходу трека
// выписывается заново.
uint8_t vibrato_speed_at(uint32_t tempo) {
    const uint32_t tps = engine_ticks_per_second(tempo);
    uint32_t vs = (64u * 5u + tps / 2u) / tps;
    if (vs < 1) vs = 1;
    if (vs > 15) vs = 15;
    return static_cast<uint8_t>(vs);
}

// Заголовок Song .mid: модель частот, квирки, сведение, фильтр банка, темп
// и скорость сетки.
void init_song_header(Song& out, Grid grid, uint32_t tracker_tempo) {
    // Song заново, на месте: копия Song{} стояла бы в кадре на стеке
    // Core1 (около 250 байт). Деструктор тривиален.
    static_assert(std::is_trivially_destructible_v<Song>, "Song создаётся поверх прежнего");
    new (&out) Song();
    std::snprintf(out.title, sizeof(out.title), "MIDI");
    out.frequency_model = soundsinth::model::FrequencyModel::Linear;
    // Доезд громкости 5 мс вместо 1.
    out.volume_ramp_samples = 220;
    out.quirks |= soundsinth::model::kQuirkItLinearC5Reference;
    out.quirks |= soundsinth::model::kQuirkFadeoutExponential;
    out.quirks |= soundsinth::model::kQuirkEnvelopeDecibel;
    // Интерполяция линейная: эрмитова на DREAM.MID с SGM на слух не
    // отличается, а стоит копии цикла рендера в SRAM и тактов на отсчёт; по
    // замеру зеркала сэмплов на 22050 Гц выше 10 кГц в тихих местах -83 дБFS
    // против -95.
    // Огибающие и затухание переводятся из миллисекунд ниже по стартовому темпу;
    // при смене темпа по ходу трека движок их пересчитывает.
    out.envelopes_in_real_time = true;
    out.default_speed = grid.ticks_per_row;
    out.default_tempo = static_cast<uint16_t>(tracker_tempo);
    out.default_global_volume = 128;
    // Панорама по корню (синус-косинус SF2-синтезаторов): центр 0.707 в каждую
    // сторону вместо 0.5, края те же; по восьми файлам с эталоном отклонение
    // RMS 1.00 дБ против 3.34 у линейной. Без квирка FT2: половины стереопары
    // банка стоят на -500/+500 и обязаны попасть в края целиком.
    out.pan_law = Song::PanLaw::Sqrt;
    // Общий уровень - как у эталонного синтезатора (голос x 0.6, без нормировки
    // и лимитера): 80/128 = 0.625. При 64 тише эталона на 2.0 дБ (SGM), 2.35
    // (GeneralUser без его +6 дБ при выпечке), 0.8 (Timbres of Heaven); десять
    // файлов, первые 60 с, RMS.
    out.sample_preamp = 80;
    out.filter_follows_note = true;
    // Шкала среза банка - 16 делений на октаву, отклик фильтра как в SF2.
    out.filter_units_per_octave = soundsinth::model::kFilterUnitsMid;
    out.filter_sf2_response = true;
    // Общий ревербератор нужен только здесь: у трекерных форматов посылов нет,
    // и движок не трогает ни шину, ни ревербератор. Включает его первый
    // ненулевой посыл, выписанный в ячейки: без CC91 > 0 (половина архива) шина
    // и линии нулевые, и рендер тот же.
    out.reverb_enabled = false;
    // Без лимитера и компрессора, как у эталона: уровень не ужимается. Перегруз
    // не обрезается полкой, а мягко насыщается: до 0.75 шкалы сигнал не тронут,
    // выше загибается к шкале. Громкий банк перегружает сумму, как и у эталона:
    // у Timbres of Heaven эталон уходит за шкалу на 11.5..13.7 дБ пика.
    out.limiter_enabled = false;
    out.soft_clip_enabled = true;
}

// Инструмент банка -> поля Instrument: громкость, фильтр, NNA, панорама,
// затухание из миллисекунд по частоте тиков трека. Огибающие и keymap -
// у вызывающего: у них своя память (арена трека, пул живого входа).
void instrument_from_bank(const bank::BankInstrument& bi, uint32_t ticks_per_second_rounded, Instrument& ins) {
    ins.global_volume = bi.global_volume;
    // Фильтр банка. Движок включает его по старшему биту (соглашение IT), банк
    // хранит 0..127 - бит ставится здесь. Срез банка - пик хода; огибающая
    // фильтра (из огибающей модуляции SF2 на срез) опускает его, поэтому с ней
    // фильтр включается и при срезе 127 - это верх шкалы, от которого она
    // отсчитывает. Без огибающей срез ниже 21 деления (около 330 Гц) глушит
    // инструмент - такой фильтр не включается; в GeneralUser GS 2.0.3 таких
    // инструментов 8 из 3807.
    constexpr uint8_t kMinUsableCutoff = 21;
    const bool filter_on =
        bi.env_filter != bank::kNoIndex || (bi.filter_cutoff < 127 && bi.filter_cutoff >= kMinUsableCutoff);
    if (filter_on) {
        ins.filter_cutoff = static_cast<uint8_t>(bi.filter_cutoff | 0x80u);
        ins.filter_resonance = static_cast<uint8_t>(bi.filter_resonance | 0x80u);
        // Яркость от силы удара (Instrument::velocity_to_cutoff) - только при
        // включённом фильтре: открытому срезу двигать нечего.
        ins.velocity_to_cutoff = bi.velocity_to_cutoff;
    } else {
        ins.filter_cutoff = 127;
        ins.filter_resonance = 0;
    }
    // NNA Off: голос, на канал которого легла новая нота, уходит в фоновый
    // пул и доигрывает релиз, а не обрывается (с Cut хор на смене аккорда
    // терял хвосты). Банк хранит Cut. Исключение - инструменты с
    // exclusiveClass: их обрыв конвертер пишет командой NoteCut в канал, до
    // хвоста в фоновом пуле она не дотянется.
    ins.nna = bi.exclusive_class != 0 ? static_cast<soundsinth::model::NewNoteAction>(bi.nna) : soundsinth::model::NewNoteAction::Off;
    ins.dct = static_cast<soundsinth::model::DuplicateCheckType>(bi.dct);
    ins.dca = static_cast<soundsinth::model::DuplicateCheckAction>(bi.dca);
    ins.instrument_panning = bi.instrument_panning;

    // Затухание: банк держит миллисекунды, частота тиков зависит от темпа
    // файла и приходит параметром.
    if (bi.fadeout_ms) {
        // Затухание по децибелам: за fadeout_ms пройти от 65536 до порога 256
        // (-48 дБ), множитель на тик - корень степени ticks из 256/65536. Число
        // тиков дробное, не округлять: на релизе в 3.2 тика округление до 3 гасит
        // хвост на 7% быстрее.
        const double ticks =
            static_cast<double>(bi.fadeout_ms) * static_cast<double>(ticks_per_second_rounded) / 1000.0;
        ins.fadeout_rate =
            ticks > 0.0 ? static_cast<uint32_t>(65536.0 * std::pow(256.0 / 65536.0, 1.0 / ticks) + 0.5) : 0u;
        if (ins.fadeout_rate >= 65536u) ins.fadeout_rate = 65535u;
    }
}

// Огибающая банка -> огибающая песни; миллисекунды - в тики.
void envelope_from_bank(const bank::BankEnvelope& be, uint32_t ticks_per_second_rounded, Envelope& env) {
    env.enabled = (be.flags & bank::kEnvEnabledBit) != 0;
    env.sustain_enabled = (be.flags & bank::kEnvSustainBit) != 0;
    env.loop_enabled = (be.flags & bank::kEnvLoopBit) != 0;
    env.carry = (be.flags & bank::kEnvCarryBit) != 0;
    // Точки, попавшие в один тик, сливаются: поздняя заменяет раннюю по
    // значению, тик остаётся общим. Атака короче тика выходит мгновенной, а
    // длиннее - ложится по времени с точностью до тика. Разводить такие точки
    // по соседним тикам нельзя: сдвиги копятся и растягивают атаку, а равные
    // тики движок не играет. Номера точек сустейна и петли - на новые места.
    uint8_t remap[soundsinth::model::kMaxEnvelopePoints] = {};
    uint8_t written = 0;
    for (uint8_t k = 0; k < be.point_count && k < soundsinth::model::kMaxEnvelopePoints; ++k) {
        // Миллисекунды в тики: банк от темпа файла не зависит.
        const uint32_t ms_ticks = (static_cast<uint32_t>(be.points[k].ms) * ticks_per_second_rounded + 500u) / 1000u;
        const uint16_t tick = static_cast<uint16_t>(ms_ticks > 65535u ? 65535u : ms_ticks);
        if (written && tick <= env.points[written - 1].tick) {
            env.points[written - 1].value = be.points[k].value;
            remap[k] = static_cast<uint8_t>(written - 1);
            continue;
        }
        env.points[written].tick = tick;
        env.points[written].value = be.points[k].value;
        remap[k] = written;
        ++written;
    }
    env.point_count = written;
    auto moved = [&](uint8_t point) {
        return point < be.point_count && point < soundsinth::model::kMaxEnvelopePoints ? remap[point] : point;
    };
    env.sustain_point = moved(be.sustain_point);
    env.sustain_end = moved(be.sustain_end);
    env.loop_start = moved(be.loop_start);
    env.loop_end = moved(be.loop_end);
}

// Запись сэмпла песни из сэмпла банка bs; PCM не трогается.
void sample_from_bank(const bank::Bank& bank, uint16_t bs, SampleDescriptor& d) {
    const bank::BankSample& src_s = bank.samples[bs];
    d.resident_encoding = static_cast<soundsinth::model::ResidentEncoding>(src_s.resident_encoding);
    d.channels = 1;
    d.length_samples = src_s.length_samples;
    d.loop_enabled = (src_s.flags & bank::kSampleLoopBit) != 0;
    d.loop_start = src_s.loop_start;
    d.loop_end = src_s.loop_end;
    d.c5_speed = src_s.c5_speed; // корневая нота и подстройка уже сложены препроцессором
    d.relative_note = 0;
    d.finetune = 0;
    d.default_volume = src_s.default_volume;
    d.global_volume = src_s.global_volume;
    d.default_panning = src_s.default_panning;
    // file_offset у .mid хранит индекс сэмпла банка: сэмплы приходят из банка,
    // а не из файла. По нему load_sample_from_bank находит, что распаковывать
    // при догрузке. Планировщик порядка сортирует по этому полю, чтобы хост не
    // перематывал файл; у .mid хост не участвует, порядок безразличен.
    d.file_offset = bs;
}

// Инструмент песни i из инструмента банка used_instruments[i]: фильтр,
// NNA, затухание, огибающие (общие с инструментами 0..i-1 - одной копией)
// и keymap с перенумерацией сэмплов. Инструменты 0..i-1 уже построены.
const char* build_instrument(Song& out, memory::TrackMemory& mem, const bank::Bank& bank,
                             const uint16_t* used_instruments, uint16_t i, const uint16_t* bank_to_song_sample,
                             uint32_t ticks_per_second_rounded, uint32_t& env_copies) {
    const bank::BankInstrument& bi = bank.instruments[used_instruments[i]];
    Instrument& ins = out.instruments[i];
    instrument_from_bank(bi, ticks_per_second_rounded, ins);

    // Огибающая банка -> огибающая песни, общая у всех инструментов с тем же
    // индексом (громкости и фильтра индексы из одной таблицы банка). Огибающие
    // в банке массово общие: в GeneralUser GS 2.0.3 огибающих громкости 785 на
    // 3807 инструментов, копия каждому - 106 байт резидентной арены, на плотных
    // файлах копии её переполняют. Уже скопированную ищем в построенных
    // инструментах: у прежних - в обоих полях, у этого - в огибающей
    // громкости (его фильтр ещё не заполнен). Порядок поиска - порядок
    // копирования, указатель тот же, что дала бы отдельная таблица.
    auto copy_envelope = [&](uint16_t bank_index) -> const Envelope* {
        for (uint16_t k = 0; k <= i; ++k) {
            const bank::BankInstrument& bk = bank.instruments[used_instruments[k]];
            const Instrument& built = out.instruments[k];
            if (bk.env_volume == bank_index && built.volume_envelope) return built.volume_envelope;
            if (k < i && bk.env_filter == bank_index && built.filter_envelope) return built.filter_envelope;
        }
        Envelope* env = memory::arena_new<Envelope>(mem.resident);
        if (!env) return nullptr;
        ++env_copies;
        envelope_from_bank(bank.envelopes[bank_index], ticks_per_second_rounded, *env);
        return env;
    };
    if (bi.env_volume != bank::kNoIndex) {
        ins.volume_envelope = copy_envelope(bi.env_volume);
        if (!ins.volume_envelope) return "резидентная память переполнена (огибающие)";
    }
    if (bi.env_filter != bank::kNoIndex) {
        ins.filter_envelope = copy_envelope(bi.env_filter);
        if (!ins.filter_envelope) return "резидентная память переполнена (огибающие)";
    }

    KeymapRange* km = memory::arena_new<KeymapRange>(mem.resident, bi.keymap_count);
    if (!km) return "резидентная память переполнена (keymap)";
    fill_keymap(bank, bi, bank_to_song_sample, km);
    ins.note_to_sample_ranges = km;
    ins.note_to_sample_range_count = static_cast<uint8_t>(bi.keymap_count > 255 ? 255 : bi.keymap_count);
    ins.default_sample_index = 0;
    return nullptr;
}

// Keymap копируется с перенумерацией сэмплов банка в сэмплы песни.
void fill_keymap(const bank::Bank& bank, const bank::BankInstrument& bi, const uint16_t* bank_to_song_sample,
                 KeymapRange* km) {
    for (uint16_t k = 0; k < bi.keymap_count; ++k) {
        const bank::BankKeymapRange& r = bank.keymap[bi.keymap_first + k];
        km[k].start_note = r.start_note;
        km[k].note_offset = static_cast<int8_t>(r.note_offset_s8);
        km[k].sample_index = (r.sample_index == bank::kNoSample || bank_to_song_sample[r.sample_index] == 0xffff)
                                 ? soundsinth::model::kNoSample
                                 : bank_to_song_sample[r.sample_index];
    }
}

// Инструменты и сэмплы песни из банка, все разом после разбора файла.
// nullptr - готово, иначе причина отказа; env_copies - огибающих в арене.
const char* build_instruments(Song& out, memory::TrackMemory& mem, const bank::Bank& bank,
                              const uint16_t* used_instruments, uint16_t used_instrument_count,
                              uint16_t used_sample_count, const uint16_t* bank_to_song_sample,
                              uint32_t ticks_per_second_rounded, uint32_t& env_copies) {
    out.channel_count = kMaxChannels;
    out.instrument_count = used_instrument_count;
    out.sample_count = used_sample_count;
    out.instruments = memory::arena_new<Instrument>(mem.resident, used_instrument_count);
    out.samples = memory::arena_new<SampleDescriptor>(mem.resident, used_sample_count);
    if (!out.instruments || !out.samples) return "резидентная память переполнена (инструменты/сэмплы)";

    for (uint32_t bs = 0; bs < bank.header->sample_count; ++bs) {
        const uint16_t si = bank_to_song_sample[bs];
        if (si == 0xffff) continue;
        sample_from_bank(bank, static_cast<uint16_t>(bs), out.samples[si]);
    }

    env_copies = 0;
    for (uint16_t i = 0; i < used_instrument_count; ++i) {
        if (const char* why = build_instrument(out, mem, bank, used_instruments, i, bank_to_song_sample,
                                               ticks_per_second_rounded, env_copies)) {
            return why;
        }
    }
    return nullptr;
}

} // namespace soundsinth::formats::midi
