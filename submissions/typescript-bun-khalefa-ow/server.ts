// The $12 server challenge API on Bun: Bun.serve + bun:sqlite, one process, no dependencies.
//
// Bun's HTTP server (uWebSockets underneath) and its SQLite binding are both native, so the
// JavaScript on a request's path is just routing, a prepared-statement call and a string build.
// Everything runs on one thread: the box has one vCPU and every query is an index lookup.
import { Database } from "bun:sqlite";
import { createHmac, timingSafeEqual } from "node:crypto";

const SQLITE_PATH = process.env.SQLITE_PATH;
const JWT_SECRET = process.env.JWT_SECRET;
if (!SQLITE_PATH || JWT_SECRET === undefined) throw new Error("SQLITE_PATH and JWT_SECRET must be set");
const HOST = process.env.HOST || "127.0.0.1";
const PORT = Number(process.env.PORT || 3000);
const START = performance.now();

// --- database ---------------------------------------------------------------------------------

const db = new Database(SQLITE_PATH, { strict: true });
db.run("PRAGMA journal_mode = WAL");
db.run("PRAGMA synchronous = NORMAL"); // rule 6: WAL + NORMAL
db.run("PRAGMA busy_timeout = 5000");
db.run("PRAGMA mmap_size = 1073741824"); // reads go straight to the OS page cache
db.run("PRAGMA cache_size = -32768"); // 32 MiB
db.run("PRAGMA temp_store = MEMORY");

// Column aliases match the wire keys, in wire order, so rows serialize as post objects as is.
const POST_SELECT = `
SELECT p.id, p.body, p.created_at, u.username AS author,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id`;
const pingStmt = db.prepare("SELECT 1");
const feedStmt = db.prepare(`${POST_SELECT} ORDER BY p.created_at DESC, p.id DESC LIMIT 20`);
const postStmt = db.prepare(`${POST_SELECT} WHERE p.id = $id`);
const insertPostStmt = db.prepare<{ id: number; created_at: string }, any>(
  "INSERT INTO posts (user_id, body) VALUES ($user, $body) RETURNING id, created_at",
);
// Inserts only if the post exists; 0 changes means "already liked" or "no such post".
const insertLikeStmt = db.prepare(`
INSERT INTO likes (user_id, post_id)
SELECT $user, $post WHERE EXISTS (SELECT 1 FROM posts WHERE id = $post)
ON CONFLICT (user_id, post_id) DO NOTHING`);
const postExistsStmt = db.prepare("SELECT 1 FROM posts WHERE id = $id");

// --- responses --------------------------------------------------------------------------------

const JSON_HEADERS = { "Content-Type": "application/json" };

function json(status: number, body: string): Response {
  return new Response(body, { status, headers: JSON_HEADERS });
}
function error(status: number, message: string): Response {
  return json(status, `{"error":"${message}"}`);
}
const NOT_FOUND = '{"error":"not found"}';

// A positive integer in plain decimal digits: the id, 0 if invalid, or -1 if it is valid but too
// large for any row to have it (beyond 2^53 is out of scope; treat it as missing).
function parseId(s: string): number {
  if (!/^[0-9]+$/.test(s)) return 0;
  if (s.length > 15) return /^0*$/.test(s) ? 0 : -1;
  const n = Number(s);
  return n > 0 ? n : 0;
}

// --- auth -------------------------------------------------------------------------------------

const B64URL = /^[A-Za-z0-9_-]*$/;

function b64urlJson(s: string): any {
  if (!B64URL.test(s) || s.length % 4 === 1) throw new Error("not base64url");
  return JSON.parse(Buffer.from(s, "base64url").toString("utf8"));
}

type User = { id: number; username: string };

// Returns the user, or an error Response. Every token is fully verified (rule 5).
function authenticate(req: Request): User | Response {
  const header = req.headers.get("authorization");
  if (header === null || !header.startsWith("Bearer ")) return error(401, "missing bearer token");
  let payload: any;
  try {
    const parts = header.slice(7).split(".");
    if (parts.length !== 3) throw new Error("parts");
    const [h, p, sig] = parts;
    const head = b64urlJson(h);
    if (typeof head !== "object" || head === null || head.alg !== "HS256") throw new Error("alg");
    const expected = createHmac("sha256", JWT_SECRET!).update(`${h}.${p}`).digest();
    if (!B64URL.test(sig)) throw new Error("sig");
    const given = Buffer.from(sig, "base64url");
    if (given.length !== expected.length || !timingSafeEqual(given, expected)) throw new Error("sig");
    payload = b64urlJson(p);
    if (typeof payload !== "object" || payload === null || Array.isArray(payload)) throw new Error("payload");
    const now = Math.floor(Date.now() / 1000);
    const { exp, nbf } = payload;
    if (exp != null && (typeof exp !== "number" || now >= exp)) throw new Error("exp");
    if (nbf != null && (typeof nbf !== "number" || now < nbf)) throw new Error("nbf");
  } catch {
    return error(401, "invalid or expired token");
  }
  const { sub, username } = payload;
  const id = typeof sub === "string" ? parseId(sub) : 0;
  if (id <= 0 || typeof username !== "string") return error(401, "invalid token payload");
  return { id, username };
}

// Length in Unicode code points (as SQLite's length() counts), not UTF-16 units.
function codePoints(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0xdc00 || c > 0xdfff || i === 0 || (s.charCodeAt(i - 1) & 0xfc00) !== 0xd800) n++;
  }
  return n;
}

// --- handlers ---------------------------------------------------------------------------------

function health(): Response {
  try {
    pingStmt.get();
  } catch (e) {
    return json(503, JSON.stringify({ status: "degraded", db: "unreachable", error: String((e as Error).message) }));
  }
  return json(200, `{"status":"ok","db":"ok","uptime_s":${Math.floor((performance.now() - START) / 1000)}}`);
}

function feed(): Response {
  return json(200, `{"posts":${JSON.stringify(feedStmt.all())}}`);
}

function getPost(rawId: string): Response {
  const id = parseId(rawId);
  if (id === 0) return error(400, "invalid post id");
  const row = id > 0 ? postStmt.get({ id }) : null;
  if (row === null) return error(404, "post not found");
  return json(200, `{"post":${JSON.stringify(row)}}`);
}

async function createPost(req: Request): Promise<Response> {
  const user = authenticate(req);
  if (user instanceof Response) return user;
  let data: any;
  try {
    data = JSON.parse(await req.text());
  } catch {
    return error(400, "malformed JSON body");
  }
  let body = typeof data === "object" && data !== null && !Array.isArray(data) ? data.body : undefined;
  if (typeof body !== "string") return error(400, "body is required");
  body = body.trim();
  if (body.length === 0) return error(400, "body is required");
  if (body.length > 500 && codePoints(body) > 500) return error(400, "body must be at most 500 characters");
  // Autocommit: get() runs the statement to completion, so the row is committed before we respond.
  const row = insertPostStmt.get({ user: user.id, body })!;
  return json(
    201,
    JSON.stringify({
      post: { id: row.id, body, created_at: row.created_at, author: user.username, like_count: 0 },
    }),
  );
}

function like(req: Request, rawId: string): Response {
  const user = authenticate(req); // auth before the id, per the spec
  if (user instanceof Response) return user;
  const id = parseId(rawId);
  if (id === 0) return error(400, "invalid post id");
  if (id > 0) {
    if (insertLikeStmt.run({ user: user.id, post: id }).changes === 1)
      return json(201, `{"liked":true,"already_liked":false,"post_id":${id}}`);
    if (postExistsStmt.get({ id }) !== null)
      return json(200, `{"liked":true,"already_liked":true,"post_id":${id}}`);
  }
  return error(404, "post not found");
}

// --- server -----------------------------------------------------------------------------------

Bun.serve({
  hostname: HOST,
  port: PORT,
  idleTimeout: 120, // SPEC: keep idle keep-alive connections open for at least 65 s
  development: false,
  routes: {
    "/health": { GET: health },
    "/feed": { GET: feed },
    "/posts": { POST: createPost },
    "/posts/:id": { GET: (req) => getPost(req.params.id) },
    "/posts/:id/like": { POST: (req) => like(req, req.params.id) },
  },
  fetch() {
    return json(404, NOT_FOUND);
  },
  error() {
    return error(500, "internal server error");
  },
});
console.log(`listening on ${HOST}:${PORT}`);
