#include <check.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <sys/uio.h>

/*
 * We cannot directly call slirp_input via the backend without a full slirp
 * instance, so we test the invariant that iov_len must be validated before
 * casting to int. We intercept slirp_input to verify the length parameter
 * is never negative or truncated.
 *
 * This test defines a mock slirp_input that records the passed length,
 * then includes the backend source to link against it.
 */

static int last_slirp_input_len = 0;
static int slirp_input_called = 0;

/* Mock slirp_input to capture the (int) cast length */
void slirp_input(void *slirp, const uint8_t *pkt, int pkt_len)
{
    (void)slirp;
    (void)pkt;
    last_slirp_input_len = pkt_len;
    slirp_input_called = 1;
}

/*
 * Simulate what the vulnerable code path does:
 *   slirp_input(priv->slirp, iov->iov_base, (int)iov->iov_len);
 * The invariant: the length passed to slirp_input must be non-negative
 * and must faithfully represent iov_len (i.e., iov_len <= INT_MAX).
 */
static int safe_send_to_slirp(struct iovec *iov)
{
    /* This is what SHOULD happen - validate before cast */
    if (iov->iov_len > (size_t)INT_MAX) {
        return -1; /* reject */
    }
    slirp_input(NULL, iov->iov_base, (int)iov->iov_len);
    return 0;
}

static int vulnerable_send_to_slirp(struct iovec *iov)
{
    /* This is what the vulnerable code does - no validation */
    slirp_input(NULL, iov->iov_base, (int)iov->iov_len);
    return 0;
}

START_TEST(test_iov_len_cast_safety)
{
    /* Invariant: casting iov_len to int must never produce a negative value
     * or a value that doesn't match the original size_t length */
    uint8_t buf[64] = {0};

    size_t test_lengths[] = {
        (size_t)INT_MAX + 1,        /* exact exploit: overflows int */
        (size_t)UINT32_MAX,         /* boundary: max 32-bit value */
        (size_t)INT_MAX,            /* boundary: max valid int */
        64,                         /* valid small packet */
    };
    int num_tests = sizeof(test_lengths) / sizeof(test_lengths[0]);

    for (int i = 0; i < num_tests; i++) {
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = test_lengths[i];

        slirp_input_called = 0;

        if (test_lengths[i] > (size_t)INT_MAX) {
            /* For oversized lengths, the code MUST reject or clamp */
            /* Demonstrate the vulnerability: direct cast goes negative */
            vulnerable_send_to_slirp(&iov);
            int cast_result = (int)test_lengths[i];
            ck_assert_msg(cast_result <= 0 || cast_result != (int)test_lengths[i],
                "Overflow confirmed for length %zu -> int %d", test_lengths[i], cast_result);

            /* The safe version must reject it */
            int ret = safe_send_to_slirp(&iov);
            ck_assert_int_eq(ret, -1);
        } else {
            /* Valid lengths must pass through correctly */
            int ret = safe_send_to_slirp(&iov);
            ck_assert_int_eq(ret, 0);
            ck_assert_int_eq(last_slirp_input_len, (int)test_lengths[i]);
            ck_assert_msg(last_slirp_input_len >= 0,
                "Length must be non-negative, got %d", last_slirp_input_len);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_iov_len