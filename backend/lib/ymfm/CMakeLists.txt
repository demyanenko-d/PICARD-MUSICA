# ymfm (Aaron Giles, BSD-3, LICENSE рядом): эмуляция FM-чипов Yamaha.
# Взята из github.com/aaronsgiles/ymfm, 81aec25 (2026-07-27), только файлы
# семейств OPL, OPN и OPM и то, что они включают. Прошивка собирает OPL;
# OPN (ymfm_opn, ymfm_ssg) собирается ради движка OPNA: на нём FM-часть
# YM2203 живого TurboSound FM. OPM (ymfm_opm) - YM2151 плеера VGM.
#
# Правки, все помечены SoundSinth:
# - ymfm.h: деструкторы ymfm_interface и ymfm_engine_callbacks не виртуальные:
#   виртуальный даёт удаляющий деструктор, а тот тянет в образ кучу;
# - ymfm_fm.h: MAX_BATCH 64 вместо 128 - массивы батча лежат на стеке рендера;
# - ymfm_opl.cpp, ymfm_opn.cpp, ymfm_opm.cpp: log_keyon под YMFM_NO_KEYON_LOG;
# - ymfm_fm.h/.ipp: каналы и операторы движка лежат внутри объекта (inplace)
#   вместо std::unique_ptr - кучи у прошивки нет; шаблон inplace - в ymfm.h;
# - ymfm_adpcm.h/.cpp: каналы движков ADPCM-A и ADPCM-B - так же inplace (на
#   них ADPCM чипов Y8950 и YM2610 в плеере VGM);
# - ymfm_opl.cpp: явно построен движок OPL3 - его зовут напрямую, без ymf262;
# - ymfm_fm.h/.ipp: пакетный рендер семейств OPL, OPLL, OPN, OPM (generate_channels): канал
#   целиком на батч, сразу на частоте выхода, моно на канал; конец
#   ymfm_fm.ipp - выбор ядра и подготовка операторов, в классах - объявления,
#   friend и поля батча; шум OPM и стадия depress OPLL - канал по
#   отсчётам чипа, ритм OPLL с ударом - весь батч; SSG-EG на частоте чипа
#   идёт пакетным путём (такт SSG и огибающая по отсчёту чипа, перезапуски
#   фазы - маской на кусок), вне её и у неактивного канала - по отсчётам чипа; фаза хай-хэта и тарелки -
#   операторы каналов 7 и 8 (у ymfm - номера 13 и 17, у OPLL это канал 6);
#   канал, где все операторы отпущены до полного затухания (dead), на
#   частоте чипа не считается: до взятия ноты у него идут только фазы;
#   generate_channels возвращает каналы, в которых может быть звук;
#   idle() движка - все операторы затухли и клавиши отпущены: живой чип в
#   таком состоянии не считают вовсе; m_prepared - до первого prepare() кэш
#   операторов не заполнен, рендер отдаёт нули (частота чипа ниже выхода);
# - ymfm_batch.h/.cpp, ymfm_batch_hot.cpp (свои файлы): общая для всех
#   семейств часть пакетного рендера - таблицы, огибающая, ядра каналов 2op,
#   4op, бочка, ритм 7+8; ymfm_batch.cpp - по кускам (флеш), ymfm_batch_hot.cpp -
#   по отсчётам (прошивка держит его в SRAM). Ядра 2op и 4op идут по
#   операторам: оператор считает кусок до 32 отсчётов в свою строку,
#   следующий читает строку модуляции; огибающая шагает по событиям;
# - ymfm_opl.h/.cpp, ymfm_opn.h/.cpp, ymfm_opm.h/.cpp: формы волны (и три
#   формы LFO OPM) - из общей fm_tables (set_tables) вместо копии на чип;
# - ymfm_opn.h: ym2203_on_opna - YM2203 на движке OPNA;
# - ymfm_opl.h, ymfm_opn.h, ymfm_opm.h: состояние LFO тремоло батча
#   (lfo_am_raw, set_lfo_am_raw, lfo_am_from_raw), у OPM и шума (set_noise_state);
# - ymfm_opl.cpp, ymfm_opn.cpp, ymfm_opm.cpp: явные экземпляры движков OPLL,
#   OPN, OPNA, OPM;
# - ymfm_opl.h: таблица тембров ds1001 (VRC7) открыта - её берёт плеер VGM.
# - ymfm_fm.h/.ipp, ymfm_adpcm.h: счёт слышимых голосов для телеметрии
#   (heard_channels - несущий оператор громче порога; playing у ADPCM-A).
# Остальное - как в верховьях, чтобы обновление оставалось сравнением файлов.
add_library(ymfm STATIC
    ymfm_opl.cpp
    ymfm_batch.cpp
    ymfm_batch_hot.cpp
    ymfm_opn.cpp
    ymfm_opm.cpp
    ymfm_ssg.cpp
    ymfm_adpcm.cpp
    ymfm_pcm.cpp
)
target_include_directories(ymfm SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ymfm PUBLIC cxx_std_17)
# Журнал нажатий (log_keyon) возвращает std::string: в прошивке он тянет кучу.
target_compile_definitions(ymfm PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:YMFM_NO_KEYON_LOG=1>)
target_compile_options(ymfm PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/GR- /W0>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-exceptions;-fno-unwind-tables;-fno-rtti;-ffunction-sections;-fdata-sections;-w>
)
