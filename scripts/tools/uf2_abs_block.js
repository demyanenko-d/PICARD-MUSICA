// Дописать в начало образа UF2 абсолютный блок (обход ошибки RP2350-E10).
//
//   node uf2_abs_block.js <образ.uf2> <откуда-взять.uf2>
//
// Блок берётся байт в байт из второго файла: его туда кладёт picotool, и
// что в нём лежит, кроме адреса и длины, решает он же. Своя нумерация у
// блока тоже своя - семейства считаются независимо, поэтому остальные
// блоки образа не трогаются.
//
// picotool умеет ставить такой блок при склейке двух образов, но не при
// одиночном преобразовании: ключ принимает, сообщение печатает, блок в
// файл не кладёт.

const fs = require('fs');

const BLOCK = 512;
const MAGIC0 = 0x0a324655;
const MAGIC1 = 0x9e5d5157;
const MAGIC_END = 0x0ab16f30;
const FAMILY_ABSOLUTE = 0xe48bff57;

function blocks(buf, name) {
    if (buf.length === 0 || buf.length % BLOCK !== 0) {
        throw new Error(`${name}: длина ${buf.length} не кратна ${BLOCK}`);
    }
    return buf.length / BLOCK;
}

function familyOf(buf, i) {
    const o = i * BLOCK;
    if (buf.readUInt32LE(o) !== MAGIC0 || buf.readUInt32LE(o + 4) !== MAGIC1 ||
        buf.readUInt32LE(o + 508) !== MAGIC_END) {
        throw new Error(`блок ${i}: не UF2`);
    }
    return buf.readUInt32LE(o + 28);
}

function findAbs(buf, name) {
    const n = blocks(buf, name);
    for (let i = 0; i < n; ++i) {
        if (familyOf(buf, i) === FAMILY_ABSOLUTE) return i;
    }
    return -1;
}

function main() {
    const [target, donor] = process.argv.slice(2);
    if (!target || !donor) {
        console.error('uf2_abs_block: нужны два пути - образ и донор');
        process.exit(2);
    }

    const buf = fs.readFileSync(target);
    if (findAbs(buf, target) >= 0) {
        console.log(`uf2_abs_block: ${target} - абсолютный блок уже есть`);
        return;
    }

    const src = fs.readFileSync(donor);
    const at = findAbs(src, donor);
    if (at < 0) {
        console.error(`uf2_abs_block: в ${donor} нет абсолютного блока`);
        process.exit(1);
    }

    const abs = src.subarray(at * BLOCK, (at + 1) * BLOCK);
    fs.writeFileSync(target, Buffer.concat([abs, buf]));
    console.log(`uf2_abs_block: ${target} - блок дописан, стало ${blocks(fs.readFileSync(target), target)} блоков`);
}

main();
