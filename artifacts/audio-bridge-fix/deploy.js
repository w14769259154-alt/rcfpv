// 一次性部署: 用 ssh2 exec + stdin 流式写入(设备 dropbear 无 sftp-server, 只能 cat 管道)
// 用法: node deploy.js <本地文件> <远程路径> [重启命令]
const path = require('path');
const fs = require('fs');
const { Client } = require('d:\\Users\\1\\Desktop\\fpv\\已完成项目\\APrc管理平台\\deploy\\ssh-tool\\node_modules\\ssh2');

const HOST = process.env.SSH_HOST || '192.168.137.238';
const USER = process.env.SSH_USER || 'root';
const PASS = process.env.SSH_PASS || '12345678';

const [local, remote, restartCmd] = process.argv.slice(2);
if (!local || !remote) { console.error('用法: node deploy.js <本地> <远程> [重启命令]'); process.exit(2); }

const conn = new Client();
conn
  .on('ready', () => {
    const cmd = `cat > ${remote} && chmod 755 ${remote} && ls -l ${remote}`;
    conn.exec(cmd, (err, stream) => {
      if (err) { console.error('exec err:', err.message); conn.end(); process.exit(1); }
      stream.stderr.on('data', (d) => process.stderr.write(d));
      stream.on('close', (code) => {
        console.log('上传完成, exit=' + code);
        if (restartCmd) {
          conn.exec(restartCmd, (err2, s2) => {
            if (err2) { console.error('restart err:', err2.message); conn.end(); process.exit(1); }
            s2.on('data', (d) => process.stdout.write(d));
            s2.stderr.on('data', (d) => process.stderr.write(d));
            s2.on('close', (c2) => { console.log('重启 exit=' + c2); conn.end(); process.exit(c2 || 0); });
          });
        } else { conn.end(); process.exit(0); }
      });
      fs.createReadStream(local).pipe(stream.stdin);
    });
  })
  .on('error', (e) => { console.error('ssh err:', e.message); process.exit(1); })
  .connect({ host: HOST, port: 22, username: USER, password: PASS, readyTimeout: 15000 });
