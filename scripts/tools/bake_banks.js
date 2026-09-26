// Выпечка банков .ssb: все параметры печи - здесь.
//
//   node scripts/tools/bake_banks.js                          все банки
//   node scripts/tools/bake_banks.js sgm                      один
//   node scripts/tools/bake_banks.js sgm --drum-gain-db 9     подмена ключа
//   node scripts/tools/bake_banks.js sgm --out SGM-test.ssb   в другой файл
//
// Ключи хранятся кодом, а не набираются руками: потерянный --fuse-stereo
// раскладывает стереопары двумя сэмплами, и банк вырастает на 31%.

const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const BAKER = 'build/pc/tools/Release/sf2bake.exe';

// Громкостный баланс - две ручки, и обе здесь.
//   --bank-gain-db  общее усиление мелодических программ
//   --drum-gain-db  усиление наборов ударных (банк 128)
// Баланс "ударные против всего остального" это их разность: у всех трёх
// банков она нулевая.
//
// SGM и Timbres of Heaven пекутся без усиления, уровни как в самом .sf2:
// с +6 на мелодических и +12 на ударных SGM громче эталона на 2.6..6.0 дБ
// по десяти файлам, медиана +4, больше всего на файлах с ударными.
const BANKS = {
    gu: {
        name: 'GeneralUser GS',
        src: 'SF2/GeneralUser-GS.sf2',
        out: 'release/banks/GeneralUser-GS.ssb',
        // Без усиления, как SGM и Timbres: уровни как в самом .sf2. С +6 дБ на
        // обеих ручках банк выходил на 6 дБ выше эталонного синтезатора
        // (Bond.mid: RMS -16.1 против -22.2 дБFS, 96 срезанных кадров против
        // нуля), и живой MIDI, который играет только из него, перегружал сумму.
        args: ['--rate-cap', '32000'],
    },
    sgm: {
        name: 'SGM-V2.01',
        src: 'SF2/SGM-V2.01.sf2',
        out: 'release/banks/SGM.ssb',
        args: ['--rate-cap', '32000', '--rate-auto', '--rate-loss-db', '-30',
               '--rate-auto-min', '11025', '--fuse-stereo'],
    },
    toh: {
        name: 'Timbres of Heaven 4.00',
        src: 'SF2/Timbres of Heaven (XGM) 4.00(G).sf2',
        out: 'release/banks/Timbres-of-Heaven.ssb',
        // --keep-dup-layers только здесь: у Timbres одинаковые слои стоят в
        // самом .sf2 (без ключа до -6 дБ), у SGM дубли возникают при
        // группировке профилей, и ключ даёт Tenor Sax +6 дБ.
        args: ['--rate-cap', '32000', '--rate-loss-db', '-20', '--fuse-stereo',
               '--merge-zones', '3', '--merge-bright-db', '1.8', '--keep-dup-layers'],
    },
};

// Подмена ключа: если он уже есть - заменяем значение, иначе добавляем.
function override(args, flag, value) {
    const i = args.indexOf(flag);
    if (i >= 0) { args[i + 1] = value; return args; }
    return args.concat([flag, value]);
}

function main() {
    const argv = process.argv.slice(2);
    const which = argv.length && !argv[0].startsWith('--') ? [argv.shift()] : Object.keys(BANKS);
    for (const b of which) if (!BANKS[b]) { console.error('нет такого банка: ' + b + ' (есть: ' + Object.keys(BANKS).join(', ') + ')'); process.exit(2); }

    for (const key of which) {
        const b = BANKS[key];
        let args = b.args.slice();
        let out = b.out;
        for (let i = 0; i < argv.length - 1; i += 2) {
            if (argv[i] === '--out') out = argv[i + 1];
            else args = override(args, argv[i], argv[i + 1]);
        }
        if (!fs.existsSync(b.src)) { console.log(b.name + ': исходника нет (' + b.src + '), пропускаю'); continue; }
        console.log('=== ' + b.name + ' -> ' + out);
        console.log('    ' + args.join(' '));
        const t0 = Date.now();
        const res = cp.spawnSync(BAKER, [b.src, out, ...args], { encoding: 'utf8', maxBuffer: 1 << 28 });
        if (res.error) { console.error('    не запустился: ' + res.error.message); process.exit(1); }
        const text = (res.stdout || '') + (res.stderr || '');
        for (const line of text.split(/\r?\n/))
            if (/ВНИМАНИЕ|яркость|огибающая|слоёв|записан|Гц/.test(line)) console.log('  ' + line.trim());
        if (res.status !== 0) { console.error('    код возврата ' + res.status); process.exit(1); }
        const mb = fs.existsSync(out) ? (fs.statSync(out).size / 1048576).toFixed(2) : '?';
        console.log('    готово: ' + mb + ' МБ за ' + ((Date.now() - t0) / 1000).toFixed(0) + ' с');
    }
}

main();
