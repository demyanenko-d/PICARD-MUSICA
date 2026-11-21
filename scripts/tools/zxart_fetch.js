// Выгрузка zxart.ee для словаря каталогов (zxart_dict.js): авторы,
// программы, релизы-образы, музыка, картинки страницами по 1000 в каталог,
// который передан первым аргументом. Скачанные страницы не
// перекачиваются. Пауза 2 с между запросами, User-Agent обязателен.
//
// node scripts/tools/zxart_fetch.js <каталог> [base|media]
// Страница, на которой сайт отвечает 500, дробится до одной записи; битая
// запись пропускается и пишется в skipped.txt каталога.
const fs = require('fs');
const path = require('path');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const dir = process.argv[2];
if (!dir) {
    console.error('usage: zxart_fetch.js <dir> [base|media]');
    process.exit(2);
}
fs.mkdirSync(dir, { recursive: true });
const kImages = 'trd,scl,tap,tzx,z80,sna,dsk,mgt,img,mdr,rom,nex,szx';
const kLimit = 1000;

// null - сайт не отдал страницу (500 или обрезанный ответ).
async function get(url) {
    for (let attempt = 1; attempt <= 2; ++attempt) {
        try {
            const r = await fetch(url, { headers: { 'User-Agent': 'SoundSinth/tools' } });
            const text = await r.text();
            const j = JSON.parse(text);
            if (j.responseStatus !== 'success') throw new Error('status ' + j.responseStatus);
            return { j, text };
        } catch (e) {
            console.log(`  attempt ${attempt} failed: ${e.message}`);
            await sleep(5000);
        }
    }
    return null;
}

const name_of = (name, start, limit) => path.join(dir, `${name}_${String(start).padStart(6, '0')}_${String(limit).padStart(4, '0')}.json`);

// Отрезок [start, start+limit): целиком или по частям.
async function piece(name, head, start, limit) {
    const file = name_of(name, start, limit);
    if (fs.existsSync(file)) return JSON.parse(fs.readFileSync(file, 'utf8')).totalAmount;
    const t0 = Date.now();
    const got = await get(`http://zxart.ee/api/language:eng/${head}/start:${start}/limit:${limit}`);
    await sleep(2000);
    if (got != null) {
        fs.writeFileSync(file, got.text);
        console.log(`${name} ${start}+${limit}/${got.j.totalAmount}: ${got.text.length} B in ${Date.now() - t0} ms`);
        return got.j.totalAmount;
    }
    if (limit == 1) {
        fs.appendFileSync(path.join(dir, 'skipped.txt'), `${name} ${start}\n`);
        console.log(`${name} ${start}: skipped`);
        return null;
    }
    const step = limit > 200 ? 200 : limit > 20 ? 20 : 1;
    let total = null;
    for (let s = start; s < start + limit; s += step) {
        const t = await piece(name, head, s, step);
        if (t != null) total = t;
        if (total != null && s + step >= total) break;
    }
    return total;
}

async function all(name, head) {
    let total = Infinity;
    for (let start = 0; start < total; start += kLimit) {
        const t = await piece(name, head, start, kLimit);
        if (t != null) total = t;
    }
}

(async () => {
    // Старые имена без длины - страницы по 1000.
    for (const f of fs.readdirSync(dir)) {
        const m = f.match(/^(\w+)_(\d{6})\.json$/);
        if (m) fs.renameSync(path.join(dir, f), name_of(m[1], Number(m[2]), kLimit));
    }
    const only = process.argv[3];
    if (!only || only === 'base') {
        await all('author', 'export:author/order:date,asc');
        await all('prod', 'export:zxProd/order:date,asc');
        await all('release', `export:zxRelease/order:date,asc/filter:zxReleaseFormat=${kImages}`);
    }
    if (!only || only === 'media') {
        await all('music', 'export:zxMusic/order:date,asc');
        await all('picture', 'export:zxPicture/order:date,asc');
    }
    console.log('done');
})();
