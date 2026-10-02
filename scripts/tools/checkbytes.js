// Проверить байты по адресам в двоичном образе.
//
//   node checkbytes.js <образ.bin> <адрес>=<байт,байт,...> ...
//
// Нужно там, где раскладка держится на счёте байт: векторы ПЗУ стоят по
// фиксированным адресам, и ошибка в выравнивании иначе заметна только на
// машине.

const fs = require('fs');

const [, , file, ...checks] = process.argv;
if (!file || checks.length === 0) {
    console.error('usage: checkbytes.js <image.bin> <addr>=<byte,byte,...> ...');
    process.exit(2);
}

const image = fs.readFileSync(file);
let bad = 0;
for (const check of checks) {
    const [addrText, bytesText] = check.split('=');
    const addr = parseInt(addrText, 16);
    const want = bytesText.split(',').map((b) => parseInt(b, 16));
    for (let i = 0; i < want.length; ++i) {
        const got = image[addr + i];
        if (got !== want[i]) {
            console.error(
                `${file}: at 0x${(addr + i).toString(16).padStart(4, '0')} expected 0x${want[i].toString(16).padStart(2, '0')}, got 0x${(got ?? 0).toString(16).padStart(2, '0')}`);
            ++bad;
        }
    }
}
if (bad !== 0) process.exit(1);
console.log(`${file}: ${checks.length} vector(s) in place`);
