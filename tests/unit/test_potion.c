/* Static tokenizer/pooling and checked-loader regressions using analytic tables. */
#include "test.h"
#include "torchlight/potion.h"
#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put_u32(unsigned char *bytes, uint32_t value) {
    for (size_t i = 0; i < 4; i++)
        bytes[i] = (unsigned char)(value >> (i * 8));
}
static size_t fixture(unsigned char *bytes) {
    const char *words[] = {"[PAD]", "[UNK]", "[CLS]", "[SEP]", "[MASK]", "hello", "world", "cafe",
                           "play",  "##ing", "+",     "中",    "str",    "##a",   "##ße"};
    size_t count = sizeof(words) / sizeof(words[0]), vocabulary = 0;
    for (size_t i = 0; i < count; i++)
        vocabulary += strlen(words[i]) + 1;
    memset(bytes, 0, 4096);
    memcpy(bytes, "TLSTAT01", 8);
    put_u32(bytes + 8, (uint32_t)count);
    put_u32(bytes + 12, 2);
    put_u32(bytes + 16, (uint32_t)vocabulary);
    memset(bytes + 20, '0', 168);
    unsigned char *offsets = bytes + 220, *text = offsets + (count + 1) * 4;
    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        put_u32(offsets + i * 4, (uint32_t)offset);
        memcpy(text + offset, words[i], strlen(words[i]) + 1);
        offset += strlen(words[i]) + 1;
    }
    put_u32(offsets + count * 4, (uint32_t)offset);
    unsigned char *weights = text + vocabulary;
    for (size_t i = 0; i < count; i++) {
        float values[] = {(float)(i + 1), 1};
        for (size_t j = 0; j < 2; j++) {
            uint32_t bits = 0;
            memcpy(&bits, values + j, sizeof(bits));
            put_u32(weights + (i * 2 + j) * 4, bits);
        }
    }
    size_t length = (size_t)(weights - bytes) + count * 8;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    CHECK(checksum != NULL);
    g_checksum_update(checksum, bytes + 220, (gssize)(length - 220));
    gsize size = 32;
    g_checksum_get_digest(checksum, bytes + 188, &size);
    g_checksum_free(checksum);
    return length;
}
static void check_encoding(tl_embedder *model, const char *text, float mean) {
    float values[2];
    CHECK(embedder_encode(model, EMBED_QUERY, text, values, 2) == TL_OK);
    CHECK(fabsf(values[0] - mean / sqrtf(mean * mean + 1)) < 1e-6F);
    CHECK(fabsf(values[1] - 1 / sqrtf(mean * mean + 1)) < 1e-6F);
}
void test_potion(void) {
    unsigned char bytes[4096];
    size_t length = fixture(bytes);
    char path[] = "/tmp/torchlight-potion-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && write(fd, bytes, length) == (ssize_t)length && close(fd) == 0);
    tl_embedder *model = NULL;
    CHECK(potion_load(path, POTION_MODEL_BYTES, &model) == TL_OK);
    check_encoding(model, "HELLO world", 6.5F);
    check_encoding(model, "Café cafe\xcc\x81", 8);
    check_encoding(model, "playing", 9.5F);
    check_encoding(model, "hello\xe2\x80\xa8world", 6.5F);
    float unknown[2] = {1, 1};
    CHECK(embedder_encode(model, EMBED_QUERY, "hello\x01world", unknown, 2) == TL_STATE);
    CHECK(unknown[0] == 0 && unknown[1] == 0);
    check_encoding(model, "hello[CLS]world", 16.0F / 3.0F);
    check_encoding(model, "hello[UNK]world", 6.5F);
    char long_word[102];
    memset(long_word, 'a', 101);
    long_word[101] = 0;
    CHECK(embedder_encode(model, EMBED_QUERY, long_word, unknown, 2) == TL_STATE);
    embedder_destroy(model);
    CHECK(potion_load(path, 1, &model) == TL_LIMIT && model == NULL);
    bytes[length - 1] ^= 1;
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL && fwrite(bytes, 1, length, file) == length && fclose(file) == 0);
    CHECK(potion_load(path, POTION_MODEL_BYTES, &model) == TL_STATE && model == NULL);
    CHECK(unlink(path) == 0);
    CHECK(potion_load(path, POTION_MODEL_BYTES, &model) == TL_IO && model == NULL);
    char prepared[128];
    CHECK(potion_prepare_path("/home/demo/Work/productRoadmap_2026.md", prepared,
                              sizeof(prepared)) == TL_OK);
    CHECK(strcmp(prepared, "demo Work product Roadmap 2026 md") == 0);
    CHECK(potion_prepare_path("/raw/\xff"
                              "name.txt",
                              prepared, sizeof(prepared)) == TL_OK);
    CHECK(strcmp(prepared, " raw  name txt") == 0);
    CHECK(potion_prepare_path("/long/file", prepared, 3) == TL_LIMIT && prepared[0] == 0);
}
