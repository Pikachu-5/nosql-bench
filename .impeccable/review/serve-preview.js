const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const routes = {
  '/': ['.impeccable/review/preview.html', 'text/html; charset=utf-8'],
  '/css/app.css': ['ui/wwwroot/css/app.css', 'text/css; charset=utf-8'],
  '/fonts/CascadiaCode.ttf': ['ui/wwwroot/fonts/CascadiaCode.ttf', 'font/ttf']
};
http.createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  const route = routes[pathname];
  if (request.method !== 'GET' || !route) { response.writeHead(404); response.end('Not found'); return; }
  response.writeHead(200, { 'content-type': route[1], 'cache-control': 'no-store' });
  response.end(fs.readFileSync(path.join(root, route[0])));
}).listen(5218, '127.0.0.1');
