# 简物 server

一个用 C 写的迷你电商后端：多线程 HTTP 服务 + SQLite + PBKDF2 密码 + Cookie 会话，
自带网页前端，商品数据来自 `data/123.db`（ECShop 结构的商品库）。

页面按四级组织：

| 级别 | 页面 | 做什么 |
|---|---|---|
| 一级 | `index.html` | 登录 / 注册 / 注销（有会话时直接显示已登录） |
| 二级 | `search.html` | 搜索入口：关键词、热门词、分类 |
| 三级 | `results.html` | 搜索结果：排序、分页、加入购物车 |
| 四级 | `detail.html` | 商品详情：图集、价格、库存、参数、加购 |
| 附加 | `shop.html` | 购物车与订单（原页面，补了商品图） |

---

## 1. 目录结构

```
简物-server/
├── src/              后端源码
│   ├── main.c        入口：监听、accept、每连接一个线程
│   ├── http.c/.h     HTTP 解析（按 Content-Length 读完整 body）、响应、静态文件
│   ├── api.c/.h      业务路由：注册/登录/分类/商品/购物车/订单
│   ├── db.c/.h       SQLite 数据访问 + 从 123.db 导入商品
│   ├── crypto.c/.h   SHA-256 / HMAC / PBKDF2 / 随机数（不依赖 OpenSSL）
│   └── util.c/.h     日志、字符串缓冲、JSON 取值、URL 解码、MIME
├── www/              前端与静态资源
│   ├── index.html    一级：登录 / 注册 / 注销
│   ├── search.html   二级：搜索入口 + 推荐商品
│   ├── results.html  三级：搜索结果 + 同类商品
│   ├── detail.html   四级：商品详情
│   ├── shop.html     购物车 / 订单
│   ├── style.css     原样式
│   ├── shop.css      商品页补充样式
│   └── images/       make assets 从 data/images 拷过来
├── data/             商品素材
│   ├── 123.db        ECShop 商品库（goods / goods_gallery / category / brand）
│   └── images/       商品图，路径形如 images/200905/goods_img/xxx.jpg
├── docs/legacy/      改造前的原版代码，留作对照，可以直接删
├── Makefile
└── README.md
```

编译产物：`jianwu`（位于项目根目录），运行后数据存 `jianwu.db`。

---

## 2. 编译运行

```bash
sudo apt install libsqlite3-dev    # 只需要这一个依赖

make                # 编译 + 拷贝商品图片到 www/images
make run            # 编译并运行（8080 端口）

./jianwu                        # 默认 8080 端口、jianwu.db、商品库取默认路径
./jianwu 9000 data.db           # 指定端口和数据库
./jianwu 9000 data.db 123.db    # 再指定商品库路径
```

浏览器打开 <http://127.0.0.1:8080>，登录后自动进入二级搜索页。

> 静态文件路径是相对当前目录的 `www/`，所以要在项目目录里启动。

### 商品数据从哪来

首次启动时，程序会尝试 `ATTACH` 商品库并把 `goods` 表里在售的商品导进本项目的
`products` 表，商品 id 与源库保持一致（购物车、订单都靠这个 id 关联）。

- 商品简介、详情会自动去掉 HTML 标签，只留纯文本；
- 价格从「元」换算成「分」（整数），避免浮点误差；
- 商品图集从 `goods_gallery` 导入，路径补上开头的 `/`，变成站点路径。

商品库不存在或打不开时，商品列表为空（不再内置示例商品），页面显示空状态。

如果数据库里还留着早期版本写死的示例商品（没有商品编号的那些），启动时会被识别出来，
连同相关购物车记录一起清掉，然后重新导入真实商品。用户、会话、历史订单不受影响。

图片要能用，需要把 `data/images/` 拷到 `www/images/`（`make` 会自动做一次）：

```bash
make assets    # 等价于 cp -r data/images www/images
```

重置数据：`rm -f jianwu.db*`（WAL 模式会带 `-wal`、`-shm` 两个文件）。

---

## 3. 接口

| 方法 | 路径 | 需要登录 | 说明 |
|------|------|:---:|------|
| POST | `/api/register` | | `{account, password, nickname?}` |
| POST | `/api/login` | | `{account, password}`，成功后种 Cookie `sid` |
| POST | `/api/logout` | | 清除会话 |
| GET  | `/api/me` | ✓ | 当前用户 |
| GET  | `/api/categories` | | 分类列表（含每类商品数） |
| GET  | `/api/products` | | 商品列表，见下方参数 |
| GET  | `/api/product?id=1` | | 商品详情（含图集） |
| GET  | `/api/cart` | ✓ | 购物车 |
| POST | `/api/cart/add` | ✓ | `{product_id, qty}` |
| POST | `/api/cart/update` | ✓ | `{product_id, qty}`，qty≤0 即删除 |
| POST | `/api/cart/remove` | ✓ | `{product_id}` |
| POST | `/api/order/create` | ✓ | 结算（事务：扣库存 + 写订单 + 清购物车） |
| GET  | `/api/orders` | ✓ | 订单列表（含明细） |

### 商品列表参数

| 参数 | 说明 |
|---|---|
| `keyword` | 模糊匹配商品名、卖点、品牌、分类、商品编号 |
| `cat` | 分类名精确匹配 |
| `brand` | 品牌名精确匹配 |
| `sort` | `price_asc` / `price_desc` / `hot` / `new`，默认按 id |
| `page` | 第几页，从 1 开始 |
| `size` | 每页条数，默认 12，最大 48 |

返回：

```json
{"code":0,"msg":"ok","data":{
  "items":[...],"count":12,"total":31,"page":1,"size":12,"pages":3,"keyword":"诺基亚"
}}
```

商品对象主要字段：`id`、`name`、`subtitle`、`price`（分）、`market_price`（分）、
`stock`、`category`、`brand`、`sn`、`thumb`、`image`、`clicks`、`description`。
详情接口额外返回 `images` 数组（图集）。

统一响应：

```json
{"code":0,"msg":"ok","data":{...}}
```

`code` 非 0 即失败，`msg` 是给人看的原因（401 未登录、400 参数错、409 账号已存在…）。

### curl 速查

```bash
# 注册
curl -X POST localhost:8080/api/register \
     -H 'Content-Type: application/json' \
     -d '{"account":"demo","password":"123456"}'

# 登录（-c 存 cookie）
curl -c /tmp/c.txt -X POST localhost:8080/api/login \
     -H 'Content-Type: application/json' \
     -d '{"account":"demo","password":"123456"}'

# 分类 / 搜索 / 详情
curl localhost:8080/api/categories
curl 'localhost:8080/api/products?keyword=诺基亚&sort=price_asc&page=1&size=12'
curl 'localhost:8080/api/product?id=1'

# 加购物车 / 下单（-b 带 cookie）
curl -b /tmp/c.txt -X POST localhost:8080/api/cart/add \
     -H 'Content-Type: application/json' -d '{"product_id":1,"qty":2}'
curl -b /tmp/c.txt -X POST localhost:8080/api/order/create
curl -b /tmp/c.txt localhost:8080/api/orders
```

---

## 4. 数据表

| 表 | 说明 |
|---|---|
| `users` | 账号 + `pbkdf2-sha256$10000$salt$hash` |
| `sessions` | 登录态，32 字节随机 token，7 天过期 |
| `products` | 商品，价格存**分**（整数）；含品牌、编号、缩略图、主图、浏览量 |
| `product_images` | 商品图集，`(product_id, url, sort)` |
| `cart_items` | 购物车，主键 `(user_id, product_id)` |
| `orders` / `order_items` | 订单 + 明细快照（商品改价不影响历史订单） |

老版本数据库不用删：启动时会检查 `products` 缺哪些字段并自动 `ALTER TABLE` 补上。

---

## 5. 四级页面是怎么串起来的

```
index.html（一级，登录/注册/注销）
   └─ 登录成功 → search.html（二级，关键词 / 热门词 / 分类）
        └─ 提交或点分类 → results.html（三级，排序 + 分页 + 加购）
             └─ 点卡片 → detail.html（四级，图集 + 参数 + 数量 + 加购）
                  └─ 去购物车 → shop.html
```

每一页进入时都会先请求 `/api/me`，没登录会被踢回一级页面，保证后面的购物车、
订单接口拿得到会话。

---

## 6. 相比原版改了什么

| 原版问题 | 现在 |
|---|---|
| 收到 `\r\n\r\n` 就返回，POST body 可能没收全 | 解析 `Content-Length`，循环读到完整 body |
| 单线程，一个慢客户端卡住全站 | 一个连接一个线程（10 秒收发超时） |
| 密码用 DJB2 加盐，可秒破 | PBKDF2-HMAC-SHA256，10000 次迭代 + 16 字节盐 |
| 登录后无状态，谁都能冒充 | 随机会话 token + HttpOnly Cookie + 过期清理 |
| 用户存 `users.txt`，自己拼 SQL | SQLite 预处理语句，全部参数绑定 |
| 前端登录只是 `alert('登录成功')` | 真的调接口，登录后跳到二级搜索页 |
| 商品只有 12 条手写示例 | 从 123.db 导入 26 件在售商品与 42 张图集 |
| 只有「登录页 + 商品页」两层 | 登录 → 搜索（含推荐）→ 结果（含同类）→ 详情 四级动线 |

---

## 7. 安全边界（给你个底）

- SQL 注入：全部走 `sqlite3_bind_*`，没有字符串拼 SQL；搜索的 `LIKE` 也是绑定参数。
- XSS：后端 `json_escape`，前端渲染统一过 `esc()`。
- 路径穿越：静态文件拒绝 `..`。
- 越权：订单/购物车接口一律用会话里的 `user_id`，不信任前端传的 id。
- 密码：只在登录时用 PBKDF2 校验，数据库里只有盐和哈希。

**没做的**（答辩时被问到可以答"知道但没做"）：HTTPS、CSRF token、
登录失败次数限制、管理员后台、支付对接。

---

## 8. 交叉编译到板子（可选）

```bash
make CC=arm-linux-gnueabihf-gcc \
     LDLIBS="-L$HOME/arm-libs/lib -lsqlite3 -lpthread" \
     CFLAGS="-O2 -std=gnu11 -D_GNU_SOURCE -pthread -I$HOME/arm-libs/include"
```

板上把 `jianwu`、`www/`（含 `images/`）、`libsqlite3.so` 一起拷过去，执行 `./jianwu 8080` 即可。
板子 512MB 内存跑这个完全够。
