// 一次性:Contents API 更新仓库文件(本机 github.com 不通,但 api.github.com 通)
// 用法: node gh-update.js <本地文件> <仓库路径> <commit消息> <sha>
const fs = require('fs');
const https = require('https');

const TOKEN = process.env.GHTOKEN;
const [local, repoPath, msg, sha] = process.argv.slice(2);
if (!local || !repoPath || !msg || !sha || !TOKEN) {
  console.error('用法: node gh-update.js <本地文件> <仓库路径> <commit> <sha>');
  process.exit(2);
}

const b64 = fs.readFileSync(local).toString('base64');
const body = JSON.stringify({ message: msg, content: b64, sha });
const req = https.request({
  host: 'api.github.com',
  path: `/repos/w14769259154-alt/rcfpv/contents/${repoPath}`,
  method: 'PUT',
  headers: {
    Authorization: `Bearer ${TOKEN}`,
    'Content-Type': 'application/json',
    'Content-Length': Buffer.byteLength(body),
    'User-Agent': 'gh-update',
    'Accept': 'application/vnd.github+json',
  },
}, (res) => {
  let d = '';
  res.on('data', (c) => (d += c));
  res.on('end', () => {
    console.log(`status=${res.statusCode} ${d.slice(0, 200)}`);
    process.exit(res.statusCode === 200 || res.statusCode === 201 ? 0 : 1);
  });
});
req.on('error', (e) => { console.error('err:', e.message); process.exit(1); });
req.write(body);
req.end();
