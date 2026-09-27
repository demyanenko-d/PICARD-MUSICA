// Проверка готового образа .uf2 перед выпуском.
//
//   node scripts/tools/uf2_check.js <образ.uf2>
//
// Ловит то, что глазами не видно, а стоит получаса разбора: загрузчик
// RP2350 берёт только блоки с нагрузкой ровно в 256 байт, а короткий
// выбрасывает молча. Один такой блок - и принятых блоков на один меньше
// объявленного: плата не перезагружается после заливки, диск остаётся
// смонтированным, а кусок данных во флеш не попадает.
//
// Заодно сверяется нумерация: blockNo идут подряд внутри семейства, а
// numBlocks у всех одинаков и равен их числу.

const fs = require('fs');

const BLOCK = 512;
const MAGIC0 = 0x0a324655;
const MAGIC1 = 0x9e5d5157;
const MAGIC_END = 0x0ab16f30;
const PAYLOAD = 256;

function main() {
    const path = process.argv[2];
    if (!path) {
        console.error('uf2_check: нужен путь к образу');
        process.exit(2);
    }
    const b = fs.readFileSync(path);
    if (b.length === 0 || b.length % BLOCK !== 0) {
        console.error('uf2_check: ' + path + ' не кратен 512 байтам - это не UF2');
        process.exit(1);
    }

    const total = b.length / BLOCK;
    const fam = new Map(); // familyID -> { count, numBlocks, nextNo, badNo }
    const errors = [];

    for (let i = 0; i < total; i++) {
        const o = i * BLOCK;
        if (b.readUInt32LE(o) !== MAGIC0 || b.readUInt32LE(o + 4) !== MAGIC1 ||
            b.readUInt32LE(o + BLOCK - 4) !== MAGIC_END) {
            errors.push('блок ' + i + ': битые сигнатуры');
            continue;
        }
        const size = b.readUInt32LE(o + 16);
        if (size !== PAYLOAD) {
            errors.push('блок ' + i + ' по адресу 0x' + b.readUInt32LE(o + 12).toString(16) + ': нагрузка ' + size +
                        ' байт вместо ' + PAYLOAD + ' - загрузчик его выбросит');
        }
        const id = b.readUInt32LE(o + 28);
        const no = b.readUInt32LE(o + 20);
        const num = b.readUInt32LE(o + 24);
        let f = fam.get(id);
        if (f === undefined) {
            f = { count: 0, numBlocks: num, nextNo: 0, badNo: 0, badNum: 0 };
            fam.set(id, f);
        }
        if (num !== f.numBlocks) ++f.badNum;
        if (no !== f.nextNo) ++f.badNo;
        f.nextNo = no + 1;
        ++f.count;
    }

    for (const [id, f] of fam) {
        const name = '0x' + id.toString(16);
        if (f.badNo) errors.push('семейство ' + name + ': ' + f.badNo + ' блоков с непоследовательным blockNo');
        if (f.badNum) errors.push('семейство ' + name + ': ' + f.badNum + ' блоков с расходящимся numBlocks');
        // Семейство absolute приходит от picotool одним блоком с
        // numBlocks = 2 - так он его и выпускает, это не ошибка.
        if (f.numBlocks !== f.count && id !== 0xe48bff57) {
            errors.push('семейство ' + name + ': объявлено ' + f.numBlocks + ' блоков, в файле ' + f.count +
                        ' - загрузчик не досчитается и не перезагрузит плату');
        }
    }

    const mb = (b.length / (1024 * 1024)).toFixed(2);
    if (errors.length) {
        console.error('uf2_check: ' + path + ' (' + mb + ' МБ, ' + total + ' блоков) - НЕГОДЕН');
        for (const e of errors.slice(0, 20)) console.error('  ' + e);
        if (errors.length > 20) console.error('  ... и ещё ' + (errors.length - 20));
        process.exit(1);
    }

    const parts = [...fam].map(([id, f]) => '0x' + id.toString(16) + ': ' + f.count).join(', ');
    console.log('uf2_check: ' + path + ' - ' + mb + ' МБ, ' + total + ' блоков (' + parts + ') - в порядке');
}

main();
