#include <time.h>

#include "pub/serial.h"
#include "crypto/aes.h"

#include "encoding.h"
#include "vmess.h"

// search for the timestamp the request is generated
uint64_t
_vmess_decode_lookup_timestamp(vmess_config_t *config, const hash128_t valid_code)
{
    uint64_t cur_time = time(NULL);
    uint64_t delta = config->time_delta;
    uint64_t t;

    hash128_t test;
    
    for (t = cur_time - delta; t <= cur_time + delta; t++) {
        vmess_gen_validation_code(config->user_id, t, test);

        if (memcmp(test, valid_code, sizeof(test)) == 0) {
            // found a match
            return t;
        }
    }

    return (uint64_t)-1;
}

ssize_t
vmess_decode_data(vmess_config_t *config, uint64_t gen_time,
                  const byte_t *data, size_t size,
                  byte_t **result, size_t *res_size)
{
    serial_t ser;
    hash128_t key, iv;

    uint16_t data_len;

    byte_t *data_buf;
    byte_t *data_dec;

    uint32_t checksum_expect, checksum;
    size_t n_read;

    vmess_gen_key(config, key);
    vmess_gen_iv(config, gen_time, iv);

    serial_init(&ser, (byte_t *)data, size, 1);

    if (!serial_read(&ser, &data_len, sizeof(data_len))) {
        serial_destroy(&ser);
        return 0;
    }

    data_len = from_be16(data_len);

    printf("data_len: %d\n", data_len);

    if (!data_len) {
        *result = NULL;
        *res_size = 0;
        n_read = serial_read_idx(&ser);
        serial_destroy(&ser);
        return n_read;
    }

    if (data_len < 4) {
        // corrupted packet
        serial_destroy(&ser);
        return -1;
    }

    data_buf = malloc(data_len);
    ASSERT(data_buf, "out of mem");

    if (!serial_read(&ser, data_buf, data_len)) {
        serial_destroy(&ser);
        free(data_buf);
        return 0;
    }

    data_dec = crypto_aes_128_cfb_dec(key, iv, data_buf, data_len, NULL);
    free(data_buf);

    if (!data_dec) return -1;

    checksum_expect = *(uint32_t *)data_dec;
    checksum = be32(crypto_fnv1a(data_dec + 4, data_len - 4));

    if (checksum != checksum_expect) {
        serial_destroy(&ser);
        free(data_dec);
        return -1;
    }

    *result = malloc(data_len - 4);
    ASSERT(*result, "out of mem");

    memcpy(*result, data_dec + 4, data_len - 4);
    *res_size = data_len - 4;
    n_read = serial_read_idx(&ser);

    serial_destroy(&ser);
    free(data_dec);

    return n_read;
}

ssize_t // number of decoded bytes
vmess_decode_request(vmess_config_t *config, vmess_request_t *req,
                     const byte_t *data, size_t size)
{
    serial_t ser;
    hash128_t valid_code;

    byte_t vers;
    byte_t nonce;
    byte_t opt;

    byte_t p_sec;
    byte_t resv;
    byte_t cmd;
    byte_t addr_type;
    byte_t len;

    uint16_t port;

    uint8_t ipv4[4];
    uint8_t ipv6[16];

    uint64_t gen_time;

    target_id_t *target = NULL;
    char *domain = NULL;

    uint32_t checksum, checksum_expect;

    size_t cmd_size;
    byte_t *cmd_buf = NULL;

    size_t n_read;

    hash128_t key, iv;
    hash128_t key_check, iv_check;

#define CLEARUP() \
    do { \
        serial_destroy(&ser); \
        target_id_free(target); \
    } while (0)

#define DECODE_FAIL(ret) \
    do { \
        CLEARUP(); \
        free(domain); \
        return (ret); \
    } while (0)

    serial_init(&ser, (byte_t *)data, size, 1);

    if (!serial_read(&ser, valid_code, sizeof(valid_code))) DECODE_FAIL(0);

    gen_time = _vmess_decode_lookup_timestamp(config, valid_code);

    // wrong validation code
    if (gen_time == -1) DECODE_FAIL(-1);

    vmess_gen_key(config, key);
    vmess_gen_iv(config, gen_time, iv);

    cmd_buf = serial_ofs(&ser, sizeof(valid_code));
    cmd_buf = crypto_aes_128_cfb_dec(key, iv, cmd_buf, serial_size(&ser) - sizeof(valid_code), NULL);
    
    if (!cmd_buf) DECODE_FAIL(0);

    // reinit again with new decrypted command data
    serial_destroy(&ser);
    serial_init(&ser, cmd_buf, size - sizeof(valid_code), 0);

    // hexdump("decoded", cmd_buf, size - sizeof(valid_code));

    if (!serial_read(&ser, &vers, sizeof(vers))) DECODE_FAIL(0);
    if (!serial_read(&ser, &iv_check, sizeof(iv_check))) DECODE_FAIL(0);
    if (!serial_read(&ser, &key_check, sizeof(key_check))) DECODE_FAIL(0);

    if (!serial_read(&ser, &nonce, sizeof(nonce))) DECODE_FAIL(0);
    if (!serial_read(&ser, &opt, sizeof(opt))) DECODE_FAIL(0);
    if (!serial_read(&ser, &p_sec, sizeof(p_sec))) DECODE_FAIL(0);
    if (!serial_read(&ser, &resv, sizeof(resv))) DECODE_FAIL(0);
    if (!serial_read(&ser, &cmd, sizeof(cmd))) DECODE_FAIL(0);

    if (!serial_read(&ser, &port, sizeof(port))) DECODE_FAIL(0);
    port = from_be16(port);

    if (!serial_read(&ser, &addr_type, sizeof(addr_type))) DECODE_FAIL(0);

    switch (addr_type) {
        case ADDR_TYPE_IPV4:
            if (!serial_read(&ser, ipv4, sizeof(ipv4))) DECODE_FAIL(0);
            target = target_id_new_ipv4(ipv4, port);
            break;

        case ADDR_TYPE_DOMAIN:
            if (!serial_read(&ser, &len, sizeof(len))) DECODE_FAIL(0);

            domain = malloc(len + 1);
            ASSERT(domain, "out of mem");

            if (!serial_read(&ser, domain, len)) DECODE_FAIL(0);
            target = target_id_new_domain(domain, port);

            free(domain);
            domain = NULL;

            break;

        case ADDR_TYPE_IPV6:
            if (!serial_read(&ser, ipv6, sizeof(ipv6))) DECODE_FAIL(0);
            target = target_id_new_ipv6(ipv6, port);
            break;

        default: DECODE_FAIL(0);
    }

    // read off p bytes of empty value
    if (!serial_read(&ser, NULL, p_sec >> 4)) DECODE_FAIL(0);

    if (!serial_read(&ser, &checksum, sizeof(checksum))) DECODE_FAIL(0);

    cmd_size = serial_read_idx(&ser) - sizeof(checksum);
    cmd_buf = serial_ofs(&ser, 0);
    checksum_expect = be32(crypto_fnv1a(cmd_buf, cmd_size));

    if (checksum != checksum_expect) DECODE_FAIL(-1);

    n_read = serial_read_idx(&ser) + sizeof(valid_code);

    serial_destroy(&ser);

    // req = malloc(sizeof(*req));
    // ASSERT(req, "out of mem");

    req->target = target;
    req->gen_time = gen_time;
    req->vers = vers;
    req->crypt = p_sec & 0xf;
    req->cmd = cmd;
    req->nonce = nonce;
    req->opt = opt;

    return n_read;
}
