# =========================================================
#  简物 server
#
#    make            编译
#    make assets     把商品图片拷到 www/images（只需跑一次）
#    make run        编译并运行（8080 端口）
#    make clean
#
#  依赖：libsqlite3-dev
#    Ubuntu/Debian: sudo apt install libsqlite3-dev
# =========================================================

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -pthread
LDLIBS  ?= -lsqlite3 -lpthread

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:.c=.o)
DEPS := $(SRCS:.c=.d)
TARGET := jianwu

# 商品素材：123.db 里的 goods_img 是 images/200905/... 这样的相对路径，
# 拷到 www/images 之后，前端直接用 /images/... 访问。
SHOP_DB := data/123.db
SRC_IMG := data/images
DST_IMG := www/images

all: $(TARGET) assets

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

assets:
	@if [ -d "$(SRC_IMG)" ] && [ ! -d "$(DST_IMG)" ]; then \
		echo "拷贝商品图片 → $(DST_IMG)"; \
		cp -r "$(SRC_IMG)" "$(DST_IMG)"; \
	else \
		echo "商品图片已就绪（或源目录不存在），跳过"; \
	fi

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

.PHONY: all assets run clean

-include $(DEPS)
