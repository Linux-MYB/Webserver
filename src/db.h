#ifndef JIANWU_DB_H
#define JIANWU_DB_H

#include <stddef.h>

#define PRODUCT_IMG_MAX 8
#define PRODUCT_IMG_LEN 192

typedef struct {
    long id;
    char name[128];
    char subtitle[160];
    char emoji[16];
    int  price;
    int  market_price;
    int  stock;
    char category[40];
    char brand[40];
    char sn[24];
    char thumb[PRODUCT_IMG_LEN];
    char image[PRODUCT_IMG_LEN];
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

void db_seed(const char *shop_db);

int db_user_create(const char *account, const char *hash, const char *nickname, long *uid);

int db_user_auth(const char *account, const char *password, long *uid, char *nick, size_t nicklen);
int db_user_info(long uid, char *account, size_t alen, char *nick, size_t nlen);

int db_session_new(long uid, char *token, size_t tokensz);
int db_session_uid(const char *token, long *uid);
void db_session_del(const char *token);

int db_product_list(const char *kw, const char *cat, const char *brand,
                    const char *sort, int page, int size,
                    product_t *arr, int max, int *n, int *total);
int db_product_get(long id, product_t *p);

int db_product_images(long id, char (*urls)[PRODUCT_IMG_LEN], int max, int *n);

int db_category_list(category_t *arr, int max, int *n);

int db_cart_list(long uid, cart_item_t *arr, int max, int *n, int *total);
int db_cart_add(long uid, long pid, int qty, int *err);
int db_cart_set(long uid, long pid, int qty, int *err);
int db_cart_remove(long uid, long pid);
int db_cart_clear(long uid);

int db_order_create(long uid, long *order_id, int *total, int *err);
int db_order_list(long uid, order_t *arr, int max, int *n);
int db_order_items(long oid, cart_item_t *arr, int max, int *n);

#endif
