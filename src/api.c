#include "api.h"
#include "crypto.h"
#include "db.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==================== 工具 ==================== */

static int is_post(const http_request_t *req)
{
    return strcmp(req->method, "POST") == 0;
}

static long auth_uid(const http_request_t *req)
{
    char token[128];
    long uid = 0;

    if (http_cookie(req, "sid", token, sizeof(token)) != 0) return 0;
    if (db_session_uid(token, &uid) != 0) return 0;

    return uid;
}

static void require_login(http_request_t *req, long uid)
{
    if (uid == 0) http_json(req, 401, "请先登录", NULL);
}

static int valid_account(const char *s)
{
    size_t i, n = strlen(s);

    if (n < 3 || n > 32) return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

/* 去掉首尾空白，避免「手机 」这种带空格的搜索词查不到东西 */
static void trim_inplace(char *s)
{
    char *p = s;
    size_t n;

    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/* ==================== 账号相关 ==================== */

static void handle_register(http_request_t *req)
{
    char account[64], password[64], nickname[64], hash[256];
    long uid = 0;
    int rc;

    if (http_param(req, "account", account, sizeof(account)) != 0 ||
        http_param(req, "password", password, sizeof(password)) != 0) {
        http_json(req, 400, "账号和密码不能为空", NULL);
        return;
    }

    if (!valid_account(account)) {
        http_json(req, 400, "账号需 3-32 位，只能是字母、数字、下划线", NULL);
        return;
    }

    if (strlen(password) < 6 || strlen(password) > 64) {
        http_json(req, 400, "密码长度需 6-64 位", NULL);
        return;
    }

    if (http_param(req, "nickname", nickname, sizeof(nickname)) != 0 || !nickname[0]) {
        snprintf(nickname, sizeof(nickname), "%s", account);
    }

    if (password_hash(password, hash, sizeof(hash)) != 0) {
        http_json(req, 500, "服务器内部错误", NULL);
        return;
    }

    rc = db_user_create(account, hash, nickname, &uid);

    if (rc == 0) {
        http_json(req, 0, "注册成功", NULL);
    } else if (rc == 1) {
        http_json(req, 409, "该账号已被注册", NULL);
    } else {
        http_json(req, 500, "注册失败，请稍后再试", NULL);
    }
}

static void handle_login(http_request_t *req)
{
    char account[64], password[64], nickname[64];
    char token[128], cookie[256], data[256], nick_esc[128], acc_esc[128];
    long uid = 0;
    int rc;

    if (http_param(req, "account", account, sizeof(account)) != 0 ||
        http_param(req, "password", password, sizeof(password)) != 0) {
        http_json(req, 400, "请输入账号和密码", NULL);
        return;
    }

    rc = db_user_auth(account, password, &uid, nickname, sizeof(nickname));

    if (rc == 1) {
        http_json(req, 404, "账号不存在", NULL);
        return;
    }
    if (rc == 2) {
        http_json(req, 400, "密码错误", NULL);
        return;
    }
    if (rc != 0) {
        http_json(req, 500, "登录失败", NULL);
        return;
    }

    if (db_session_new(uid, token, sizeof(token)) != 0) {
        http_json(req, 500, "创建会话失败", NULL);
        return;
    }

    snprintf(cookie, sizeof(cookie),
             "sid=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
             token, 7 * 24 * 3600);

    json_escape(account, acc_esc, sizeof(acc_esc));
    json_escape(nickname, nick_esc, sizeof(nick_esc));
    snprintf(data, sizeof(data), "{\"account\":\"%s\",\"nickname\":\"%s\"}",
             acc_esc, nick_esc);

    http_json_cookie(req, 0, "登录成功", data, cookie);
}

static void handle_logout(http_request_t *req)
{
    char token[128];

    if (http_cookie(req, "sid", token, sizeof(token)) == 0) {
        db_session_del(token);
    }

    http_json_cookie(req, 0, "已退出", NULL,
                     "sid=; Path=/; HttpOnly; Max-Age=0");
}

static void handle_me(http_request_t *req, long uid)
{
    char account[64], nickname[64];
    char data[512], a_esc[128], n_esc[128];

    if (db_user_info(uid, account, sizeof(account), nickname, sizeof(nickname)) != 0) {
        http_json(req, 401, "会话已失效", NULL);
        return;
    }

    json_escape(account, a_esc, sizeof(a_esc));
    json_escape(nickname, n_esc, sizeof(n_esc));
    snprintf(data, sizeof(data), "{\"account\":\"%s\",\"nickname\":\"%s\"}", a_esc, n_esc);

    http_json(req, 0, "ok", data);
}

/* ==================== 商品 ==================== */

static void product_to_json(strbuf_t *sb, const product_t *p)
{
    char name[256], sub[320], emoji[32], cat[96], brand[96], sn[48];
    char thumb[PRODUCT_IMG_LEN * 2], image[PRODUCT_IMG_LEN * 2], desc[2048];

    json_escape(p->name, name, sizeof(name));
    json_escape(p->subtitle, sub, sizeof(sub));
    json_escape(p->emoji, emoji, sizeof(emoji));
    json_escape(p->category, cat, sizeof(cat));
    json_escape(p->brand, brand, sizeof(brand));
    json_escape(p->sn, sn, sizeof(sn));
    json_escape(p->thumb, thumb, sizeof(thumb));
    json_escape(p->image, image, sizeof(image));
    json_escape(p->description, desc, sizeof(desc));

    sb_printf(sb,
              "{\"id\":%ld,\"name\":\"%s\",\"subtitle\":\"%s\",\"emoji\":\"%s\","
              "\"price\":%d,\"market_price\":%d,\"stock\":%d,"
              "\"category\":\"%s\",\"brand\":\"%s\",\"sn\":\"%s\","
              "\"thumb\":\"%s\",\"image\":\"%s\",\"clicks\":%d,\"description\":\"%s\"}",
              p->id, name, sub, emoji, p->price, p->market_price, p->stock,
              cat, brand, sn, thumb, image, p->clicks, desc);
}

static void handle_products(http_request_t *req)
{
    product_t list[48];
    int n = 0, i, total = 0;
    int page = 1, size = 12, pages = 1;
    long tmp = 0;
    char kw[96] = "", cat[48] = "", brand[48] = "", sort[24] = "";
    strbuf_t data, items;

    http_param(req, "keyword", kw, sizeof(kw));
    http_param(req, "cat", cat, sizeof(cat));
    http_param(req, "brand", brand, sizeof(brand));
    http_param(req, "sort", sort, sizeof(sort));

    trim_inplace(kw);
    trim_inplace(cat);
    trim_inplace(brand);

    if (http_param_long(req, "page", &tmp) == 0 && tmp > 0) page = (int)tmp;
    if (http_param_long(req, "size", &tmp) == 0 && tmp > 0) size = (int)tmp;

    if (size < 1) size = 12;
    if (size > 48) size = 48;

    if (db_product_list(kw, cat, brand, sort, page, size, list, 48, &n, &total) != 0) {
        http_json(req, 500, "读取商品失败", NULL);
        return;
    }

    if (size < 1) size = 12;
    pages = (total + size - 1) / size;
    if (pages < 1) pages = 1;

    sb_init(&items);
    sb_putc(&items, '[');
    for (i = 0; i < n; i++) {
        if (i) sb_putc(&items, ',');
        product_to_json(&items, &list[i]);
    }
    sb_putc(&items, ']');

    sb_init(&data);
    sb_printf(&data,
              "{\"items\":%s,\"count\":%d,\"total\":%d,\"page\":%d,\"size\":%d,\"pages\":%d,"
              "\"keyword\":\"%s\"}",
              sb_cstr(&items), n, total, page, size, pages, kw);

    http_json(req, 0, "ok", sb_cstr(&data));

    sb_free(&data);
    sb_free(&items);
}

static void handle_product(http_request_t *req)
{
    long id = 0;
    product_t p;
    strbuf_t data, obj, imgs;
    char urls[PRODUCT_IMG_MAX][PRODUCT_IMG_LEN];
    int nimg = 0, i;

    if (http_param_long(req, "id", &id) != 0 || id <= 0) {
        http_json(req, 400, "缺少商品 id", NULL);
        return;
    }

    if (db_product_get(id, &p) != 0) {
        http_json(req, 404, "商品不存在", NULL);
        return;
    }

    db_product_images(id, urls, PRODUCT_IMG_MAX, &nimg);

    sb_init(&imgs);
    sb_putc(&imgs, '[');
    for (i = 0; i < nimg; i++) {
        char esc[PRODUCT_IMG_LEN * 2];

        json_escape(urls[i], esc, sizeof(esc));
        if (i) sb_putc(&imgs, ',');
        sb_printf(&imgs, "\"%s\"", esc);
    }
    sb_putc(&imgs, ']');

    /* 商品字段已经是一个完整对象，去掉结尾的 } 再接图集 */
    sb_init(&obj);
    product_to_json(&obj, &p);
    if (obj.len > 0) {
        obj.len--;
        obj.buf[obj.len] = '\0';
    }

    sb_init(&data);
    sb_printf(&data, "%s,\"images\":%s}", sb_cstr(&obj), sb_cstr(&imgs));

    http_json(req, 0, "ok", sb_cstr(&data));

    sb_free(&data);
    sb_free(&obj);
    sb_free(&imgs);
}

static void handle_categories(http_request_t *req)
{
    category_t cats[32];
    int n = 0, i;
    strbuf_t data, arr;

    if (db_category_list(cats, 32, &n) != 0) {
        http_json(req, 500, "读取分类失败", NULL);
        return;
    }

    sb_init(&arr);
    sb_putc(&arr, '[');
    for (i = 0; i < n; i++) {
        char nm[96];

        json_escape(cats[i].name, nm, sizeof(nm));
        if (i) sb_putc(&arr, ',');
        sb_printf(&arr, "{\"name\":\"%s\",\"count\":%d}", nm, cats[i].count);
    }
    sb_putc(&arr, ']');

    sb_init(&data);
    sb_printf(&data, "{\"items\":%s,\"count\":%d}", sb_cstr(&arr), n);

    http_json(req, 0, "ok", sb_cstr(&data));

    sb_free(&data);
    sb_free(&arr);
}

/* ==================== 购物车 ==================== */

static void cart_to_json(strbuf_t *data, long uid)
{
    cart_item_t items[64];
    int n = 0, total = 0, i;
    strbuf_t arr;

    db_cart_list(uid, items, 64, &n, &total);

    sb_init(&arr);
    sb_putc(&arr, '[');
    for (i = 0; i < n; i++) {
        char name[128], emoji[32];

        json_escape(items[i].name, name, sizeof(name));
        json_escape(items[i].emoji, emoji, sizeof(emoji));

        if (i) sb_putc(&arr, ',');
        sb_printf(&arr,
                  "{\"product_id\":%ld,\"name\":\"%s\",\"emoji\":\"%s\","
                  "\"price\":%d,\"qty\":%d,\"stock\":%d,\"subtotal\":%d}",
                  items[i].product_id, name, emoji,
                  items[i].price, items[i].qty, items[i].stock,
                  items[i].price * items[i].qty);
    }
    sb_putc(&arr, ']');

    sb_printf(data, "{\"items\":%s,\"total\":%d,\"count\":%d}", sb_cstr(&arr), total, n);
    sb_free(&arr);
}

static void handle_cart(http_request_t *req, long uid)
{
    strbuf_t data;

    sb_init(&data);
    cart_to_json(&data, uid);
    http_json(req, 0, "ok", sb_cstr(&data));
    sb_free(&data);
}

static void handle_cart_add(http_request_t *req, long uid)
{
    long pid = 0, qty = 1;
    int err = 0;
    strbuf_t data;

    if (http_param_long(req, "product_id", &pid) != 0 || pid <= 0) {
        http_json(req, 400, "缺少商品 id", NULL);
        return;
    }
    http_param_long(req, "qty", &qty);
    if (qty <= 0) qty = 1;

    if (db_cart_add(uid, pid, (int)qty, &err) != 0) {
        http_json(req, 500, "加入购物车失败", NULL);
        return;
    }

    if (err == 1) {
        http_json(req, 404, "商品不存在", NULL);
        return;
    }

    sb_init(&data);
    cart_to_json(&data, uid);
    http_json(req, 0, (err == 2) ? "库存不足，已调整数量" : "已加入购物车", sb_cstr(&data));
    sb_free(&data);
}

static void handle_cart_update(http_request_t *req, long uid)
{
    long pid = 0, qty = 0;
    int err = 0;
    strbuf_t data;

    if (http_param_long(req, "product_id", &pid) != 0 || pid <= 0) {
        http_json(req, 400, "缺少商品 id", NULL);
        return;
    }
    if (http_param_long(req, "qty", &qty) != 0) {
        http_json(req, 400, "缺少数量", NULL);
        return;
    }

    db_cart_set(uid, pid, (int)qty, &err);

    sb_init(&data);
    cart_to_json(&data, uid);
    http_json(req, 0, "已更新", sb_cstr(&data));
    sb_free(&data);
}

static void handle_cart_remove(http_request_t *req, long uid)
{
    long pid = 0;
    strbuf_t data;

    if (http_param_long(req, "product_id", &pid) != 0) {
        http_json(req, 400, "缺少商品 id", NULL);
        return;
    }

    db_cart_remove(uid, pid);

    sb_init(&data);
    cart_to_json(&data, uid);
    http_json(req, 0, "已移除", sb_cstr(&data));
    sb_free(&data);
}

/* ==================== 订单 ==================== */

static void handle_order_create(http_request_t *req, long uid)
{
    long oid = 0;
    int total = 0, err = 0;
    char data[128];

    if (db_order_create(uid, &oid, &total, &err) != 0) {
        if (err == 1) {
            http_json(req, 400, "购物车是空的", NULL);
        } else if (err == 2) {
            http_json(req, 400, "部分商品库存不足，请调整数量", NULL);
        } else {
            http_json(req, 500, "下单失败", NULL);
        }
        return;
    }

    snprintf(data, sizeof(data), "{\"order_id\":%ld,\"total\":%d}", oid, total);
    http_json(req, 0, "下单成功", data);
}

static void handle_orders(http_request_t *req, long uid)
{
    order_t orders[50];
    int n = 0, i;

    if (db_order_list(uid, orders, 50, &n) != 0) {
        http_json(req, 500, "读取订单失败", NULL);
        return;
    }

    strbuf_t data, arr;
    sb_init(&arr);
    sb_putc(&arr, '[');

    for (i = 0; i < n; i++) {
        cart_item_t items[32];
        int m = 0, j;
        char status[64];

        db_order_items(orders[i].id, items, 32, &m);
        json_escape(orders[i].status, status, sizeof(status));

        if (i) sb_putc(&arr, ',');
        sb_printf(&arr,
                  "{\"id\":%ld,\"total\":%d,\"status\":\"%s\",\"created_at\":%ld,\"items\":[",
                  orders[i].id, orders[i].total, status, orders[i].created_at);

        for (j = 0; j < m; j++) {
            char name[128];

            json_escape(items[j].name, name, sizeof(name));
            if (j) sb_putc(&arr, ',');
            sb_printf(&arr,
                      "{\"product_id\":%ld,\"name\":\"%s\",\"price\":%d,\"qty\":%d}",
                      items[j].product_id, name, items[j].price, items[j].qty);
        }
        sb_puts(&arr, "]}");
    }
    sb_putc(&arr, ']');

    sb_init(&data);
    sb_printf(&data, "{\"items\":%s,\"count\":%d}", sb_cstr(&arr), n);

    http_json(req, 0, "ok", sb_cstr(&data));

    sb_free(&data);
    sb_free(&arr);
}

/* ==================== 路由 ==================== */

int api_dispatch(http_request_t *req)
{
    long uid;
    const char *p = req->path;

    if (strncmp(p, "/api/", 5) != 0) return 0;

    /* ---- 无需登录 ---- */
    if (strcmp(p, "/api/register") == 0) {
        if (!is_post(req)) { http_json(req, 405, "只支持 POST", NULL); return 1; }
        handle_register(req);
        return 1;
    }

    if (strcmp(p, "/api/login") == 0) {
        if (!is_post(req)) { http_json(req, 405, "只支持 POST", NULL); return 1; }
        handle_login(req);
        return 1;
    }

    if (strcmp(p, "/api/logout") == 0) {
        handle_logout(req);
        return 1;
    }

    if (strcmp(p, "/api/products") == 0) {
        handle_products(req);
        return 1;
    }

    if (strcmp(p, "/api/categories") == 0) {
        handle_categories(req);
        return 1;
    }

    if (strcmp(p, "/api/product") == 0) {
        handle_product(req);
        return 1;
    }

    /* ---- 需要登录 ---- */
    uid = auth_uid(req);

    if (strcmp(p, "/api/me") == 0) {
        if (!uid) { require_login(req, uid); return 1; }
        handle_me(req, uid);
        return 1;
    }

    if (strcmp(p, "/api/cart") == 0) {
        if (!uid) { require_login(req, uid); return 1; }
        handle_cart(req, uid);
        return 1;
    }

    if (strcmp(p, "/api/cart/add") == 0) {
        if (!uid) { require_login(req, uid); return 1; }
        handle_cart_add(req, uid);
        return 1;
    }

    if (strcmp(p, "/api/cart/update") == 0) {
        if (!uid) { require_login(req, uid); return 1; }
        handle_cart_update(req, uid);
        return 1;
    }

    if (strcmp(p, "/api/cart/remove") == 0) {
        if (!uid) { require_login(req, uid); return 1; }
        handle_cart_remove(req, uid);
        return 1;
    }

    if (strcmp(p, "/api/order/create") == 0) {
        if (!uid) { require_login(req, uid); return 1; }
        if (!is_post(req)) { http_json(req, 405, "只支持 POST", NULL); return 1; }
        handle_order_create(req, uid);
        return 1;
    }

    if (strcmp(p, "/api/orders") == 0) {
        if (!uid) { require_login(req, uid); return 1; }
        handle_orders(req, uid);
        return 1;
    }

    http_json(req, 404, "接口不存在", NULL);
    return 1;
}
