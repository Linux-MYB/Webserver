# 简物 server

用 C 写的迷你电商后端：多线程 HTTP 服务 + SQLite + SHA-256 密码 + Cookie 会话，自带网页前端，
商品数据来自 `data/123.db`（ECShop 结构）。

页面：`index.html` 登录注册 / `search.html` 搜索入口 / `results.html` 搜索结果 / `detail.html` 商品详情 / `shop.html` 购物车与订单。

## 结构

```
src/          main http api db crypto util
www/          前端页面与静态资源
data/         123.db 与商品图
docs/legacy/  改造前的原版代码
```

## 编译运行

```bash
sudo apt install libsqlite3-dev
make
./jianwu
```

浏览器打开 <http://127.0.0.1:8080>，静态文件按当前目录的 `www/` 查找。

## 接口

```
POST /api/register  /api/login  /api/logout
GET  /api/me  /api/categories  /api/products  /api/product?id=1
GET  /api/cart   POST /api/cart/add /api/cart/update /api/cart/remove
POST /api/order/create   GET /api/orders
```

`/api/products` 支持 `keyword` `cat` `brand` `sort`（price_asc / price_desc / hot / new）`page` `size`。
