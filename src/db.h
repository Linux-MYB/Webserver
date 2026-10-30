/*
 * db.h —— sqlite3 数据访问层
 *
 * 所有函数都是线程安全的（内部一把大锁 + SQLITE_OPEN_FULLMUTEX）。
 */
#ifndef JIANWU_DB_H
#define JIANWU_DB_H

#include <stddef.h>

#define PRODUCT_IMG_MAX 8
#define PRODUCT_IMG_LEN 192

typedef struct {
    long id;
    char name[128];
    char subtitle[160];     /* 卖点 / 一句话简介 */
    char emoji[16];         /* 没有图时兜底显示 */
    int  price;             /* 单位：分，避免浮点误差 */
    int  market_price;      /* 划线价，单位：分，0 表示没有 */
    int  stock;
    char category[40];
    char brand[40];
    char sn[24];
    char thumb[PRODUCT_IMG_LEN];   /* /images/... 缩略图 */
    char image[PRODUCT_IMG_LEN];   /* /images/... 主图 */
    int  clicks;
    char description[1024];
} product_t;

typedef struct {
    char name[40];
    int  count;
} category_t;

typedef struct {
    long product_id;
    char name[64];
    char emoji[16];
    int  price;
    int  qty;
    int  stock;
} cart_item_t;

typedef struct {
    long id;
    int  total;
    char status[16];
    long created_at;
} order_t;

int  db_open(const char *path);
void db_close(void);

/*
 * 灌数据：商品表为空时才动作。
 * shop_db 指向 ECShop 风格的 123.db，存在就导入；不存在则用内置示例商品。
 */
void db_seed(const char *shop_db);

/* 用户：返回 0 成功，1 已存在，-1 内部错误 */
int db_user_create(const char *account, const char *hash, const char *nickname, long *uid);
/* 校验账号密码：0 成功，1 账号不存在，2 密码错误，-1 内部错误 */
int db_user_auth(const char *account, const char *password, long *uid, char *nick, size_t nicklen);
int db_user_info(long uid, char *account, size_t alen, char *nick, size_t nlen);

/* 会话 */
int db_session_new(long uid, char *token, size_t tokensz);
int db_session_uid(const char *token, long *uid);
void db_session_del(const char *token);

/*
 * 商品检索。
 *   kw / cat / brand  可为 NULL 或空串，表示不限制
 *   sort              NULL/"" | price_asc | price_desc | hot | new
 *   page              从 1 开始
 *   total             输出：符合条件的总数（不受分页影响）
 *   返回 0 成功，-1 出错
 */
int db_product_list(const char *kw, const char *cat, const char *brand,
                    const char *sort, int page, int size,
                    product_t *arr, int max, int *n, int *total);
int db_product_get(long id, product_t *p);

/* 商品图集（详情页缩略图条） */
int db_product_images(long id, char (*urls)[PRODUCT_IMG_LEN], int max, int *n);

/* 分类及其商品数，按数量降序 */
int db_category_list(category_t *arr, int max, int *n);

/* 购物车 */
int db_cart_list(long uid, cart_item_t *arr, int max, int *n, int *total);
int db_cart_add(long uid, long pid, int qty, int *err);   /* err: 1 商品不存在 2 库存不足 */
int db_cart_set(long uid, long pid, int qty, int *err);
int db_cart_remove(long uid, long pid);
int db_cart_clear(long uid);

/* 订单 */
int db_order_create(long uid, long *order_id, int *total, int *err); /* err: 1 购物车空 2 库存不足 */
int db_order_list(long uid, order_t *arr, int max, int *n);
int db_order_items(long oid, cart_item_t *arr, int max, int *n);

#endif /* JIANWU_DB_H */
