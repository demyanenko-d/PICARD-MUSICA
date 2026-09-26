#include "pc/fatfs_byte_source.h"

#ifdef _WIN32
// ff.h на MSVC подключает <windows.h> внутри своего extern "C" { ... },
// что ломает C++-перегрузки из <windows.h>, если он не был подключён
// раньше. Поэтому подключается заранее, вне extern "C".
#include <windows.h>
#endif
#include "ff.h"

namespace soundsinth::formats {

FatFsByteSource::FatFsByteSource() : file_(new FIL()) {}

FatFsByteSource::~FatFsByteSource() {
    close();
    delete static_cast<FIL*>(file_);
}

bool FatFsByteSource::open(const char* path) {
    close();
    return f_open(static_cast<FIL*>(file_), path, FA_READ) == FR_OK;
}

void FatFsByteSource::close() {
    auto* fil = static_cast<FIL*>(file_);
    if (fil->obj.fs != nullptr) { // открыт (тот же признак проверяет f_close)
        f_close(fil);
    }
}

bool FatFsByteSource::is_open() const {
    return static_cast<const FIL*>(file_)->obj.fs != nullptr;
}

ByteSource FatFsByteSource::as_byte_source() {
    return ByteSource{this, &read_fn, &seek_fn, &size_fn};
}

uint32_t FatFsByteSource::read_fn(void* self, void* dst, uint32_t n) {
    auto* src = static_cast<FatFsByteSource*>(self);
    UINT read = 0;
    const FRESULT res = f_read(static_cast<FIL*>(src->file_), dst, n, &read);
    return res == FR_OK ? static_cast<uint32_t>(read) : 0;
}

bool FatFsByteSource::seek_fn(void* self, uint32_t offset) {
    auto* src = static_cast<FatFsByteSource*>(self);
    return f_lseek(static_cast<FIL*>(src->file_), offset) == FR_OK;
}

uint32_t FatFsByteSource::size_fn(void* self) {
    auto* src = static_cast<FatFsByteSource*>(self);
    return static_cast<uint32_t>(f_size(static_cast<FIL*>(src->file_)));
}

} // namespace soundsinth::formats
