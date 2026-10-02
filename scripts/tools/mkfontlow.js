// Нижняя половина знакогенератора отдельным файлом.
//
// Конфигуратор несёт шрифт в своём образе ПЗУ, а места там ровно
// килобайт: интерфейс английский, верхняя половина - кириллица и
// псевдографика - не нужна.
//
//   node mkfontlow.js <font.s> <font_low.s>
//
// Источник тот же, что у остального кадра, поэтому буквы везде одни.

const fs = require('fs');

const [, , src, dst] = process.argv;
if (!src || !dst) {
    console.error('usage: mkfontlow.js <font.s> <font_low.s>');
    process.exit(1);
}

const text = fs.readFileSync(src, 'utf8');
const bytes = [];
for (const line of text.split(/\r?\n/)) {
    const m = line.match(/^\s*\.db\s+(.+)$/);
    if (!m) continue;
    for (const part of m[1].split(',')) {
        const v = part.trim();
        if (v === '') continue;
        bytes.push(parseInt(v, v.startsWith('0x') ? 16 : 10) & 0xff);
    }
}

const kChars = 128;
const need = kChars * 8;
if (bytes.length < need) {
    console.error(`в ${src} только ${bytes.length} байт, нужно ${need}`);
    process.exit(1);
}

const out = [];
out.push(';; font_low.s - нижняя половина знакогенератора, 128 знаков.');
out.push(';;');
out.push(`;; Сгенерирован из ${src.replace(/\\/g, '/')}, руками не правится.`);
out.push(';;');
out.push(';; Килобайт в конце образа ПЗУ. Коды выше 127 этим шрифтом не');
out.push(';; рисуются: за ним начинается страница настроек.');
out.push('');
out.push('        .module font_low');
out.push('        .globl  _fw_font');
out.push('');
out.push('        .area   _FONT');
out.push('');
out.push('_fw_font::');
for (let i = 0; i < need; i += 8) {
    const row = bytes.slice(i, i + 8).map((b) => '0x' + b.toString(16).toUpperCase().padStart(2, '0'));
    out.push('        .db     ' + row.join(', '));
}
out.push('');

fs.writeFileSync(dst, out.join('\n'));
console.log(`${dst}: ${need} bytes, ${kChars} characters`);
