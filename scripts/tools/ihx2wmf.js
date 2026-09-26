/**
 * ihx2wmf.js - Конвертер SDCC .ihx (Intel HEX) + заголовок WC -> .wmf
 *
 * Плагин запускается по SC_MENU_EMENU - из меню F10 и из меню выбора
 * вьювера.
 *
 * Использование:
 *   node scripts/tools/ihx2wmf.js <input.ihx> <output.wmf>
 */

'use strict';

const fs = require('fs');

const HEADER_SIZE = 512;
const PLUGIN_ORG  = 0x8000;
const PLUGIN_NAME = 'SoundSinth Player v0.1';   /* дополняется пробелами до 32 */
const EXTENSIONS  = ['XM', 'MOD', 'S3M', 'IT', 'MID'];
const SC_MENU_EMENU = 0x05;                     /* как в wc_api.h */
const MENU_TEXT = 'Play tracker module';        /* +204, до 24 байт */

function parseIHX(src) {
    const buf = Buffer.alloc(0x10000, 0x00);
    let minA = 0x10000;
    let maxA = 0;

    for (const rawLine of src.split('\n')) {
        const line = rawLine.trim();
        if (!line.startsWith(':')) continue;

        const byteCount  = parseInt(line.slice(1, 3), 16);
        const address    = parseInt(line.slice(3, 7), 16);
        const recordType = parseInt(line.slice(7, 9), 16);

        if (recordType === 0x00) {
            for (let i = 0; i < byteCount; i++) {
                const b = parseInt(line.slice(9 + i * 2, 11 + i * 2), 16);
                const a = address + i;
                buf[a] = b;
                if (a < minA) minA = a;
                if (a > maxA) maxA = a;
            }
        }
    }

    return { buf, minAddr: minA, maxAddr: maxA };
}

function buildHeader(totalPages, blockEntries) {
    const hdr = Buffer.alloc(HEADER_SIZE, 0x00);

    Buffer.from('WildCommanderMDL').copy(hdr, 16);
    // Версия формата заголовка: WC отвергает плагин, если байт +32 вне
    // диапазона PLGOV(#02)..PLGCV(#10). 0x10 на
    // живом железе даёт "wrong version"; 0x0A - версия TRDisp.wmf,
    // совместимая с более широким диапазоном прошивок WC.
    hdr[32] = 0x0A;
    hdr[34] = totalPages;
    hdr[35] = 0;

    for (let i = 0; i < Math.min(blockEntries.length, 6); i++) {
        hdr[36 + i * 2]     = blockEntries[i].page;
        hdr[36 + i * 2 + 1] = blockEntries[i].blocks;
    }

    hdr[63] = 0;

    // Каждая запись - 3 байта, короче 3 символов - дополняется ПРОБЕЛОМ
    // (0x20), не нулём: WC оперирует FAT32-подобными записями каталога
    // (запись TENTRY - "структура как в каталоге FAT32"), где
    // 3-байтное расширение всегда space-padded. "XM"/"IT" короче "MOD"/
    // "S3M" - единственные записи, которых это реально касается.
    let extOff = 64;
    for (const ext of EXTENSIONS) {
        for (let i = 0; i < 3; i++) hdr[extOff + i] = i < ext.length ? ext.charCodeAt(i) : 0x20;
        extOff += 3;
    }
    hdr[160] = 0;

    hdr[161] = 0xFF; hdr[162] = 0xFF; hdr[163] = 0xFF; hdr[164] = 0xFF;

    Buffer.from(PLUGIN_NAME.slice(0, 32).padEnd(32, ' '), 'ascii').copy(hdr, 165);

    hdr[197] = SC_MENU_EMENU;
    Buffer.from(MENU_TEXT.slice(0, 24), 'ascii').copy(hdr, 204);

    return hdr;
}

function main() {
    const args = process.argv.slice(2);
    const inFile = args[0];
    const outFile = args[1];

    if (!inFile || !outFile) {
        console.error('Usage: node ihx2wmf.js <input.ihx> <output.wmf>');
        process.exit(1);
    }

    const src = fs.readFileSync(inFile, 'utf8');
    const { buf, minAddr, maxAddr } = parseIHX(src);

    if (minAddr > maxAddr) {
        console.error('ERROR: empty IHX');
        process.exit(1);
    }
    if (minAddr < PLUGIN_ORG) {
        console.error(`ERROR: code starts at 0x${minAddr.toString(16)}, expected >= 0x${PLUGIN_ORG.toString(16)}`);
        process.exit(1);
    }

    const codeSize = maxAddr - PLUGIN_ORG + 1;
    const codePages = Math.max(1, Math.ceil(codeSize / 16384));

    console.log(`Code: 0x${PLUGIN_ORG.toString(16)} - 0x${maxAddr.toString(16)} (${codeSize} bytes)`);
    console.log(`Code pages: ${codePages}`);

    const blockEntries = [];
    for (let p = 0; p < codePages; p++) {
        if (p < codePages - 1) {
            blockEntries.push({ page: p, blocks: 32 });
        } else {
            const pageStart = p * 16384;
            const remaining = codeSize - pageStart;
            blockEntries.push({ page: p, blocks: Math.ceil(remaining / 512) });
        }
    }

    if (blockEntries.length > 6) {
        console.error(`ERROR: too many block entries (${blockEntries.length}, max 6)`);
        process.exit(1);
    }

    const totalPages = blockEntries.reduce((sum, e) => Math.max(sum, e.page + 1), 0);
    const hdr = buildHeader(totalPages, blockEntries);
    const codeSlice = buf.slice(PLUGIN_ORG, PLUGIN_ORG + codeSize);

    const out = Buffer.concat([hdr, codeSlice]);
    fs.writeFileSync(outFile, out);

    console.log(`Pages: ${totalPages}, Block entries: ${blockEntries.length}`);
    for (let i = 0; i < blockEntries.length; i++) {
        console.log(`  block[${i}]: page=${blockEntries[i].page}, sectors=${blockEntries[i].blocks}`);
    }
    console.log(`Written: ${outFile} (${out.length} bytes)`);
}

main();
