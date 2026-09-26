// mksna.js - снимок памяти .sna (128К) из плоского двоичного образа.
//
// Зачем он рядом с mkhobeta.js. Hobeta нужен настоящей машине и TR-DOS,
// а снимок - эмулятору: приложение поднимается сразу, без диска и без
// загрузчика. С эмулятором, умеющим Z-Controller и образ карты, так
// проверяются список файлов, сортировка и навигация - то есть всё, что
// не требует самой платы.
//
// Формат 128К (131103 байта):
//   27      заголовок регистров (тот же, что у 48К)
//   16384   страница 5   - она же экран, окно 0x4000..0x7FFF
//   16384   страница 2   - окно 0x8000..0xBFFF
//   16384   страница, стоящая в окне 0xC000 сейчас (по младшим битам порта)
//   2       PC
//   1       значение порта 0x7FFD
//   1       страничная ли ПЗУ TR-DOS (нам не нужна - 0)
//   5*16384 остальные страницы по возрастанию, кроме уже записанных
//
// У 128К-снимка PC лежит явно, поэтому подкладывать адрес возврата на
// стек, как в 48К, не требуется.

const fs = require('fs');

const [, , inPath, outPath, loadArg, entryArg, portArg] = process.argv;
if (!inPath || !outPath) {
    console.error('использование: mksna.js <вход.bin> <выход.sna> [адрес загрузки] [точка входа] [порт 7FFD]');
    process.exit(1);
}

const num = (s, def) => (s === undefined ? def : Number(s.startsWith('0x') ? s : s));
const loadAddr = num(loadArg, 0x6000);
const entry = num(entryArg, loadAddr);
const port = num(portArg, 0x10);

const bin = fs.readFileSync(inPath);
const end = loadAddr + bin.length;
if (loadAddr < 0x4000 || end > 0xC000) {
    // Выше 0xC000 класть нечего: там окно страниц, и что в нём лежит,
    // приложение решает само во время работы.
    console.error(`mksna: образ ${loadAddr.toString(16)}..${end.toString(16)} не укладывается в 0x4000..0xBFFF`);
    process.exit(1);
}

// Восемь страниц по 16К, все нулями. Экран (начало страницы 5) тоже
// нулевой: чёрный фон, а приложение всё равно чистит экран при старте.
const PAGE = 16384;
const banks = [];
for (let i = 0; i < 8; ++i) banks.push(Buffer.alloc(PAGE, 0));

// Разложить образ по страницам, как их видит процессор.
for (let i = 0; i < bin.length; ++i) {
    const addr = loadAddr + i;
    if (addr < 0x8000) banks[5][addr - 0x4000] = bin[i];
    else banks[2][addr - 0x8000] = bin[i];
}

const header = Buffer.alloc(27, 0);
header[0] = 0x3F;          // I - как после сброса
header[19] = 0x00;         // IFF2: прерывания запрещены, приложение включит их само
header[23] = 0x00;         // SP младший
header[24] = 0xBF;         // SP старший - 0xBF00, туда же его ставит crt0
header[25] = 0x01;         // IM 1
header[26] = 0x00;         // бордюр чёрный

const cur = port & 0x07;   // какая страница стоит в окне 0xC000
const tail = Buffer.alloc(4, 0);
tail.writeUInt16LE(entry & 0xFFFF, 0);
tail[2] = port & 0xFF;
tail[3] = 0;               // ПЗУ TR-DOS не подключено

const rest = [];
for (let i = 0; i < 8; ++i) {
    if (i === 5 || i === 2 || i === cur) continue;
    rest.push(banks[i]);
}

const out = Buffer.concat([header, banks[5], banks[2], banks[cur], tail, ...rest]);
fs.writeFileSync(outPath, out);

const kb = (n) => `${(n / 1024).toFixed(1)}К`;
console.log(
    `${outPath}: SNA 128К, ${out.length} байт; образ ${kb(bin.length)} по 0x${loadAddr.toString(16).toUpperCase()}, ` +
    `старт 0x${entry.toString(16).toUpperCase()}, порт 0x${(port & 0xFF).toString(16).toUpperCase().padStart(2, '0')}`);
