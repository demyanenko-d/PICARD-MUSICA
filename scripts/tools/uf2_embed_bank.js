// Вкладывает испечённый банк в образ прошивки .uf2.
//
//   node scripts/tools/uf2_embed_bank.js <прошивка.uf2> <банк.ssb> <выход.uf2>
//
// Зачем. Банк это отдельный регион флеша по SOUNDSINTH_BANK_FLASH_OFFSET,
// и заливать его отдельным действием - лишний шаг для того, кто просто
// ставит прошивку. Один файл, одна заливка.
//
// Формат UF2: последовательность блоков по 512 байт, каждый сам себя
// описывает - адрес назначения, размер полезной нагрузки, номер блока,
// общее число блоков и familyID. Загрузчик просто пишет каждый блок по
// его адресу, поэтому "слияние" это буквально дописать блоки банка и
// перенумеровать.
//
// Тонкость, из-за которой нельзя просто склеить два файла: blockNo и
// numBlocks у RP2350 считаются В ПРЕДЕЛАХ СЕМЕЙСТВА. Блоки банка идут с
// тем же familyID, что и прошивка, значит перенумеровать надо всю эту
// группу целиком. Блок семейства 0xe48bff57 (absolute, адрес у самого
// конца флеша) не трогается вовсе - он про другое.

const fs = require('fs');

const BLOCK = 512;
const PAYLOAD = 256;          // столько кладёт SDK; больше класть незачем и рискованно
const MAGIC0 = 0x0a324655;
const MAGIC1 = 0x9e5d5157;
const MAGIC_END = 0x0ab16f30;

// Должно совпадать с SOUNDSINTH_BANK_FLASH_OFFSET в
// backend/ports/rp2350/firmware_config.h.
const XIP_BASE = 0x10000000;
const BANK_OFFSET = 1 * 1024 * 1024;

function fail(msg) {
    console.error('uf2_embed_bank: ' + msg);
    process.exit(1);
}

function main() {
    const [fwPath, bankPath, outPath] = process.argv.slice(2);
    if (!fwPath || !bankPath || !outPath) {
        fail('нужно три аргумента: <прошивка.uf2> <банк.ssb> <выход.uf2>');
    }
    if (!fs.existsSync(fwPath)) fail('нет прошивки: ' + fwPath);
    if (!fs.existsSync(bankPath)) fail('нет банка: ' + bankPath);

    const fw = fs.readFileSync(fwPath);
    if (fw.length === 0 || fw.length % BLOCK !== 0) fail('прошивка не кратна 512 байтам, это не UF2');

    // Семейство прошивки - то, к которому относится блок по адресу XIP_BASE.
    let family = null;
    for (let i = 0; i < fw.length / BLOCK; i++) {
        const o = i * BLOCK;
        if (fw.readUInt32LE(o) !== MAGIC0 || fw.readUInt32LE(o + 4) !== MAGIC1) fail('битый блок ' + i);
        if (fw.readUInt32LE(o + 12) === XIP_BASE) family = fw.readUInt32LE(o + 28);
    }
    if (family === null) fail('в прошивке нет блока по адресу начала флеша - не понимаю, куда класть банк');

    // Идемпотентность: выход может совпадать со входом, а образ уже
    // может нести банк с прошлого прогона. Выбрасываем всё, что лежит в
    // регионе банка, и кладём заново - иначе банк вложился бы дважды.
    const BANK_LO = XIP_BASE + BANK_OFFSET;
    const keep = [];
    let dropped = 0;
    for (let i = 0; i < fw.length / BLOCK; i++) {
        const o = i * BLOCK;
        if (fw.readUInt32LE(o + 12) >= BANK_LO && fw.readUInt32LE(o + 28) === family) { dropped++; continue; }
        keep.push(fw.subarray(o, o + BLOCK));
    }
    if (dropped) console.log('в образе уже был банк (' + dropped + ' блоков) - заменяю');
    const base = Buffer.concat(keep);

    const bank = fs.readFileSync(bankPath);
    const bankBlocks = Math.ceil(bank.length / PAYLOAD);

    // Сколько блоков этого семейства станет всего - это число пойдёт в
    // numBlocks КАЖДОГО блока семейства, и старых тоже.
    let fwFamilyBlocks = 0;
    for (let i = 0; i < base.length / BLOCK; i++) {
        if (base.readUInt32LE(i * BLOCK + 28) === family) fwFamilyBlocks++;
    }
    const totalFamily = fwFamilyBlocks + bankBlocks;

    const out = Buffer.alloc(base.length + bankBlocks * BLOCK);
    base.copy(out, 0);

    // Перенумеровать старые блоки семейства.
    let no = 0;
    for (let i = 0; i < base.length / BLOCK; i++) {
        const o = i * BLOCK;
        if (out.readUInt32LE(o + 28) !== family) continue;
        out.writeUInt32LE(no++, o + 20);
        out.writeUInt32LE(totalFamily, o + 24);
    }

    // Блоки банка.
    const flags = 0x00002000; // familyID присутствует
    let w = base.length;
    for (let i = 0; i < bankBlocks; i++, w += BLOCK) {
        const off = i * PAYLOAD;
        const n = Math.min(PAYLOAD, bank.length - off);
        out.writeUInt32LE(MAGIC0, w);
        out.writeUInt32LE(MAGIC1, w + 4);
        out.writeUInt32LE(flags, w + 8);
        out.writeUInt32LE(XIP_BASE + BANK_OFFSET + off, w + 12);
        out.writeUInt32LE(n, w + 16);
        out.writeUInt32LE(no++, w + 20);
        out.writeUInt32LE(totalFamily, w + 24);
        out.writeUInt32LE(family, w + 28);
        bank.copy(out, w + 32, off, off + n);
        // Хвост последнего блока остаётся нулями: payloadSize говорит
        // загрузчику, сколько байт реально писать.
        out.writeUInt32LE(MAGIC_END, w + BLOCK - 4);
    }

    fs.writeFileSync(outPath, out);

    const mb = x => (x / (1024 * 1024)).toFixed(2);
    console.log('банк вложен в образ: прошивка ' + (base.length / BLOCK) + ' блоков + банк ' + bankBlocks +
                ' = ' + (out.length / BLOCK) + ' блоков, ' + mb(out.length) + ' МБ' +
                ' (банк ' + mb(bank.length) + ' МБ по 0x' + (XIP_BASE + BANK_OFFSET).toString(16) + ')');

    // Прошивка обязана оставаться НИЖЕ банка, иначе они затрут друг друга.
    let maxFw = 0;
    for (let i = 0; i < base.length / BLOCK; i++) {
        const o = i * BLOCK;
        if (base.readUInt32LE(o + 28) !== family) continue;
        if (base.readUInt32LE(o + 12) >= XIP_BASE + 0x00F00000) continue; // блок absolute у конца флеша - не про прошивку
        const end = base.readUInt32LE(o + 12) + base.readUInt32LE(o + 16);
        if (end > maxFw) maxFw = end;
    }
    if (maxFw > XIP_BASE + BANK_OFFSET) {
        fail('прошивка доросла до 0x' + maxFw.toString(16) + ' и залезла в регион банка - ' +
             'поднимайте SOUNDSINTH_BANK_FLASH_OFFSET');
    }
    console.log('  прошивка занимает до 0x' + maxFw.toString(16) + ', банк начинается с 0x' +
                (XIP_BASE + BANK_OFFSET).toString(16) + ' - запас ' +
                mb(XIP_BASE + BANK_OFFSET - maxFw) + ' МБ');
}

main();
