// mkhobeta.js - завернуть плоский бинарник в файл Hobeta для TR-DOS.
//
//   node scripts/tools/mkhobeta.js вход.bin выход.$C ИМЯ 0x6000
//
// Hobeta - это файл TR-DOS плюс 17-байтная шапка, в которой лежит то, что
// на диске хранится в каталоге: имя, тип, адрес загрузки, длина.
//
//   [0..7]   имя, дополненное пробелами
//   [8]      тип: 'B' бейсик, 'C' код, 'D' массив
//   [9..10]  адрес загрузки, младший байт вперёд
//   [11..12] длина в байтах
//   [13]     ноль
//   [14]     секторов: длина, округлённая вверх до 256
//   [15..16] сумма: Σ(байт[i]*257 + i) по i = 0..14
//
// Данные дополняются нулями до целого числа секторов: TR-DOS оперирует
// секторами, и хвост всё равно будет прочитан.

const fs = require('fs');

const inFile = process.argv[2];
const outFile = process.argv[3];
const name = (process.argv[4] || 'PLAYER').toUpperCase();
const start = parseInt(process.argv[5] || '0x6000', 16);

if (!inFile || !outFile) {
    console.error('нужно: node scripts/tools/mkhobeta.js вход.bin выход.$C ИМЯ 0x6000');
    process.exit(2);
}

const data = fs.readFileSync(inFile);
if (data.length === 0) throw new Error(inFile + ' пуст');
if (data.length > 0xFFFF) throw new Error('файл длиннее 64 КБ: ' + data.length);

const secs = Math.ceil(data.length / 256);
if (secs > 255) throw new Error('секторов ' + secs + ', в шапку помещается 255');

const hdr = Buffer.alloc(17, 0x20);
hdr.write(name.slice(0, 8).padEnd(8), 0, 'latin1');
hdr[8] = 'C'.charCodeAt(0);          // код, а не бейсик
hdr.writeUInt16LE(start, 9);
hdr.writeUInt16LE(data.length, 11);
hdr[13] = 0;
hdr[14] = secs;

let sum = 0;
for (let i = 0; i <= 14; ++i) sum = (sum + hdr[i] * 257 + i) & 0xFFFF;
hdr.writeUInt16LE(sum, 15);

const body = Buffer.alloc(secs * 256);
data.copy(body);

fs.writeFileSync(outFile, Buffer.concat([hdr, body]));
console.log(outFile + ': ' + name + ', загрузка на 0x' + start.toString(16) +
            ', ' + data.length + ' байт (' + secs + ' секторов)');
