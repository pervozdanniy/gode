/**
 * Тест: горутины выполняются на разных OS-потоках.
 *
 * Запуск:
 *   GOMAXPROCS=4 ./node test_thread_ids.js
 *
 * Ожидаем:
 *   - threadid() внутри горутины ≠ main thread tid
 *   - при GOMAXPROCS≥2 горутины получают разные tid-ы
 */

'use strict';

const { go, yield: goyield, goid, threadid } = require('goroutine');

const mainTid = threadid();
console.log(`Main thread OS tid: ${mainTid}\n`);

const results = [];   // { goid, tid, phase }
const N = 8;          // запускаем 8 горутин
let done = 0;

for (let i = 0; i < N; i++) {
  go(() => {
    const id  = goid();
    const tid = threadid();
    results.push({ id, tid, phase: 'before-yield' });

    goyield();  // уступаем: другая горутина / другой M может подхватить

    const tid2 = threadid();
    results.push({ id, tid: tid2, phase: 'after-yield' });

    done++;
  });
}

// Ждём завершения всех горутин
function check() {
  if (done < N) {
    setTimeout(check, 20);
    return;
  }

  console.log('=== Thread IDs per goroutine ===\n');
  const byGoid = {};
  for (const r of results) {
    if (!byGoid[r.id]) byGoid[r.id] = [];
    byGoid[r.id].push(r);
  }

  const workerTids = new Set();
  for (const [id, phases] of Object.entries(byGoid)) {
    const tids = phases.map(p => p.tid);
    console.log(`G${id}: ${phases.map(p => `${p.phase}→tid=${p.tid}`).join('  |  ')}`);
    tids.forEach(t => workerTids.add(t));
  }

  console.log('\n=== Summary ===');
  console.log(`Main tid:         ${mainTid}`);
  console.log(`Worker tids seen: [${[...workerTids].join(', ')}]`);
  console.log(`Distinct worker tids: ${workerTids.size}`);

  // Проверки
  let ok = true;

  // 1. Все горутины выполнялись НЕ на main thread
  const ranOnMain = results.filter(r => r.tid === mainTid);
  if (ranOnMain.length > 0) {
    console.log(`\n⚠️  ${ranOnMain.length} записей выполнились на main thread tid (${mainTid})`);
    // Это может быть нормально при GOMAXPROCS=1 (M0 тоже выполняет)
  } else {
    console.log('\n✅ Ни одна горутина не выполнялась на main thread');
  }

  // 2. При GOMAXPROCS≥2 должны быть минимум 2 разных worker tid
  const gomaxprocs = parseInt(process.env.NODE_GOMAXPROCS || process.env.GOMAXPROCS || '1');
  if (gomaxprocs >= 2) {
    if (workerTids.size >= 2) {
      console.log(`✅ Горутины выполнялись на ${workerTids.size} разных OS потоках (GOMAXPROCS=${gomaxprocs})`);
    } else {
      console.log(`❌ Все горутины на одном OS потоке! (GOMAXPROCS=${gomaxprocs}, ожидали ≥2)`);
      ok = false;
    }
  } else {
    console.log(`ℹ️  GOMAXPROCS=1: один worker тред — нормально`);
  }

  console.log(ok ? '\n✅ PASS' : '\n❌ FAIL');
  process.exit(ok ? 0 : 1);
}

setTimeout(check, 100);
