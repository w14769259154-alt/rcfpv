// 一次性:本地 HTTP 服务,供设备 curl 下载部署文件
const http = require('http');
const fs = require('fs');
const path = require('path');
const ROOT = process.argv[2] || __dirname;
http.createServer((req, res) => {
  const f = path.join(ROOT, path.basename(req.url));
  if (!fs.existsSync(f)) { res.writeHead(404); res.end('nf'); return; }
  res.writeHead(200, { 'Content-Length': fs.statSync(f).size });
  fs.createReadStream(f).pipe(res);
  console.log(`served ${path.basename(req.url)}`);
}).listen(8123, '0.0.0.0', () => console.log('http on :8123, ROOT=' + ROOT));
