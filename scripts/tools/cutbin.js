// cutbin.js - вырезать из 64-килобайтного образа makebin то, что реально
// занимает программа.
//
//   node scripts/tools/cutbin.js вход.full выход.bin 0x6000 [карта.map]
//
// makebin выдаёт всё адресное пространство целиком, а в файл на диске надо
// положить только код и данные. Где они кончаются - говорит карта
// компоновщика: берём наибольший конец областей от адреса загрузки.
//
// Без карты режем до последнего ненулевого байта - грубее, но работает.

const fs = require('fs');

const inFile = process.argv[2];
const outFile = process.argv[3];
const start = parseInt(process.argv[4] || '0x6000', 16);
const mapFile = process.argv[5];

if (!inFile || !outFile) {
    console.error('нужно: node scripts/tools/cutbin.js вход.full выход.bin 0x6000 [карта.map]');
    process.exit(2);
}

const full = fs.readFileSync(inFile);
let end = 0;

if (mapFile && fs.existsSync(mapFile)) {
    // Строки карты вида: _CODE   00006000    00001085 = 4229. bytes
    const lines = fs.readFileSync(mapFile, 'utf8').split(/\r?\n/);
    for (const l of lines) {
        const m = l.match(/^(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s+=/);
        if (!m) continue;
        const [, name, addrHex, sizeHex] = m;
        const addr = parseInt(addrHex, 16);
        const size = parseInt(sizeHex, 16);
        if (size === 0) continue;
        if (addr < start) continue;
        const e = addr + size;
        if (e > end) end = e;
    }
}

if (end === 0) {
    // Карты нет - ищем последний ненулевой байт.
    for (let i = full.length - 1; i >= start; --i) {
        if (full[i] !== 0) { end = i + 1; break; }
    }
}

if (end <= start) throw new Error('программа пуста: конец ' + end.toString(16));

fs.writeFileSync(outFile, full.subarray(start, end));
console.log(outFile + ': 0x' + start.toString(16) + '..0x' + end.toString(16) +
            ', ' + (end - start) + ' байт');
