#include "db.h"
#include "crypto.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

static sqlite3       *g_db;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ==================== 基础设施 ==================== */

static int db_exec(const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);

    if (rc != SQLITE_OK) {
        log_err("SQL 失败: %s", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static void col_str(sqlite3_stmt *st, int col, char *out, size_t n)
{
    const unsigned char *t = sqlite3_column_text(st, col);

    if (!t) {
        out[0] = '\0';
        return;
    }
    snprintf(out, n, "%s", (const char *)t);
}

/* ==================== 打开 / 建表 ==================== */

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS users("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account TEXT UNIQUE NOT NULL,"
    "  pass_hash TEXT NOT NULL,"
    "  nickname TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS sessions("
    "  token TEXT PRIMARY KEY,"
    "  user_id INTEGER NOT NULL,"
    "  expires_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS products("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL,"
    "  subtitle TEXT NOT NULL DEFAULT '',"
    "  emoji TEXT NOT NULL DEFAULT '',"
    "  price INTEGER NOT NULL,"
    "  market_price INTEGER NOT NULL DEFAULT 0,"
    "  stock INTEGER NOT NULL DEFAULT 0,"
    "  category TEXT NOT NULL DEFAULT '',"
    "  brand TEXT NOT NULL DEFAULT '',"
    "  sn TEXT NOT NULL DEFAULT '',"
    "  thumb TEXT NOT NULL DEFAULT '',"
    "  image TEXT NOT NULL DEFAULT '',"
    "  clicks INTEGER NOT NULL DEFAULT 0,"
    "  description TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE TABLE IF NOT EXISTS product_images("
    "  product_id INTEGER NOT NULL,"
    "  url TEXT NOT NULL,"
    "  sort INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_pimg ON product_images(product_id, sort);"
    "CREATE TABLE IF NOT EXISTS cart_items("
    "  user_id INTEGER NOT NULL,"
    "  product_id INTEGER NOT NULL,"
    "  qty INTEGER NOT NULL,"
    "  PRIMARY KEY(user_id, product_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS orders("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id INTEGER NOT NULL,"
    "  total INTEGER NOT NULL,"
    "  status TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS order_items("
    "  order_id INTEGER NOT NULL,"
    "  product_id INTEGER NOT NULL,"
    "  name TEXT NOT NULL,"
    "  price INTEGER NOT NULL,"
    "  qty INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_orders_user ON orders(user_id, id DESC);"
    "CREATE INDEX IF NOT EXISTS idx_sessions_exp ON sessions(expires_at);";

/* 老库升级：缺哪个字段补哪个（CREATE TABLE IF NOT EXISTS 不会改已有表） */
static int column_exists(const char *table, const char *col)
{
    sqlite3_stmt *st = NULL;
    char sql[128];
    int found = 0;

    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;

    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (name && strcmp((const char *)name, col) == 0) {
            found = 1;
            break;
        }
    }
    sqlite3_finalize(st);
    return found;
}

static void db_migrate(void)
{
    static const struct {
        const char *name;
        const char *def;
    } EXTRA[] = {
        { "market_price", "INTEGER NOT NULL DEFAULT 0" },
        { "brand",        "TEXT NOT NULL DEFAULT ''" },
        { "sn",           "TEXT NOT NULL DEFAULT ''" },
        { "thumb",        "TEXT NOT NULL DEFAULT ''" },
        { "image",        "TEXT NOT NULL DEFAULT ''" },
        { "clicks",       "INTEGER NOT NULL DEFAULT 0" }
    };
    size_t i;
    char sql[192];

    for (i = 0; i < sizeof(EXTRA) / sizeof(EXTRA[0]); i++) {
        if (column_exists("products", EXTRA[i].name)) continue;

        snprintf(sql, sizeof(sql), "ALTER TABLE products ADD COLUMN %s %s;",
                 EXTRA[i].name, EXTRA[i].def);

        /* 并发启动时两个进程可能同时 ALTER，报"重复列"属正常，不当成错误 */
        if (sqlite3_exec(g_db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            log_warn("补字段 %s 失败（可能已存在）", EXTRA[i].name);
        }
    }
}

int db_open(const char *path)
{
    int rc;

    rc = sqlite3_open_v2(path, &g_db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                         SQLITE_OPEN_FULLMUTEX,
                         NULL);
    if (rc != SQLITE_OK) {
        log_err("打开数据库失败: %s", sqlite3_errmsg(g_db));
        if (g_db) sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    sqlite3_busy_timeout(g_db, 3000);

    pthread_mutex_lock(&g_lock);
    rc = db_exec(SCHEMA);
    if (rc == 0) db_migrate();
    pthread_mutex_unlock(&g_lock);

    if (rc != 0) {
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    log_info("数据库就绪: %s", path);
    return 0;
}

void db_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}

/* ==================== 从 ECShop 商品库导入 ==================== */

static int is_space_byte(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* 如果缓冲区正好截在半个汉字上，退回到这个字符的开头 */
static void utf8_trim_tail(char *s, size_t *len)
{
    size_t o = *len;

    if (o == 0) return;

    {
        size_t start = o - 1;
        unsigned char lead;
        size_t need;

        while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80) start--;

        lead = (unsigned char)s[start];
        need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;

        if (start + need > o) *len = start;
    }
}

/* 商品详情在库里是 HTML，去掉标签、合并空白，只留纯文本 */
static void html_to_text(const char *in, char *out, size_t outsz)
{
    const char *p = in ? in : "";
    size_t o = 0;
    int pending_space = 0;

    while (*p && o + 8 < outsz) {
        if (*p == '<') {
            while (*p && *p != '>') p++;
            if (*p) p++;
            pending_space = (o > 0);
            continue;
        }

        if (*p == '&') {
            if (strncmp(p, "&nbsp;", 6) == 0) { p += 6; pending_space = (o > 0); continue; }
            if (strncmp(p, "&#160;", 6) == 0) { p += 6; pending_space = (o > 0); continue; }
            if (strncmp(p, "&amp;", 5) == 0)  { out[o++] = '&';  p += 5; continue; }
            if (strncmp(p, "&quot;", 6) == 0) { out[o++] = '"';  p += 6; continue; }
            if (strncmp(p, "&lt;", 4) == 0)   { out[o++] = '<';  p += 4; continue; }
            if (strncmp(p, "&gt;", 4) == 0)   { out[o++] = '>';  p += 4; continue; }
            if (strncmp(p, "&#39;", 5) == 0)  { out[o++] = '\''; p += 5; continue; }
        }

        if (is_space_byte((unsigned char)*p)) {
            pending_space = (o > 0);
            p++;
            continue;
        }

        if (pending_space) {
            out[o++] = ' ';
            pending_space = 0;
        }
        out[o++] = *p++;
    }

    utf8_trim_tail(out, &o);
    out[o] = '\0';
}

static void abs_img(const char *raw, char *out, size_t n)
{
    if (!raw || !*raw) {
        out[0] = '\0';
        return;
    }
    if (raw[0] == '/') snprintf(out, n, "%s", raw);
    else               snprintf(out, n, "/%s", raw);
}

static const char *SHOP_SELECT =
    "SELECT g.goods_id, g.goods_name, IFNULL(g.goods_sn,''),"
    "       IFNULL(c.cat_name,''), IFNULL(b.brand_name,''),"
    "       IFNULL(g.goods_number,0),"
    "       CAST(g.shop_price * 100 + 0.5 AS INTEGER),"
    "       CAST(IFNULL(g.market_price,0) * 100 + 0.5 AS INTEGER),"
    "       IFNULL(g.goods_img,''), IFNULL(g.goods_thumb,''),"
    "       IFNULL(g.click_count,0), IFNULL(g.goods_desc,''), IFNULL(g.goods_brief,'')"
    "  FROM shop.goods g"
    "  LEFT JOIN shop.category c ON c.cat_id = g.cat_id"
    "  LEFT JOIN shop.brand    b ON b.brand_id = g.brand_id"
    " WHERE g.is_on_sale = 1 AND IFNULL(g.is_delete,0) = 0"
    " ORDER BY g.goods_id;";

/*
 * 把 123.db 里的商品搬进本项目的 products 表。
 *
 * 用 ATTACH 直接读源库，商品 id 保持和源库一致（购物车、订单都靠这个 id 关联）。
 * 返回 0 表示导入成功，1 表示没有可用源库（调用方回退到示例数据）。
 */
static int import_shop(const char *path)
{
    sqlite3_stmt *sel = NULL, *ins = NULL;
    char desc[1024], brief[192];
    char image[PRODUCT_IMG_LEN], thumb[PRODUCT_IMG_LEN];
    int cnt = 0;
    char *err = NULL;

    if (!path || !*path) return 1;

    if (sqlite3_prepare_v2(g_db, "ATTACH DATABASE ? AS shop;", -1, &sel, NULL) != SQLITE_OK) {
        return 1;
    }
    sqlite3_bind_text(sel, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sel) != SQLITE_DONE) {
        sqlite3_finalize(sel);
        log_warn("商品库打不开，改用示例数据: %s", path);
        return 1;
    }
    sqlite3_finalize(sel);

    if (sqlite3_exec(g_db, "BEGIN;", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_exec(g_db, "DETACH DATABASE shop;", NULL, NULL, NULL);
        return 1;
    }

    if (sqlite3_prepare_v2(g_db, SHOP_SELECT, -1, &sel, NULL) != SQLITE_OK) {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_exec(g_db, "DETACH DATABASE shop;", NULL, NULL, NULL);
        return 1;
    }

    if (sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO products"
            "(id,name,subtitle,emoji,price,market_price,stock,category,brand,sn,thumb,image,clicks,description)"
            " VALUES(?,?,?,'',?,?,?,?,?,?,?,?,?,?);", -1, &ins, NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_exec(g_db, "DETACH DATABASE shop;", NULL, NULL, NULL);
        return 1;
    }

    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *desc_raw  = (const char *)sqlite3_column_text(sel, 11);
        const char *brief_raw = (const char *)sqlite3_column_text(sel, 12);

        html_to_text(desc_raw, desc, sizeof(desc));
        html_to_text(brief_raw, brief, sizeof(brief));

        if (!desc[0]) snprintf(desc, sizeof(desc), "%s", brief);

        abs_img((const char *)sqlite3_column_text(sel, 9),  thumb, sizeof(thumb));
        abs_img((const char *)sqlite3_column_text(sel, 8),  image, sizeof(image));

        sqlite3_bind_int64(ins, 1, sqlite3_column_int64(sel, 0));
        sqlite3_bind_text(ins, 2, (const char *)sqlite3_column_text(sel, 1), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, brief, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 4, sqlite3_column_int(sel, 6));   /* price */
        sqlite3_bind_int(ins, 5, sqlite3_column_int(sel, 7));   /* market_price */
        sqlite3_bind_int(ins, 6, sqlite3_column_int(sel, 5));   /* stock */
        sqlite3_bind_text(ins, 7, (const char *)sqlite3_column_text(sel, 3), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 8, (const char *)sqlite3_column_text(sel, 4), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 9, (const char *)sqlite3_column_text(sel, 2), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 10, thumb, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 11, image, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 12, sqlite3_column_int(sel, 10));
        sqlite3_bind_text(ins, 13, desc, -1, SQLITE_TRANSIENT);

        if (sqlite3_step(ins) == SQLITE_DONE) cnt++;
        sqlite3_reset(ins);
    }

    sqlite3_finalize(ins);
    sqlite3_finalize(sel);

    /* 图集：原库存的是相对路径，前面补 / 变成站点路径 */
    sqlite3_exec(g_db,
        "INSERT INTO product_images(product_id,url,sort)"
        " SELECT goods_id,"
        "        CASE WHEN substr(img_url,1,1)='/' THEN img_url ELSE '/'||img_url END,"
        "        img_id"
        "   FROM shop.goods_gallery;", NULL, NULL, NULL);

    if (cnt > 0) sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    else         sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);

    sqlite3_exec(g_db, "DETACH DATABASE shop;", NULL, NULL, NULL);

    return cnt > 0 ? 0 : 1;
}

void db_seed(const char *shop_db)
{
    sqlite3_stmt *st = NULL;
    long count = 0, real = 0;

    pthread_mutex_lock(&g_lock);

    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM products;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) count = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }

    /* 带商品编号的是从 123.db 导进来的真实商品，没有编号的是老版本示例数据 */
    if (sqlite3_prepare_v2(g_db,
            "SELECT COUNT(*) FROM products WHERE sn <> '';", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) real = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }

    if (count > 0 && real > 0) {
        pthread_mutex_unlock(&g_lock);
        return;                       /* 已经是真实商品，不用动 */
    }

    if (count > 0 && real == 0) {
        /* 只剩下上一版写死的示例商品：清掉，其中购物车里的引用也一并清理 */
        log_warn("检测到旧版示例商品 %ld 条，清理后改为导入真实商品库", count);

        sqlite3_exec(g_db, "DELETE FROM cart_items;", NULL, NULL, NULL);
        sqlite3_exec(g_db, "DELETE FROM product_images;", NULL, NULL, NULL);
        sqlite3_exec(g_db, "DELETE FROM products;", NULL, NULL, NULL);
    }

    if (import_shop(shop_db) != 0) {
        log_warn("商品库不可用，商品列表将为空: %s", shop_db ? shop_db : "(未指定)");
    }

    pthread_mutex_unlock(&g_lock);
}

/* ==================== 用户 ==================== */

int db_user_create(const char *account, const char *hash, const char *nickname, long *uid)
{
    sqlite3_stmt *st = NULL;
    int rc;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "INSERT INTO users(account,pass_hash,nickname,created_at) VALUES(?,?,?,?);",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_text(st, 1, account, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, nickname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));

    rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc == SQLITE_DONE) {
        if (uid) *uid = (long)sqlite3_last_insert_rowid(g_db);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    pthread_mutex_unlock(&g_lock);

    if (rc == SQLITE_CONSTRAINT) return 1;
    log_err("创建用户失败: %d", rc);
    return -1;
}

int db_user_auth(const char *account, const char *password, long *uid, char *nick, size_t nicklen)
{
    sqlite3_stmt *st = NULL;
    char hash[256];
    long id = 0;
    int rc;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "SELECT id,pass_hash,nickname FROM users WHERE account=? LIMIT 1;",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_text(st, 1, account, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_lock);
        return 1;   /* 账号不存在 */
    }

    id = (long)sqlite3_column_int64(st, 0);
    col_str(st, 1, hash, sizeof(hash));
    if (nick) col_str(st, 2, nick, nicklen);

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    /* 哈希计算放在锁外，避免阻塞其他请求 */
    if (!password_verify(password, hash)) return 2;

    if (uid) *uid = id;
    return 0;
}

int db_user_info(long uid, char *account, size_t alen, char *nick, size_t nlen)
{
    sqlite3_stmt *st = NULL;
    int rc;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "SELECT account,nickname FROM users WHERE id=? LIMIT 1;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    if (account) col_str(st, 0, account, alen);
    if (nick) col_str(st, 1, nick, nlen);

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ==================== 会话 ==================== */

#define SESSION_TTL (7 * 24 * 3600)

int db_session_new(long uid, char *token, size_t tokensz)
{
    sqlite3_stmt *st = NULL;
    char tok[65];
    int rc;

    if (random_hex(32, tok, sizeof(tok)) != 0) return -1;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "INSERT INTO sessions(token,user_id,expires_at) VALUES(?,?,?);",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_text(st, 1, tok, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)(time(NULL) + SESSION_TTL));

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    if (rc != SQLITE_DONE) return -1;

    snprintf(token, tokensz, "%s", tok);
    return 0;
}

int db_session_uid(const char *token, long *uid)
{
    sqlite3_stmt *st = NULL;
    int rc;

    if (!token || !*token) return -1;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "SELECT user_id FROM sessions WHERE token=? AND expires_at>? LIMIT 1;",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));

    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    *uid = (long)sqlite3_column_int64(st, 0);

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void db_session_del(const char *token)
{
    sqlite3_stmt *st = NULL;

    if (!token || !*token) return;

    pthread_mutex_lock(&g_lock);
    if (sqlite3_prepare_v2(g_db, "DELETE FROM sessions WHERE token=?;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_lock);
}

/* ==================== 商品 ==================== */

#define PRODUCT_COLS \
    "id,name,subtitle,emoji,price,market_price,stock,category,brand,sn,thumb,image,clicks,description"

static void read_product(sqlite3_stmt *st, product_t *p)
{
    p->id = (long)sqlite3_column_int64(st, 0);
    col_str(st, 1, p->name, sizeof(p->name));
    col_str(st, 2, p->subtitle, sizeof(p->subtitle));
    col_str(st, 3, p->emoji, sizeof(p->emoji));
    p->price        = sqlite3_column_int(st, 4);
    p->market_price = sqlite3_column_int(st, 5);
    p->stock        = sqlite3_column_int(st, 6);
    col_str(st, 7, p->category, sizeof(p->category));
    col_str(st, 8, p->brand, sizeof(p->brand));
    col_str(st, 9, p->sn, sizeof(p->sn));
    col_str(st, 10, p->thumb, sizeof(p->thumb));
    col_str(st, 11, p->image, sizeof(p->image));
    p->clicks = sqlite3_column_int(st, 12);
    col_str(st, 13, p->description, sizeof(p->description));
}

/*
 * 拼 WHERE 子句：kw 占 5 个占位符（名称 / 卖点 / 品牌 / 分类 / 编号），
 * cat、brand 各 1 个。返回值是占位符个数，绑定顺序必须和这里一致。
 */
static int build_where(const char *kw, const char *cat, const char *brand,
                       char *where, size_t wsz)
{
    int n = 0;

    where[0] = '\0';

    if (kw && *kw) {
        snprintf(where + strlen(where), wsz - strlen(where),
                 " WHERE (name LIKE ? OR subtitle LIKE ? OR brand LIKE ?"
                 " OR category LIKE ? OR sn LIKE ?)");
        n += 5;
    }
    if (cat && *cat) {
        snprintf(where + strlen(where), wsz - strlen(where),
                 "%s category = ?", n ? " AND" : " WHERE");
        n += 1;
    }
    if (brand && *brand) {
        snprintf(where + strlen(where), wsz - strlen(where),
                 "%s brand = ?", n ? " AND" : " WHERE");
        n += 1;
    }
    return n;
}

static const char *order_by(const char *sort)
{
    if (!sort) return "id ASC";

    if (strcmp(sort, "price_asc")  == 0) return "price ASC, id ASC";
    if (strcmp(sort, "price_desc") == 0) return "price DESC, id ASC";
    if (strcmp(sort, "hot")        == 0) return "clicks DESC, id ASC";
    if (strcmp(sort, "new")        == 0) return "id DESC";

    return "id ASC";
}

static void bind_filters(sqlite3_stmt *st, const char *kw, const char *cat,
                         const char *brand, int *idx)
{
    int i = 1;

    if (kw && *kw) {
        char like[160];
        snprintf(like, sizeof(like), "%%%s%%", kw);
        sqlite3_bind_text(st, i++, like, -1, SQLITE_TRANSIENT);   /* name */
        sqlite3_bind_text(st, i++, like, -1, SQLITE_TRANSIENT);   /* subtitle */
        sqlite3_bind_text(st, i++, like, -1, SQLITE_TRANSIENT);   /* brand */
        sqlite3_bind_text(st, i++, like, -1, SQLITE_TRANSIENT);   /* category */
        sqlite3_bind_text(st, i++, like, -1, SQLITE_TRANSIENT);   /* sn */
    }
    if (cat && *cat)     sqlite3_bind_text(st, i++, cat, -1, SQLITE_TRANSIENT);
    if (brand && *brand) sqlite3_bind_text(st, i++, brand, -1, SQLITE_TRANSIENT);

    *idx = i;
}

int db_product_list(const char *kw, const char *cat, const char *brand,
                    const char *sort, int page, int size,
                    product_t *arr, int max, int *n, int *total)
{
    sqlite3_stmt *st = NULL;
    char where[384];
    char sql[768];
    int cnt = 0, idx = 1, rc;

    *n = 0;
    if (total) *total = 0;

    if (page < 1) page = 1;
    if (size < 1) size = 12;
    if (size > 60) size = 60;
    if (size > max) size = max;

    build_where(kw, cat, brand, where, sizeof(where));

    pthread_mutex_lock(&g_lock);

    /* 先数总数，前端要用它算页数 */
    if (total) {
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM products%s;", where);

        if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK) {
            bind_filters(st, kw, cat, brand, &idx);
            if (sqlite3_step(st) == SQLITE_ROW) *total = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        st = NULL;
    }

    snprintf(sql, sizeof(sql),
             "SELECT " PRODUCT_COLS " FROM products%s ORDER BY %s LIMIT ? OFFSET ?;",
             where, order_by(sort));

    rc = sqlite3_prepare_v2(g_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        log_err("商品查询失败: %s", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    idx = 1;
    bind_filters(st, kw, cat, brand, &idx);
    sqlite3_bind_int(st, idx++, size);
    sqlite3_bind_int(st, idx++, (page - 1) * size);

    while (cnt < max && sqlite3_step(st) == SQLITE_ROW) {
        read_product(st, &arr[cnt]);
        cnt++;
    }

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    *n = cnt;
    return 0;
}

int db_product_get(long id, product_t *p)
{
    sqlite3_stmt *st = NULL;
    int rc;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "SELECT " PRODUCT_COLS " FROM products WHERE id=? LIMIT 1;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);

    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_lock);
        return 1;  /* 不存在 */
    }

    read_product(st, p);

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int db_product_images(long id, char (*urls)[PRODUCT_IMG_LEN], int max, int *n)
{
    sqlite3_stmt *st = NULL;
    int cnt = 0;

    *n = 0;

    pthread_mutex_lock(&g_lock);

    if (sqlite3_prepare_v2(g_db,
            "SELECT url FROM product_images WHERE product_id=? ORDER BY sort LIMIT ?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        sqlite3_bind_int(st, 2, max);

        while (cnt < max && sqlite3_step(st) == SQLITE_ROW) {
            col_str(st, 0, urls[cnt], PRODUCT_IMG_LEN);
            cnt++;
        }
    }

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    *n = cnt;
    return 0;
}

int db_category_list(category_t *arr, int max, int *n)
{
    sqlite3_stmt *st = NULL;
    int cnt = 0;

    *n = 0;

    pthread_mutex_lock(&g_lock);

    if (sqlite3_prepare_v2(g_db,
            "SELECT category, COUNT(*) FROM products WHERE category <> ''"
            " GROUP BY category ORDER BY COUNT(*) DESC, category ASC LIMIT ?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, max);

        while (cnt < max && sqlite3_step(st) == SQLITE_ROW) {
            col_str(st, 0, arr[cnt].name, sizeof(arr[cnt].name));
            arr[cnt].count = sqlite3_column_int(st, 1);
            cnt++;
        }
    }

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    *n = cnt;
    return 0;
}

/* ==================== 购物车 ==================== */

int db_cart_list(long uid, cart_item_t *arr, int max, int *n, int *total)
{
    sqlite3_stmt *st = NULL;
    int rc, cnt = 0, sum = 0;

    *n = 0;
    *total = 0;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "SELECT c.product_id,p.name,p.emoji,p.price,c.qty,p.stock"
            " FROM cart_items c JOIN products p ON p.id=c.product_id"
            " WHERE c.user_id=? ORDER BY c.product_id;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);

    while (cnt < max && sqlite3_step(st) == SQLITE_ROW) {
        cart_item_t *it = &arr[cnt];
        it->product_id = (long)sqlite3_column_int64(st, 0);
        col_str(st, 1, it->name, sizeof(it->name));
        col_str(st, 2, it->emoji, sizeof(it->emoji));
        it->price = sqlite3_column_int(st, 3);
        it->qty = sqlite3_column_int(st, 4);
        it->stock = sqlite3_column_int(st, 5);
        sum += it->price * it->qty;
        cnt++;
    }

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    *n = cnt;
    *total = sum;
    return 0;
}

int db_cart_add(long uid, long pid, int qty, int *err)
{
    sqlite3_stmt *st = NULL;
    int stock = 0, cur = 0, rc;

    if (err) *err = 0;
    if (qty <= 0) qty = 1;

    pthread_mutex_lock(&g_lock);

    /* 商品是否存在、库存多少 */
    rc = sqlite3_prepare_v2(g_db, "SELECT stock FROM products WHERE id=?;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)pid);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_lock);
        if (err) *err = 1;
        return 1;
    }
    stock = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    /* 当前数量 */
    rc = sqlite3_prepare_v2(g_db,
            "SELECT qty FROM cart_items WHERE user_id=? AND product_id=?;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)pid);
    if (sqlite3_step(st) == SQLITE_ROW) cur = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    cur += qty;
    if (cur > stock) {
        cur = stock;
        if (err) *err = 2;
    }
    if (cur <= 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    rc = sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO cart_items(user_id,product_id,qty) VALUES(?,?,?);",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)pid);
    sqlite3_bind_int(st, 3, cur);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_cart_set(long uid, long pid, int qty, int *err)
{
    sqlite3_stmt *st = NULL;
    int stock = 0, rc;

    if (err) *err = 0;

    if (qty <= 0) return db_cart_remove(uid, pid);

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db, "SELECT stock FROM products WHERE id=?;", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)pid);
        if (sqlite3_step(st) == SQLITE_ROW) stock = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }

    if (qty > stock) {
        qty = stock;
        if (err) *err = 2;
    }

    rc = sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO cart_items(user_id,product_id,qty) VALUES(?,?,?);",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)pid);
    sqlite3_bind_int(st, 3, qty);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_cart_remove(long uid, long pid)
{
    sqlite3_stmt *st = NULL;
    int rc;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "DELETE FROM cart_items WHERE user_id=? AND product_id=?;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)pid);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_cart_clear(long uid)
{
    sqlite3_stmt *st = NULL;
    int rc;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db, "DELETE FROM cart_items WHERE user_id=?;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ==================== 订单 ==================== */

int db_order_create(long uid, long *order_id, int *total, int *err)
{
    cart_item_t items[64];
    int n = 0, sum = 0, i, rc;
    sqlite3_stmt *st = NULL;
    long oid = 0;

    if (err) *err = 0;
    *order_id = 0;
    *total = 0;

    pthread_mutex_lock(&g_lock);

    if (db_exec("BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    rc = sqlite3_prepare_v2(g_db,
            "SELECT c.product_id,p.name,p.emoji,p.price,c.qty,p.stock"
            " FROM cart_items c JOIN products p ON p.id=c.product_id"
            " WHERE c.user_id=?;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        goto fail;
    }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);

    while (n < 64 && sqlite3_step(st) == SQLITE_ROW) {
        cart_item_t *it = &items[n];
        it->product_id = (long)sqlite3_column_int64(st, 0);
        col_str(st, 1, it->name, sizeof(it->name));
        col_str(st, 2, it->emoji, sizeof(it->emoji));
        it->price = sqlite3_column_int(st, 3);
        it->qty = sqlite3_column_int(st, 4);
        it->stock = sqlite3_column_int(st, 5);
        n++;
    }
    sqlite3_finalize(st);
    st = NULL;

    if (n == 0) {
        if (err) *err = 1;
        goto fail;
    }

    for (i = 0; i < n; i++) {
        if (items[i].qty > items[i].stock) {
            if (err) *err = 2;
            goto fail;
        }
        sum += items[i].price * items[i].qty;
    }

    rc = sqlite3_prepare_v2(g_db,
            "INSERT INTO orders(user_id,total,status,created_at) VALUES(?,?,'已支付',?);",
            -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;

    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int(st, 2, sum);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));

    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        st = NULL;
        goto fail;
    }
    sqlite3_finalize(st);
    st = NULL;

    oid = (long)sqlite3_last_insert_rowid(g_db);

    for (i = 0; i < n; i++) {
        rc = sqlite3_prepare_v2(g_db,
                "INSERT INTO order_items(order_id,product_id,name,price,qty) VALUES(?,?,?,?,?);",
                -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;

        sqlite3_bind_int64(st, 1, (sqlite3_int64)oid);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)items[i].product_id);
        sqlite3_bind_text(st, 3, items[i].name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, items[i].price);
        sqlite3_bind_int(st, 5, items[i].qty);

        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            st = NULL;
            goto fail;
        }
        sqlite3_finalize(st);
        st = NULL;

        rc = sqlite3_prepare_v2(g_db,
                "UPDATE products SET stock=stock-? WHERE id=? AND stock>=?;", -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;

        sqlite3_bind_int(st, 1, items[i].qty);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)items[i].product_id);
        sqlite3_bind_int(st, 3, items[i].qty);

        if (sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(g_db) != 1) {
            sqlite3_finalize(st);
            st = NULL;
            if (err) *err = 2;
            goto fail;
        }
        sqlite3_finalize(st);
        st = NULL;
    }

    rc = sqlite3_prepare_v2(g_db, "DELETE FROM cart_items WHERE user_id=?;", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_step(st);
    sqlite3_finalize(st);
    st = NULL;

    if (db_exec("COMMIT;") != 0) goto fail;

    pthread_mutex_unlock(&g_lock);

    *order_id = oid;
    *total = sum;
    return 0;

fail:
    if (st) sqlite3_finalize(st);
    db_exec("ROLLBACK;");
    pthread_mutex_unlock(&g_lock);
    return -1;
}

int db_order_list(long uid, order_t *arr, int max, int *n)
{
    sqlite3_stmt *st = NULL;
    int rc, cnt = 0;

    *n = 0;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "SELECT id,total,status,created_at FROM orders WHERE user_id=? ORDER BY id DESC LIMIT ?;",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int(st, 2, max);

    while (sqlite3_step(st) == SQLITE_ROW && cnt < max) {
        arr[cnt].id = (long)sqlite3_column_int64(st, 0);
        arr[cnt].total = sqlite3_column_int(st, 1);
        col_str(st, 2, arr[cnt].status, sizeof(arr[cnt].status));
        arr[cnt].created_at = (long)sqlite3_column_int64(st, 3);
        cnt++;
    }

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    *n = cnt;
    return 0;
}

int db_order_items(long oid, cart_item_t *arr, int max, int *n)
{
    sqlite3_stmt *st = NULL;
    int rc, cnt = 0;

    *n = 0;

    pthread_mutex_lock(&g_lock);

    rc = sqlite3_prepare_v2(g_db,
            "SELECT product_id,name,price,qty FROM order_items WHERE order_id=?;",
            -1, &st, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)oid);

    while (sqlite3_step(st) == SQLITE_ROW && cnt < max) {
        arr[cnt].product_id = (long)sqlite3_column_int64(st, 0);
        col_str(st, 1, arr[cnt].name, sizeof(arr[cnt].name));
        arr[cnt].emoji[0] = '\0';
        arr[cnt].price = sqlite3_column_int(st, 2);
        arr[cnt].qty = sqlite3_column_int(st, 3);
        arr[cnt].stock = 0;
        cnt++;
    }

    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_lock);

    *n = cnt;
    return 0;
}
