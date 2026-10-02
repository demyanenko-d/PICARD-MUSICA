// Линтер clang-tidy по коду платы: ядро backend/core, прошивка
// backend/ports/rp2350 и backend/platform. Правила - в .clang-tidy в корне.
//
//   node scripts/tools/lint.js                     весь код платы
//   node scripts/tools/lint.js backend/ports/rp2350      только каталог
//   node scripts/tools/lint.js a.cpp b.cpp         только файлы
//   node scripts/tools/lint.js --fix a.cpp         с правкой на месте
//
// Флаги компиляции берутся из build/zc/compile_commands.json: сначала
// соберите прошивку (scripts/flash_zc.cmd или cmake --build build/zc).
// Системные заголовки - arm-none-eabi из того же тулчейна: clang не
// находит их сам по имени компилятора gcc.
//
// Предупреждение из заголовка печатается один раз, хотя приходит от каждого
// файла, который его включает. Код выхода 1, если предупреждения есть.

const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..');
const DB_DIR = path.join(ROOT, 'build', 'zc');
const SCOPE = ['backend/core', 'backend/devices', 'backend/player', 'backend/ports/rp2350', 'backend/platform'];

function find_clang_tidy() {
    const probe = cp.spawnSync('clang-tidy', ['--version'], {encoding: 'utf8'});
    if (!probe.error && probe.status === 0) return 'clang-tidy';
    const llvm = 'C:/Program Files/LLVM/bin/clang-tidy.exe';
    if (fs.existsSync(llvm)) return llvm;
    console.error('clang-tidy не найден: поставьте LLVM или добавьте его в PATH');
    process.exit(2);
}

function load_db() {
    const file = path.join(DB_DIR, 'compile_commands.json');
    if (!fs.existsSync(file)) {
        console.error(`нет ${path.relative(ROOT, file)}: сначала соберите прошивку zc`);
        process.exit(2);
    }
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}

// Заголовки libstdc++ и newlib из тулчейна, которым собрана прошивка.
function toolchain_includes(db) {
    const cmd = db[0].command || db[0].arguments.join(' ');
    const compiler = cmd.trim().split(/\s+/)[0].replace(/\\/g, '/');
    const root = path.posix.dirname(path.posix.dirname(compiler));
    const inc = `${root}/arm-none-eabi/include`;
    const cxx_dir = `${inc}/c++`;
    const versions = fs.existsSync(cxx_dir) ? fs.readdirSync(cxx_dir) : [];
    if (!versions.length) {
        console.error(`нет заголовков C++ в ${cxx_dir}`);
        process.exit(2);
    }
    const cxx = `${cxx_dir}/${versions.sort().pop()}`;
    return [cxx, `${cxx}/arm-none-eabi/thumb/v8-m.main+fp/softfp`, `${cxx}/backward`, inc];
}

function norm(p) {
    return path.resolve(ROOT, p).toLowerCase();
}

const args = process.argv.slice(2);
const fix = args.includes('--fix');
const targets = args.filter(a => a !== '--fix');

const db = load_db();
const scope = (targets.length ? targets : SCOPE).map(norm);
const files = [...new Set(db.map(e => path.resolve(e.directory, e.file)))]
    .filter(f => /\.(c|cpp)$/.test(f))
    .filter(f => scope.some(s => norm(f) === s || norm(f).startsWith(s + path.sep)))
    .sort();
if (!files.length) {
    console.error('нет файлов для проверки в базе компиляции');
    process.exit(2);
}

const tidy = find_clang_tidy();
// Без признаков DSP и SAT код идёт запасной веткой без <arm_acle.h>: у clang
// этот заголовок сам определяет __wfi, __sev и __nop, и pico-sdk с ним
// сталкивается. Ветки с интринсиками - по одной строке.
const extra = ['--target=arm-none-eabi', '-U__ARM_FEATURE_DSP', '-U__ARM_FEATURE_SAT',
               ...toolchain_includes(db).map(d => `-isystem${d}`)]
    .map(a => `--extra-arg=${a}`);
const jobs = fix ? 1 : Math.max(1, os.cpus().length - 2);

const seen = new Map(); // текст предупреждения -> проверка
let errors = 0;
let next = 0;
let done = 0;

function run_one() {
    if (next >= files.length) return Promise.resolve();
    const file = files[next++];
    return new Promise(resolve => {
        const argv = ['--quiet', '-p', DB_DIR, ...extra, ...(fix ? ['--fix'] : []), file];
        const child = cp.spawn(tidy, argv, {cwd: ROOT});
        let out = '';
        child.stdout.on('data', d => (out += d));
        child.stderr.on('data', d => (out += d));
        child.on('close', () => {
            const lines = out.split(/\r?\n/);
            for (let i = 0; i < lines.length; ++i) {
                const m = /^(.*):(\d+):(\d+): (warning|error): (.*?)(?: \[([\w.,-]+)\])?$/.exec(lines[i]);
                if (!m) continue;
                if (m[4] === 'error') ++errors;
                const where = path.relative(ROOT, m[1]).replace(/\\/g, '/');
                const key = `${where}:${m[2]}:${m[3]}: ${m[4]}: ${m[5]}`;
                if (!seen.has(key)) seen.set(key, {check: m[6] || m[4], snippet: lines[i + 1] || ''});
            }
            if (/: error: /.test(out)) console.error(`\nошибка разбора: ${path.relative(ROOT, file)}`);
            process.stderr.write(`\r${++done}/${files.length}`);
            resolve(run_one());
        });
    });
}

Promise.all(Array.from({length: jobs}, run_one)).then(() => {
    process.stderr.write('\n');
    const keys = [...seen.keys()].sort();
    for (const k of keys) console.log(`${k} [${seen.get(k).check}]\n${seen.get(k).snippet}`);
    const by_check = new Map();
    for (const {check} of seen.values()) by_check.set(check, (by_check.get(check) || 0) + 1);
    console.log(`\nфайлов ${files.length}, предупреждений ${keys.length}` + (errors ? `, ошибок разбора ${errors}` : ''));
    for (const [c, n] of [...by_check].sort((a, b) => b[1] - a[1])) console.log(`  ${String(n).padStart(4)}  ${c}`);
    process.exit(keys.length ? 1 : 0);
});
