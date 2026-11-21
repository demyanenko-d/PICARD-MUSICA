// Словарь каталогов виртуального диска zxart для прошивки.
//
// Читает выгрузку сайта (страницы author_*, prod_*, release_* по 1000 -
// zxart_fetch.js) и дерево категорий, пишет backend/net/zxart/
// zxart_dict.inc: префиксы делимых списков, жанры разделов, годы, в
// которых у раздела есть программы.
//
// Деление списка (названия программ или имена авторов): до kLimit - не
// делится; больше - буквы, буква больше kLimit - пары, пара больше kLimit
// - тройки. Каталог - один фильтр сайта (префикс), поэтому группа, которой
// нужно несколько префиксов (знаки, сведённые в "_"), не создаётся. Сайт
// сравнивает без регистра и надстрочных знаков.
//
// node scripts/tools/zxart_dict.js <каталог выгрузки> <cats.json> <out.inc> <дата YYYYMMDD>
'use strict';
const fs = require('fs');
const path = require('path');

const kLimit = 200;
const [rawDir, catsFile, outFile, built] = process.argv.slice(2);
if (!built) {
    console.error('usage: zxart_dict.js <raw dir> <cats.json> <out.inc> <YYYYMMDD>');
    process.exit(2);
}

const pages = prefix => fs.readdirSync(rawDir).filter(f => f.startsWith(prefix + '_')).sort()
    .map(f => JSON.parse(fs.readFileSync(path.join(rawDir, f), 'utf8')));
const norm = s => (s || '').trim().normalize('NFD').replace(/[̀-ͯ]/g, '').toUpperCase();
const alnum = c => /[\p{L}\p{N}]/u.test(c);
const fold = c => (alnum(c) ? c : '_');

// Разделы - в порядке kSections прошивки.
const kSections = [92177, 204819, 92183, 244880, 92534, 244858, 202588, 92188];
const kRootSection = 15;

const cats = JSON.parse(fs.readFileSync(catsFile, 'utf8')).responseData.zxProdCategory;
const byId = new Map(cats.map(c => [c.id, c]));
const owner = new Map(); // категория -> {sec, genre}
kSections.forEach((sid, sec) => {
    owner.set(sid, { sec, genre: 0 });
    for (const g of byId.get(sid).categories || []) {
        const walk = id => {
            owner.set(id, { sec, genre: g });
            for (const k of (byId.get(id) || {}).categories || []) walk(k);
        };
        walk(g);
    }
});

const authors = new Map();
for (const p of pages('author')) for (const a of p.responseData.author) authors.set(a.id, a.title || '');
const withImage = new Set();
for (const p of pages('release')) for (const r of p.responseData.zxRelease) withImage.add(r.prodId);
const prods = new Map();
for (const p of pages('prod')) for (const x of p.responseData.zxProd) prods.set(x.id, x);

// Поле года ключа: 0 - нет, 1 - не указан (zxProdYear=0), иначе год - 1978.
const yearField = y => (y ? y - 1978 : 1);

// Ключ списка: 1 разряд (авторы), раздел 4, год 7, жанр 20.
const listKey = (isAuthors, sec, year, genre) => ((isAuthors ? 1 : 0) << 31 | sec << 27 | year << 20 | genre) >>> 0;

let dropped = 0;
function split(keys) {
    if (keys.length <= kLimit) return null;
    const groups = (list, depth) => {
        const m = new Map();
        for (const k of list) {
            if ([...k].length < depth) {
                ++dropped;
                continue;
            }
            const chars = [...k].slice(0, depth);
            const p = chars.map(fold).join('');
            if (!m.has(p)) m.set(p, { keys: [], real: new Set() });
            m.get(p).keys.push(k);
            m.get(p).real.add(chars.join(''));
        }
        return [...m].sort((a, b) => (a[0] < b[0] ? -1 : 1));
    };
    const out = [];
    const add = e => {
        if (e.real.size > 1) {
            dropped += e.keys.length;
            return;
        }
        out.push([...e.real][0]);
    };
    for (const [, e1] of groups(keys, 1)) {
        if (e1.keys.length <= kLimit || e1.real.size > 1) { add(e1); continue; }
        for (const [, e2] of groups(e1.keys, 2)) {
            if (e2.keys.length <= kLimit || e2.real.size > 1) { add(e2); continue; }
            for (const [, e3] of groups(e2.keys, 3)) add(e3);
        }
    }
    return out;
}

const lists = [];
const put = (key, keys) => {
    const d = split(keys);
    if (d) lists.push({ key, prefixes: d });
};
const titles = ps => ps.map(p => norm(p.title));
const authorNames = ps => {
    const ids = new Set();
    for (const p of ps) for (const a of p.authorsInfo || []) if (authors.has(a.authorId)) ids.add(a.authorId);
    return [...ids].map(id => norm(authors.get(id)));
};
const byYear = ps => {
    const m = new Map();
    for (const p of ps) {
        const y = yearField(p.year || 0);
        if (!m.has(y)) m.set(y, []);
        m.get(y).push(p);
    }
    return m;
};

const genres = [];
const yearMasks = [];
const mask = m => {
    let v = 0n;
    for (const y of m.keys()) if (y < 64) v |= 1n << BigInt(y);
    return v;
};
kSections.forEach((sid, sec) => {
    // Программы с образом - для названий; все программы раздела - для
    // авторов: фильтр авторов сайта образы не учитывает.
    const inSec = p => (p.connectedCategoriesIds || []).some(c => (owner.get(c) || {}).sec === sec);
    const all = [...prods.values()].filter(inSec);
    const img = all.filter(p => withImage.has(p.id));
    put(listKey(false, sec, 0, 0), titles(img));
    put(listKey(true, sec, 0, 0), authorNames(all));
    const yi = byYear(img), ya = byYear(all);
    yearMasks.push({ key: listKey(false, sec, 0, 0), mask: mask(yi) });
    for (const [y, ps] of yi) put(listKey(false, sec, y, 0), titles(ps));
    for (const [y, ps] of ya) put(listKey(true, sec, y, 0), authorNames(ps));
    const gmap = new Map();
    for (const p of all) for (const c of p.connectedCategoriesIds || []) {
        const o = owner.get(c);
        if (!o || o.sec !== sec || !o.genre) continue;
        if (!gmap.has(o.genre)) gmap.set(o.genre, new Set());
        gmap.get(o.genre).add(p);
    }
    for (const [g, set] of [...gmap].sort((a, b) => byId.get(a[0]).title.localeCompare(byId.get(b[0]).title))) {
        const gall = [...set], gimg = gall.filter(p => withImage.has(p.id));
        if (gimg.length === 0) continue;
        genres.push({ sec, id: g, name: byId.get(g).title });
        put(listKey(false, sec, 0, g), titles(gimg));
        put(listKey(true, sec, 0, g), authorNames(gall));
        const gy = byYear(gimg);
        yearMasks.push({ key: listKey(false, sec, 0, g), mask: mask(gy) });
        for (const [y, ps] of gy) put(listKey(false, sec, y, g), titles(ps));
    }
});
put(listKey(true, kRootSection, 0, 0), [...authors.values()].map(norm));

lists.sort((a, b) => a.key - b.key);
yearMasks.sort((a, b) => a.key - b.key);
// Строка C: не ASCII - восьмеричным escape из трёх цифр, он не съедает
// следующий знак.
const cstr = s => {
    let o = '"';
    for (const b of Buffer.from(s, 'utf8')) o += b >= 0x20 && b < 0x7f && b !== 0x22 && b !== 0x5c && b !== 0x3f ? String.fromCharCode(b) : '\\' + b.toString(8).padStart(3, '0');
    return o + '"';
};
let prefixCount = 0;
let maxBytes = 0;
const prefixLines = [];
const listLines = [];
for (const l of lists) {
    listLines.push(`    {0x${l.key.toString(16).padStart(8, '0')}u, ${prefixCount}, ${l.prefixes.length}},`);
    for (const p of l.prefixes) {
        maxBytes = Math.max(maxBytes, Buffer.byteLength(p, 'utf8'));
        prefixLines.push(`    ${cstr(p)},`);
    }
    prefixCount += l.prefixes.length;
}
if (maxBytes > 7) throw new Error('prefix longer than 7 bytes');
const out = [
    '// SPDX-License-Identifier: MIT',
    `// Generated by scripts/tools/zxart_dict.js from the ZX-Art export of ${built}. Do not edit.`,
    '// clang-format off',
    `inline constexpr uint32_t kDictBuilt = ${built}u;`,
    '',
    `inline constexpr DictList kDictLists[${lists.length}] = {`,
    ...listLines,
    '};',
    '',
    `inline constexpr char kDictPrefixes[${prefixCount}][8] = {`,
    ...prefixLines,
    '};',
    '',
    `inline constexpr Genre kGenres[${genres.length}] = {`,
    ...genres.map(g => `    {${g.sec}, ${g.id}u, ${cstr(g.name)}},`),
    '};',
    '',
    `inline constexpr CategorySection kCategorySections[${owner.size}] = {`,
    ...[...owner].sort((a, b) => a[0] - b[0]).map(([id, o]) => `    {${id}u, ${o.sec}},`),
    '};',
    '',
    `inline constexpr YearMask kYearMasks[${yearMasks.length}] = {`,
    ...yearMasks.map(y => `    {0x${y.key.toString(16).padStart(8, '0')}u, 0x${y.mask.toString(16)}ull},`),
    '};',
    '// clang-format on',
    '',
];
fs.writeFileSync(outFile, out.join('\n'));
console.log(`lists ${lists.length}, prefixes ${prefixCount}, genres ${genres.length}, year masks ${yearMasks.length}, dropped ${dropped}`);
