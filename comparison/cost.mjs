// comparison/cost.mjs <tokens.json> <server-pid> <mode>: 8 s closed loop of one request type (200 keep-alive
// connections), then prints the server's user/sys CPU microseconds per request from /proc.
import http from 'node:http';
import fs from 'node:fs';
const [tokensFile, pid, mode] = process.argv.slice(2);
const tokens = JSON.parse(fs.readFileSync(tokensFile));
const PORT = +(process.env.P || 3000);
const HZ = 100;
const cpu = () => { const f = fs.readFileSync(`/proc/${pid}/stat`, 'utf8').split(') ')[1].split(' '); return [+f[11], +f[12]]; };
const agent = new http.Agent({ keepAlive: true, maxSockets: 200 });
let done = 0, errs = 0, likeUser = 0;
const end = Date.now() + 8000;
// HOT=1: post/like ids come from the current /feed (refreshed every 50 ms) instead of 1..500000.
let hot = [];
const refresh = () => new Promise(res => http.get({ host: '127.0.0.1', port: PORT, path: '/feed' }, r => {
  let b = ''; r.on('data', d => b += d); r.on('end', () => { try { hot = JSON.parse(b).posts.map(p => p.id); } catch {} res(); });
}).on('error', res));
if (process.env.HOT) { await refresh(); setInterval(refresh, 50).unref(); }
function opts() {
  const id = process.env.HOT ? hot[Math.floor(Math.random() * hot.length)] : 1 + Math.floor(Math.random() * 500000);
  let m = mode;
  if (m === 'mix') { // bench/load.js ratios per loop: feed 1, post 1, like 0.15, create 0.02
    const r = Math.random() * 2.17;
    m = r < 1 ? 'feed' : r < 2 ? 'post' : r < 2.15 ? 'like' : 'create';
  }
  switch (m) {
    case 'health': return { path: '/health' };
    case 'feed': return { path: '/feed' };
    case 'post': return { path: '/posts/' + id };
    case 'like': return { method: 'POST', path: `/posts/${id}/like`, headers: { Authorization: 'Bearer ' + tokens[likeUser++ % tokens.length].token } };
    case 'create': return { method: 'POST', path: '/posts', body: JSON.stringify({ body: 'hello from the cost test ' + done }),
      headers: { Authorization: 'Bearer ' + tokens[likeUser++ % tokens.length].token, 'Content-Type': 'application/json' } };
  }
}
function one() {
  if (Date.now() > end) return Promise.resolve();
  const o = { host: '127.0.0.1', port: PORT, agent, method: 'GET', ...opts() };
  return new Promise(res => {
    const rq = http.request(o, r => { r.resume(); r.on('end', () => { if (r.statusCode >= 300) errs++; done++; res(); }); });
    rq.on('error', () => { errs++; res(); }); rq.end(o.body);
  }).then(one);
}
const [u0, s0] = cpu(); const t0 = Date.now();
await Promise.all(Array.from({ length: 200 }, one));
const [u1, s1] = cpu(); const secs = (Date.now() - t0) / 1000;
const us = (ticks) => (ticks / HZ * 1e6 / done).toFixed(1);
console.log(`${mode.padEnd(6)} rps=${Math.round(done / secs)} errs=${errs} server-cpu=${(((u1 - u0) + (s1 - s0)) / HZ / secs * 100).toFixed(0)}% ` +
  `per-request: user=${us(u1 - u0)}us sys=${us(s1 - s0)}us total=${us(u1 - u0 + s1 - s0)}us`);
process.exit(0);
