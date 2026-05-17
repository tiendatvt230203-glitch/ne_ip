CC = gcc
CLANG = clang
AR = ar

# Thêm -I/usr/local/include để nhận diện header mới
CFLAGS = -D_GNU_SOURCE -Iinc -I/usr/local/include -Wall -O2 $(shell pg_config --includedir 2>/dev/null | xargs -I{} echo -I{})

# Quan trọng: Thêm -L/usr/local/lib để Linker tìm thấy libxdp/libbpf mới của Server 1
LDFLAGS = -L/usr/local/lib -lxdp -lbpf -lpthread -lssl -lcrypto -lpq

BPF_CFLAGS = -O2 -target bpf -g
KERNEL_HEADERS = /usr/include

BIN_DIR = bin

APP_SRC = main.c src/core/main_diag.c src/core/interface.c src/core/forwarder.c src/core/wan_arp.c src/crypto/crypto_policy_utils.c src/crypto/crypto_dispatch.c src/crypto/packet_crypto.c src/crypto/crypto_layer2.c src/crypto/crypto_layer3.c src/crypto/crypto_layer4.c src/core/flow_table.c src/core/fragment.c
APP_OBJ = $(APP_SRC:.c=.o)
TARGET = $(BIN_DIR)/network-encryptor
DB_LIB_SRC = src/db/config.c src/db/db_config.c src/db/db_env.c src/db/db_runtime.c
DB_LIB_OBJ = $(DB_LIB_SRC:.c=.o)
DB_LIB = $(BIN_DIR)/libdb_loader.a

BPF_SRC = bpf/xdp_redirect.c bpf/xdp_wan_redirect.c
BPF_OBJ = bpf/xdp_redirect.o bpf/xdp_wan_redirect.o

TOOLS_BIN = $(BIN_DIR)/ne_ssh_probe $(BIN_DIR)/ne_send_udp

.PHONY: all clean run dirs tools db-init db-verify db-load db-notify

# Database (reads /opt/db.env — POSTGRES_SERVER/PORT/USER/DB/PASSWORD, user sep)
db-init:
	sh/xdp_init_db.sh

db-verify:
	sh/ne_db_verify.sh

db-load:
	@test -n "$(ID)" || (echo "Usage: make db-load ID=30" >&2; exit 1)
	sh/xdp_load_option.sh $(ID)

db-notify:
	@test -n "$(ID)" || (echo "Usage: make db-notify ID=30" >&2; exit 1)
	sh/ne_notify_start.sh $(ID)

all: dirs $(BPF_OBJ) $(DB_LIB) $(TARGET) tools

tools: $(TOOLS_BIN)

$(BIN_DIR)/ne_ssh_probe: tools/ne_ssh_probe.c
	$(CC) $(CFLAGS) -o $@ tools/ne_ssh_probe.c

$(BIN_DIR)/ne_send_udp: tools/ne_send_udp.c
	$(CC) $(CFLAGS) -o $@ tools/ne_send_udp.c

dirs:
	@mkdir -p $(BIN_DIR)

# Sửa lại thứ tự: File .o đứng trước LDFLAGS để Linker hoạt động chuẩn
$(DB_LIB): $(DB_LIB_OBJ)
	$(AR) rcs $@ $(DB_LIB_OBJ)

$(TARGET): $(APP_OBJ) $(DB_LIB)
	$(CC) -o $@ $(APP_OBJ) $(DB_LIB) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Bổ sung -I/usr/local/include cho phần BPF để đồng bộ
bpf/%.o: bpf/%.c
	$(CLANG) $(BPF_CFLAGS) -I$(KERNEL_HEADERS) -I/usr/local/include -c $< -o $@

clean:
	rm -rf $(BIN_DIR) src/*.o src/core/*.o src/crypto/*.o src/db/*.o *.o $(BPF_OBJ)

run:
	sudo $(TARGET)
