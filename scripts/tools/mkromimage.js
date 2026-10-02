// Двоичный образ ПЗУ в заголовок для прошивки.
//
//   node mkromimage.js <образ.bin> <выход.h> <имя> <размер> <описание>
//
// Образ дополняется до точного размера: ПЗУ отвечает машине на весь свой
// диапазон, и хвост обязан быть определён, а не тем, что осталось в
// памяти.

const fs = require('fs');

const [, , src, dst, name, sizeArg, note, mapFile] = process.argv;
if (!src || !dst || !name || !sizeArg) {
    console.error('usage: mkromimage.js <rom.bin> <out.h> <name> <size> [note] [link.map]');
    process.exit(1);
}

const size = parseInt(sizeArg, 10);
let raw = fs.readFileSync(src);

// С картой компоновщика образ можно брать целиком: что лежит выше ПЗУ -
// это данные в ОЗУ машины, им в образе не место. Без карты образ обязан
// уложиться сам.
if (mapFile && fs.existsSync(mapFile)) {
    let over = 0;
    for (const line of fs.readFileSync(mapFile, 'utf8').split(/\r?\n/)) {
        const m = line.match(/^(_\S+)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+=/);
        if (!m) continue;
        const [, area, startText, lenText] = m;
        const start = parseInt(startText, 16);
        const len = parseInt(lenText, 16);
        if (len === 0 || start >= size) continue; // область целиком в ОЗУ машины
        if (start + len > size) {
            console.error(`${mapFile}: ${area} 0x${start.toString(16)}..0x${(start + len - 1).toString(16)} does not fit ${size}`);
            ++over;
        }
    }
    if (over !== 0) process.exit(1);
    raw = raw.subarray(0, Math.min(raw.length, size));
}

if (raw.length > size) {
    console.error(`${src}: ${raw.length} bytes, does not fit ${size}`);
    process.exit(1);
}

// Добивка - 0xFF: незанятый байт ПЗУ читается единицами, и так же видно,
// где кончился код.
const rom = Buffer.alloc(size, 0xff);
raw.copy(rom);

const lines = [];
lines.push('// SPDX-License-Identifier: MIT');
lines.push('#pragma once');
lines.push('');
lines.push('// Сгенерировано mkromimage.js, руками не править.');
if (note) lines.push(`// ${note}`);
lines.push('');
lines.push('#include <stdint.h>');
lines.push('');
lines.push(`inline constexpr uint32_t k${name}_bytes = ${size}u;`);
lines.push('');
lines.push('// Во флеше; inline constexpr - одно определение на все единицы трансляции.');
lines.push(`inline constexpr uint8_t k${name}[${size}] = {`);
const perLine = 26;
for (let i = 0; i < size; i += perLine) {
    const row = [];
    for (let j = i; j < Math.min(i + perLine, size); ++j) {
        row.push('0x' + rom[j].toString(16).padStart(2, '0'));
    }
    lines.push('    ' + row.join(', ') + ',');
}
lines.push('};');
lines.push('');

fs.writeFileSync(dst, lines.join('\n'));
console.log(`${dst}: ${raw.length} bytes of code in ${size}`);
