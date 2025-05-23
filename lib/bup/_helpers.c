#define _LARGEFILE64_SOURCE 1
#define PY_SSIZE_T_CLEAN 1
#undef NDEBUG
#include "../../config/config.h"

// According to Python, its header has to go first:
//   http://docs.python.org/3/c-api/intro.html#include-files
#include <Python.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_TM_TM_GMTOFF
#include <time.h>
#endif

#if defined(BUP_RL_EXPECTED_XOPEN_SOURCE) \
    && (!defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < BUP_RL_EXPECTED_XOPEN_SOURCE)
# warning "_XOPEN_SOURCE version is incorrect for readline"
#endif

#ifdef BUP_HAVE_READLINE
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wstrict-prototypes"
# ifdef BUP_READLINE_INCLUDES_IN_SUBDIR
#   include <readline/readline.h>
#   include <readline/history.h>
# else
#   include <readline.h>
#   include <history.h>
# endif
# pragma GCC diagnostic pop
#endif

#include "bup.h"
#include "bup/intprops.h"
#include "bup/pyutil.h"
#include "bupsplit.h"
#include "_hashsplit.h"

#if defined(FS_IOC_GETFLAGS) && defined(FS_IOC_SETFLAGS)
#define BUP_HAVE_FILE_ATTRS 1
#endif

#ifndef FS_NOCOW_FL
// Of course, this assumes it's a bitfield value.
#define FS_NOCOW_FL 0
#endif


typedef unsigned char byte;


typedef struct {
    int istty2;
} state_t;

// cstr_argf: for byte vectors without null characters (e.g. paths)
// rbuf_argf: for read-only byte vectors
// wbuf_argf: for mutable byte vectors

#define get_state(x) ((state_t *) PyModule_GetState(x))
#define cstr_argf "y"
#define rbuf_argf "y#"
#define wbuf_argf "y*"

#ifndef htonll
// This function should technically be macro'd out if it's going to be used
// more than ocasionally.  As of this writing, it'll actually never be called
// in real world bup scenarios (because our packs are < MAX_INT bytes).
static uint64_t htonll(uint64_t value)
{
    static const int endian_test = 42;

    if (*(char *)&endian_test == endian_test) // LSB-MSB
	return ((uint64_t)htonl(value & 0xFFFFFFFF) << 32) | htonl(value >> 32);
    return value; // already in network byte order MSB-LSB
}
#endif

#define INTEGRAL_ASSIGNMENT_FITS(dest, src) INT_ADD_OK(src, 0, dest)


static PyObject *bup_bytescmp(PyObject *self, PyObject *args)
{
    PyObject *py_s1, *py_s2;  // This is really a PyBytes/PyString
    if (!PyArg_ParseTuple(args, "SS", &py_s1, &py_s2))
	return NULL;
    char *s1, *s2;
    Py_ssize_t s1_len, s2_len;
    if (PyBytes_AsStringAndSize(py_s1, &s1, &s1_len) == -1)
        return NULL;
    if (PyBytes_AsStringAndSize(py_s2, &s2, &s2_len) == -1)
        return NULL;
    const Py_ssize_t n = (s1_len < s2_len) ? s1_len : s2_len;
    const int cmp = memcmp(s1, s2, n);
    if (cmp != 0)
        return PyLong_FromLong(cmp);
    if (s1_len == s2_len)
        return PyLong_FromLong(0);;
    return PyLong_FromLong((s1_len < s2_len) ? -1 : 1);
}


static int write_all(int fd, const void *buf, const size_t count)
{
    size_t written = 0;
    while (written < count)
    {
        const ssize_t rc = write(fd, (char *) buf + written, count - written);
        if (rc == -1)
            return -1;
        written += rc;
    }
    return 0;
}


static inline int uadd(unsigned long long *dest,
                       const unsigned long long x,
                       const unsigned long long y)
{
    return INT_ADD_OK(x, y, dest);
}


static PyObject *append_sparse_region(const int fd, unsigned long long n)
{
    while (n)
    {
        off_t new_off;
        if (!INTEGRAL_ASSIGNMENT_FITS(&new_off, n))
            new_off = INT_MAX;
        const off_t off = lseek(fd, new_off, SEEK_CUR);
        if (off == (off_t) -1)
            return PyErr_SetFromErrno(PyExc_IOError);
        n -= new_off;
    }
    return NULL;
}


static PyObject *record_sparse_zeros(unsigned long long *new_pending,
                                     const int fd,
                                     unsigned long long prev_pending,
                                     const unsigned long long count)
{
    // Add count additional sparse zeros to prev_pending and store the
    // result in new_pending, or if the total won't fit in
    // new_pending, write some of the zeros to fd sparsely, and store
    // the remaining sum in new_pending.
    if (!uadd(new_pending, prev_pending, count))
    {
        PyObject *err = append_sparse_region(fd, prev_pending);
        if (err != NULL)
            return err;
        *new_pending = count;
    }
    return NULL;
}


static byte* find_not_zero(const byte * const start, const byte * const end)
{
    // Return a pointer to first non-zero byte between start and end,
    // or end if there isn't one.
    assert(start <= end);
    const unsigned char *cur = start;
    while (cur < end && *cur == 0)
        cur++;
    return (byte *) cur;
}


static byte* find_trailing_zeros(const byte * const start,
                                 const byte * const end)
{
    // Return a pointer to the start of any trailing run of zeros, or
    // end if there isn't one.
    assert(start <= end);
    if (start == end)
        return (byte *) end;
    const byte * cur = end;
    while (cur > start && *--cur == 0) {}
    if (*cur == 0)
        return (byte *) cur;
    else
        return (byte *) (cur + 1);
}


static byte *find_non_sparse_end(const byte * const start,
                                 const byte * const end,
                                 const ptrdiff_t min_len)
{
    // Return the first pointer to a min_len sparse block in [start,
    // end) if there is one, otherwise a pointer to the start of any
    // trailing run of zeros.  If there are no trailing zeros, return
    // end.
    if (start == end)
        return (byte *) end;
    assert(start < end);
    assert(min_len);
    // Probe in min_len jumps, searching backward from the jump
    // destination for a non-zero byte.  If such a byte is found, move
    // just past it and try again.
    const byte *candidate = start;
    // End of any run of zeros, starting at candidate, that we've already seen
    const byte *end_of_known_zeros = candidate;
    while (end - candidate >= min_len) // Handle all min_len candidate blocks
    {
        const byte * const probe_end = candidate + min_len;
        const byte * const trailing_zeros =
            find_trailing_zeros(end_of_known_zeros, probe_end);
        if (trailing_zeros == probe_end)
            end_of_known_zeros = candidate = probe_end;
        else if (trailing_zeros == end_of_known_zeros)
        {
            assert(candidate >= start);
            assert(candidate <= end);
            assert(*candidate == 0);
            return (byte *) candidate;
        }
        else
        {
            candidate = trailing_zeros;
            end_of_known_zeros = probe_end;
        }
    }

    if (candidate == end)
        return (byte *) end;

    // No min_len sparse run found, search backward from end
    const byte * const trailing_zeros = find_trailing_zeros(end_of_known_zeros,
                                                            end);

    if (trailing_zeros == end_of_known_zeros)
    {
        assert(candidate >= start);
        assert(candidate < end);
        assert(*candidate == 0);
        assert(end - candidate < min_len);
        return (byte *) candidate;
    }

    if (trailing_zeros == end)
    {
        assert(*(end - 1) != 0);
        return (byte *) end;
    }

    assert(end - trailing_zeros < min_len);
    assert(trailing_zeros >= start);
    assert(trailing_zeros < end);
    assert(*trailing_zeros == 0);
    return (byte *) trailing_zeros;
}


static PyObject *bup_write_sparsely(PyObject *self, PyObject *args)
{
    int fd;
    unsigned char *buf = NULL;
    Py_ssize_t sbuf_len;
    PyObject *py_min_sparse_len, *py_prev_sparse_len;
    if (!PyArg_ParseTuple(args, "i" rbuf_argf "OO",
                          &fd, &buf, &sbuf_len,
                          &py_min_sparse_len, &py_prev_sparse_len))
	return NULL;
    ptrdiff_t min_sparse_len;
    unsigned long long prev_sparse_len, buf_len, ul_min_sparse_len;
    if (!bup_ullong_from_py(&ul_min_sparse_len, py_min_sparse_len, "min_sparse_len"))
        return NULL;
    if (!INTEGRAL_ASSIGNMENT_FITS(&min_sparse_len, ul_min_sparse_len))
        return PyErr_Format(PyExc_OverflowError, "min_sparse_len too large");
    if (!bup_ullong_from_py(&prev_sparse_len, py_prev_sparse_len, "prev_sparse_len"))
        return NULL;
    if (sbuf_len < 0)
        return PyErr_Format(PyExc_ValueError, "negative bufer length");
    if (!INTEGRAL_ASSIGNMENT_FITS(&buf_len, sbuf_len))
        return PyErr_Format(PyExc_OverflowError, "buffer length too large");

    const byte * block = buf; // Start of pending block
    const byte * const end = buf + buf_len;
    unsigned long long zeros = prev_sparse_len;
    while (1)
    {
        assert(block <= end);
        if (block == end)
            return PyLong_FromUnsignedLongLong(zeros);

        if (*block != 0)
        {
            // Look for the end of block, i.e. the next sparse run of
            // at least min_sparse_len zeros, or the end of the
            // buffer.
            const byte * const probe = find_non_sparse_end(block + 1, end,
                                                           min_sparse_len);
            // Either at end of block, or end of non-sparse; write pending data
            PyObject *err = append_sparse_region(fd, zeros);
            if (err != NULL)
                return err;
            int rc = write_all(fd, block, probe - block);
            if (rc)
                return PyErr_SetFromErrno(PyExc_IOError);

            if (end - probe < min_sparse_len)
                zeros = end - probe;
            else
                zeros = min_sparse_len;
            block = probe + zeros;
        }
        else // *block == 0
        {
            // Should be in the first loop iteration, a sparse run of
            // zeros, or nearly at the end of the block (within
            // min_sparse_len).
            const byte * const zeros_end = find_not_zero(block, end);
            PyObject *err = record_sparse_zeros(&zeros, fd,
                                                zeros, zeros_end - block);
            if (err != NULL)
                return err;
            assert(block <= zeros_end);
            block = zeros_end;
        }
    }
}


static PyObject *selftest(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
	return NULL;
    
    return Py_BuildValue("i", !bupsplit_selftest());
}


static PyObject *rollsum(PyObject *self, PyObject *args)
{
    Py_buffer buf;
    if (!PyArg_ParseTuple(args, "y*", &buf))
        return NULL;

    const unsigned long sum = rollsum_sum(buf.buf, 0, buf.len);
    PyBuffer_Release(&buf);
    return Py_BuildValue("k", sum);
}


static PyObject *bitmatch(PyObject *self, PyObject *args)
{
    unsigned char *buf1 = NULL, *buf2 = NULL;
    Py_ssize_t len1 = 0, len2 = 0;
    Py_ssize_t byte;
    int bit;

    if (!PyArg_ParseTuple(args, rbuf_argf rbuf_argf, &buf1, &len1, &buf2, &len2))
	return NULL;
    
    bit = 0;
    for (byte = 0; byte < len1 && byte < len2; byte++)
    {
	int b1 = buf1[byte], b2 = buf2[byte];
	if (b1 != b2)
	{
	    for (bit = 0; bit < 8; bit++)
		if ( (b1 & (0x80 >> bit)) != (b2 & (0x80 >> bit)) )
		    break;
	    break;
	}
    }
    
    Py_ssize_t result;
    if (!INT_MULTIPLY_OK(byte, 8, &result)
        || !INT_ADD_OK(result, bit, &result))
    {
        PyErr_Format(PyExc_OverflowError, "bitmatch bit count too large");
        return NULL;
    }
    return PyLong_FromSsize_t(result);
}


static PyObject *firstword(PyObject *self, PyObject *args)
{
    unsigned char *buf = NULL;
    Py_ssize_t len = 0;
    uint32_t v;

    if (!PyArg_ParseTuple(args, rbuf_argf, &buf, &len))
	return NULL;
    
    if (len < 4)
	return NULL;
    
    v = ntohl(*(uint32_t *)buf);
    return PyLong_FromUnsignedLong(v);
}


#define BLOOM2_HEADERLEN 16

static void to_bloom_address_bitmask4(const unsigned char *buf,
	const int nbits, uint64_t *v, unsigned char *bitmask)
{
    int bit;
    uint32_t high;
    uint64_t raw, mask;

    memcpy(&high, buf, 4);
    mask = (1<<nbits) - 1;
    raw = (((uint64_t)ntohl(high) << 8) | buf[4]);
    bit = (raw >> (37-nbits)) & 0x7;
    *v = (raw >> (40-nbits)) & mask;
    *bitmask = 1 << bit;
}

static void to_bloom_address_bitmask5(const unsigned char *buf,
	const int nbits, uint32_t *v, unsigned char *bitmask)
{
    int bit;
    uint32_t high;
    uint32_t raw, mask;

    memcpy(&high, buf, 4);
    mask = (1<<nbits) - 1;
    raw = ntohl(high);
    bit = (raw >> (29-nbits)) & 0x7;
    *v = (raw >> (32-nbits)) & mask;
    *bitmask = 1 << bit;
}

#define BLOOM_SET_BIT(name, address, otype) \
static void name(unsigned char *bloom, const unsigned char *buf, const int nbits)\
{\
    unsigned char bitmask;\
    otype v;\
    address(buf, nbits, &v, &bitmask);\
    bloom[BLOOM2_HEADERLEN+v] |= bitmask;\
}
BLOOM_SET_BIT(bloom_set_bit4, to_bloom_address_bitmask4, uint64_t)
BLOOM_SET_BIT(bloom_set_bit5, to_bloom_address_bitmask5, uint32_t)


#define BLOOM_GET_BIT(name, address, otype) \
static int name(const unsigned char *bloom, const unsigned char *buf, const int nbits)\
{\
    unsigned char bitmask;\
    otype v;\
    address(buf, nbits, &v, &bitmask);\
    return bloom[BLOOM2_HEADERLEN+v] & bitmask;\
}
BLOOM_GET_BIT(bloom_get_bit4, to_bloom_address_bitmask4, uint64_t)
BLOOM_GET_BIT(bloom_get_bit5, to_bloom_address_bitmask5, uint32_t)


static PyObject *bloom_add(PyObject *self, PyObject *args)
{
    Py_buffer bloom, sha;
    int nbits = 0, k = 0;
    if (!PyArg_ParseTuple(args, wbuf_argf wbuf_argf "ii",
                          &bloom, &sha, &nbits, &k))
        return NULL;

    PyObject *result = NULL;

    if (bloom.len < 16+(1<<nbits) || sha.len % 20 != 0)
        goto clean_and_return;

    if (k == 5)
    {
        if (nbits > 29)
            goto clean_and_return;
        unsigned char *cur = sha.buf;
        unsigned char *end;
        for (end = cur + sha.len; cur < end; cur += 20/k)
            bloom_set_bit5(bloom.buf, cur, nbits);
    }
    else if (k == 4)
    {
        if (nbits > 37)
            goto clean_and_return;
        unsigned char *cur = sha.buf;
        unsigned char *end = cur + sha.len;
        for (; cur < end; cur += 20/k)
            bloom_set_bit4(bloom.buf, cur, nbits);
    }
    else
        goto clean_and_return;

    result = Py_BuildValue("n", sha.len / 20);

 clean_and_return:
    PyBuffer_Release(&bloom);
    PyBuffer_Release(&sha);
    return result;
}

static PyObject *bloom_contains(PyObject *self, PyObject *args)
{
    Py_buffer bloom;
    unsigned char *sha = NULL;
    Py_ssize_t len = 0;
    int nbits = 0, k = 0;
    if (!PyArg_ParseTuple(args, wbuf_argf rbuf_argf "ii",
                          &bloom, &sha, &len, &nbits, &k))
        return NULL;

    PyObject *result = NULL;

    if (len != 20)
        goto clean_and_return;

    if (k == 5)
    {
        if (nbits > 29)
            goto clean_and_return;
        int steps;
        unsigned char *end;
        for (steps = 1, end = sha + 20; sha < end; sha += 20/k, steps++)
            if (!bloom_get_bit5(bloom.buf, sha, nbits))
            {
                result = Py_BuildValue("Oi", Py_None, steps);
                goto clean_and_return;
            }
    }
    else if (k == 4)
    {
        if (nbits > 37)
            goto clean_and_return;
        int steps;
        unsigned char *end;
        for (steps = 1, end = sha + 20; sha < end; sha += 20/k, steps++)
            if (!bloom_get_bit4(bloom.buf, sha, nbits))
            {
                result = Py_BuildValue("Oi", Py_None, steps);
                goto clean_and_return;
            }
    }
    else
        goto clean_and_return;

    result = Py_BuildValue("ii", 1, k);

 clean_and_return:
    PyBuffer_Release(&bloom);
    return result;
}


static uint32_t _extract_bits(unsigned char *buf, int nbits)
{
    uint32_t v, mask;

    mask = (1<<nbits) - 1;
    v = ntohl(*(uint32_t *)buf);
    v = (v >> (32-nbits)) & mask;
    return v;
}


static PyObject *extract_bits(PyObject *self, PyObject *args)
{
    unsigned char *buf = NULL;
    Py_ssize_t len = 0;
    int nbits = 0;

    if (!PyArg_ParseTuple(args, rbuf_argf "i", &buf, &len, &nbits))
	return NULL;
    
    if (len < 4)
	return NULL;
    
    return PyLong_FromUnsignedLong(_extract_bits(buf, nbits));
}


struct sha {
    unsigned char bytes[20];
};

static inline int _cmp_sha(const struct sha *sha1, const struct sha *sha2)
{
    return memcmp(sha1->bytes, sha2->bytes, sizeof(sha1->bytes));
}


struct idx {
    unsigned char *map;
    struct sha *cur;
    struct sha *end;
    uint32_t *cur_name;
    Py_ssize_t bytes;
    int name_base;
};

static void _fix_idx_order(struct idx **idxs, Py_ssize_t *last_i)
{
    struct idx *idx;
    Py_ssize_t low, mid, high;
    int c = 0;

    idx = idxs[*last_i];
    if (idxs[*last_i]->cur >= idxs[*last_i]->end)
    {
	idxs[*last_i] = NULL;
	PyMem_Free(idx);
	--*last_i;
	return;
    }
    if (*last_i == 0)
	return;

    low = *last_i-1;
    mid = *last_i;
    high = 0;
    while (low >= high)
    {
	mid = (low + high) / 2;
	c = _cmp_sha(idx->cur, idxs[mid]->cur);
	if (c < 0)
	    high = mid + 1;
	else if (c > 0)
	    low = mid - 1;
	else
	    break;
    }
    if (c < 0)
	++mid;
    if (mid == *last_i)
	return;
    memmove(&idxs[mid+1], &idxs[mid], (*last_i-mid)*sizeof(struct idx *));
    idxs[mid] = idx;
}


static uint32_t _get_idx_i(struct idx *idx)
{
    if (idx->cur_name == NULL)
	return idx->name_base;
    return ntohl(*idx->cur_name) + idx->name_base;
}

#define MIDX4_HEADERLEN 12

static PyObject *merge_into(PyObject *self, PyObject *args)
{
    struct sha *sha_ptr, *sha_start = NULL;
    uint32_t *table_ptr, *name_ptr, *name_start;
    int i;
    unsigned int total;
    uint32_t count, prefix;


    Py_buffer fmap;
    int bits;;
    PyObject *py_total, *ilist = NULL;
    if (!PyArg_ParseTuple(args, wbuf_argf "iOO",
                          &fmap, &bits, &py_total, &ilist))
	return NULL;

    PyObject *result = NULL;
    struct idx **idxs = NULL;
    Py_ssize_t num_i = 0;
    int *idx_buf_init = NULL;
    Py_buffer *idx_buf = NULL;

    if (!bup_uint_from_py(&total, py_total, "total"))
        goto clean_and_return;

    num_i = PyList_Size(ilist);

    if (!(idxs = checked_malloc(num_i, sizeof(struct idx *))))
        goto clean_and_return;
    if (!(idx_buf_init = checked_calloc(num_i, sizeof(int))))
        goto clean_and_return;
    if (!(idx_buf = checked_malloc(num_i, sizeof(Py_buffer))))
        goto clean_and_return;

    for (i = 0; i < num_i; i++)
    {
	long len, sha_ofs, name_map_ofs;
	if (!(idxs[i] = checked_malloc(1, sizeof(struct idx))))
            goto clean_and_return;
	PyObject *itup = PyList_GetItem(ilist, i);
	if (!PyArg_ParseTuple(itup, wbuf_argf "llli",
                              &(idx_buf[i]), &len, &sha_ofs, &name_map_ofs,
                              &idxs[i]->name_base))
	    return NULL;
        idx_buf_init[i] = 1;
        idxs[i]->map = idx_buf[i].buf;
        idxs[i]->bytes = idx_buf[i].len;
	idxs[i]->cur = (struct sha *)&idxs[i]->map[sha_ofs];
	idxs[i]->end = &idxs[i]->cur[len];
	if (name_map_ofs)
	    idxs[i]->cur_name = (uint32_t *)&idxs[i]->map[name_map_ofs];
	else
	    idxs[i]->cur_name = NULL;
    }
    table_ptr = (uint32_t *) &((unsigned char *) fmap.buf)[MIDX4_HEADERLEN];
    sha_start = sha_ptr = (struct sha *)&table_ptr[1<<bits];
    name_start = name_ptr = (uint32_t *)&sha_ptr[total];

    Py_ssize_t last_i = num_i - 1;
    count = 0;
    prefix = 0;
    while (last_i >= 0)
    {
	struct idx *idx;
	uint32_t new_prefix;
	if (count % 102424 == 0 && get_state(self)->istty2)
	    fprintf(stderr, "midx: writing %.2f%% (%d/%d)\r",
		    count*100.0/total, count, total);
	idx = idxs[last_i];
	new_prefix = _extract_bits((unsigned char *)idx->cur, bits);
	while (prefix < new_prefix)
	    table_ptr[prefix++] = htonl(count);
	memcpy(sha_ptr++, idx->cur, sizeof(struct sha));
	*name_ptr++ = htonl(_get_idx_i(idx));
	++idx->cur;
	if (idx->cur_name != NULL)
	    ++idx->cur_name;
	_fix_idx_order(idxs, &last_i);
	++count;
    }
    while (prefix < ((uint32_t) 1 << bits))
	table_ptr[prefix++] = htonl(count);
    assert(count == total);
    assert(prefix == ((uint32_t) 1 << bits));
    assert(sha_ptr == sha_start+count);
    assert(name_ptr == name_start+count);

    result = PyLong_FromUnsignedLong(count);

 clean_and_return:
    if (idx_buf_init)
    {
        for (i = 0; i < num_i; i++)
            if (idx_buf_init[i])
                PyBuffer_Release(&(idx_buf[i]));
        free(idx_buf_init);
        free(idx_buf);
    }
    if (idxs)
    {
        for (i = 0; i < num_i; i++)
            free(idxs[i]);
        free(idxs);
    }
    PyBuffer_Release(&fmap);
    return result;
}

#define FAN_ENTRIES 256

static PyObject *write_idx(PyObject *self, PyObject *args)
{
    char *filename = NULL;
    PyObject *py_total, *idx = NULL;
    PyObject *part;
    unsigned int total = 0;
    uint32_t count;
    int i;
    uint32_t *fan_ptr, *crc_ptr, *ofs_ptr;
    uint64_t *ofs64_ptr;
    struct sha *sha_ptr;

    Py_buffer fmap;
    if (!PyArg_ParseTuple(args, cstr_argf wbuf_argf "OO",
                          &filename, &fmap, &idx, &py_total))
	return NULL;

    PyObject *result = NULL;

    if (!bup_uint_from_py(&total, py_total, "total"))
        goto clean_and_return;

    if (PyList_Size (idx) != FAN_ENTRIES) // Check for list of the right length.
    {
        result = PyErr_Format (PyExc_TypeError, "idx must contain %d entries",
                               FAN_ENTRIES);
        goto clean_and_return;
    }

    const char idx_header[] = "\377tOc\0\0\0\002";
    memcpy (fmap.buf, idx_header, sizeof(idx_header) - 1);

    fan_ptr = (uint32_t *)&((unsigned char *)fmap.buf)[sizeof(idx_header) - 1];
    sha_ptr = (struct sha *)&fan_ptr[FAN_ENTRIES];
    crc_ptr = (uint32_t *)&sha_ptr[total];
    ofs_ptr = (uint32_t *)&crc_ptr[total];
    ofs64_ptr = (uint64_t *)&ofs_ptr[total];

    count = 0;
    uint32_t ofs64_count = 0;
    for (i = 0; i < FAN_ENTRIES; ++i)
    {
	part = PyList_GET_ITEM(idx, i);
	PyList_Sort(part);
        uint32_t plen;
        if (!INTEGRAL_ASSIGNMENT_FITS(&plen, PyList_GET_SIZE(part))
            || UINT32_MAX - count < plen) {
            PyErr_Format(PyExc_OverflowError, "too many objects in index part");
            goto clean_and_return;
        }
        count += plen;
	*fan_ptr++ = htonl(count);
        uint32_t j;
        for (j = 0; j < plen; ++j)
	{
	    unsigned char *sha = NULL;
	    Py_ssize_t sha_len = 0;
            PyObject *crc_py, *ofs_py;
	    unsigned int crc;
            unsigned PY_LONG_LONG ofs_ull;
	    uint64_t ofs;
	    if (!PyArg_ParseTuple(PyList_GET_ITEM(part, j), rbuf_argf "OO",
				  &sha, &sha_len, &crc_py, &ofs_py))
                goto clean_and_return;
            if(!bup_uint_from_py(&crc, crc_py, "crc"))
                goto clean_and_return;
            if(!bup_ullong_from_py(&ofs_ull, ofs_py, "ofs"))
                goto clean_and_return;
            assert(crc <= UINT32_MAX);
            assert(ofs_ull <= UINT64_MAX);
	    ofs = ofs_ull;
	    if (sha_len != sizeof(struct sha))
                goto clean_and_return;
	    memcpy(sha_ptr++, sha, sizeof(struct sha));
	    *crc_ptr++ = htonl(crc);
	    if (ofs > 0x7fffffff)
	    {
                *ofs64_ptr++ = htonll(ofs);
		ofs = 0x80000000 | ofs64_count++;
	    }
	    *ofs_ptr++ = htonl((uint32_t)ofs);
	}
    }

    int rc = msync(fmap.buf, fmap.len, MS_ASYNC);
    if (rc != 0)
    {
        result = PyErr_SetFromErrnoWithFilename(PyExc_IOError, filename);
        goto clean_and_return;
    }

    result = PyLong_FromUnsignedLong(count);

 clean_and_return:
    PyBuffer_Release(&fmap);
    return result;
}


// I would have made this a lower-level function that just fills in a buffer
// with random values, and then written those values from python.  But that's
// about 20% slower in my tests, and since we typically generate random
// numbers for benchmarking other parts of bup, any slowness in generating
// random bytes will make our benchmarks inaccurate.  Plus nobody wants
// pseudorandom bytes much except for this anyway.
static PyObject *write_random(PyObject *self, PyObject *args)
{
    uint32_t buf[1024/4];
    int fd = -1, seed = 0, verbose = 0;
    ssize_t ret;
    long long len = 0, kbytes = 0, written = 0;

    if (!PyArg_ParseTuple(args, "iLii", &fd, &len, &seed, &verbose))
	return NULL;
    
    srandom(seed);
    
    for (kbytes = 0; kbytes < len/1024; kbytes++)
    {
	unsigned i;
	for (i = 0; i < sizeof(buf)/sizeof(buf[0]); i++)
	    buf[i] = (uint32_t) random();
	ret = write(fd, buf, sizeof(buf));
	if (ret < 0)
	    ret = 0;
	written += ret;
	if (ret < (int)sizeof(buf))
	    break;
	if (verbose && kbytes/1024 > 0 && !(kbytes%1024))
	    fprintf(stderr, "Random: %lld Mbytes\r", kbytes/1024);
    }
    
    // handle non-multiples of 1024
    if (len % 1024)
    {
	unsigned i;
	for (i = 0; i < sizeof(buf)/sizeof(buf[0]); i++)
	    buf[i] = (uint32_t) random();
	ret = write(fd, buf, len % 1024);
	if (ret < 0)
	    ret = 0;
	written += ret;
    }
    
    if (kbytes/1024 > 0)
	fprintf(stderr, "Random: %lld Mbytes, done.\n", kbytes/1024);
    return Py_BuildValue("L", written);
}


static PyObject *random_sha(PyObject *self, PyObject *args)
{
    static int seeded = 0;
    uint32_t shabuf[20/4];
    int i;
    
    if (!seeded)
    {
	assert(sizeof(shabuf) == 20);
	srandom((unsigned int) time(NULL));
	seeded = 1;
    }
    
    if (!PyArg_ParseTuple(args, ""))
	return NULL;
    
    memset(shabuf, 0, sizeof(shabuf));
    for (i=0; i < 20/4; i++)
	shabuf[i] = (uint32_t) random();
    return Py_BuildValue(rbuf_argf, shabuf, 20);
}


static int _open_noatime(const char *filename, int attrs)
{
    int attrs_noatime, fd;
    attrs |= O_RDONLY;
#ifdef O_NOFOLLOW
    attrs |= O_NOFOLLOW;
#endif
#ifdef O_LARGEFILE
    attrs |= O_LARGEFILE;
#endif
    attrs_noatime = attrs;
#ifdef O_NOATIME
    attrs_noatime |= O_NOATIME;
#endif
    fd = open(filename, attrs_noatime);
    if (fd < 0 && errno == EPERM)
    {
	// older Linux kernels would return EPERM if you used O_NOATIME
	// and weren't the file's owner.  This pointless restriction was
	// relaxed eventually, but we have to handle it anyway.
	// (VERY old kernels didn't recognized O_NOATIME, but they would
	// just harmlessly ignore it, so this branch won't trigger)
	fd = open(filename, attrs);
    }
    return fd;
}


static PyObject *open_noatime(PyObject *self, PyObject *args)
{
    char *filename = NULL;
    int fd;
    if (!PyArg_ParseTuple(args, cstr_argf, &filename))
	return NULL;
    fd = _open_noatime(filename, 0);
    if (fd < 0)
	return PyErr_SetFromErrnoWithFilename(PyExc_OSError, filename);
    return Py_BuildValue("i", fd);
}


// Currently the Linux kernel and FUSE disagree over the type for
// FS_IOC_GETFLAGS and FS_IOC_SETFLAGS.  The kernel actually uses int,
// but FUSE chose long (matching the declaration in linux/fs.h).  So
// if you use int, and then traverse a FUSE filesystem, you may
// corrupt the stack.  But if you use long, then you may get invalid
// results on big-endian systems.
//
// For now, we just use long, and then disable Linux attrs entirely
// (with a warning) in helpers.py on systems that are affected.

#ifdef BUP_HAVE_FILE_ATTRS
static PyObject *bup_get_linux_file_attr(PyObject *self, PyObject *args)
{
    int rc;
    unsigned long attr;
    char *path;
    int fd;

    if (!PyArg_ParseTuple(args, cstr_argf, &path))
        return NULL;

    fd = _open_noatime(path, O_NONBLOCK);
    if (fd == -1)
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);

    attr = 0;  // Handle int/long mismatch (see above)
    rc = ioctl(fd, FS_IOC_GETFLAGS, &attr);
    if (rc == -1)
    {
        close(fd);
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
    }
    close(fd);
    assert(attr <= UINT_MAX);  // Kernel type is actually int
    return PyLong_FromUnsignedLong(attr);
}
#endif /* def BUP_HAVE_FILE_ATTRS */



#ifdef BUP_HAVE_FILE_ATTRS
static PyObject *bup_set_linux_file_attr(PyObject *self, PyObject *args)
{
    int rc;
    unsigned long orig_attr;
    unsigned int attr;
    char *path;
    PyObject *py_attr;
    int fd;

    if (!PyArg_ParseTuple(args, cstr_argf "O", &path, &py_attr))
        return NULL;

    if (!bup_uint_from_py(&attr, py_attr, "attr"))
        return NULL;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_LARGEFILE | O_NOFOLLOW);
    if (fd == -1)
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);

    // Restrict attr to modifiable flags acdeijstuADST -- see
    // chattr(1) and the e2fsprogs source.  Letter to flag mapping is
    // in pf.c flags_array[].
    attr &= FS_APPEND_FL | FS_COMPR_FL | FS_NODUMP_FL | FS_EXTENT_FL
    | FS_IMMUTABLE_FL | FS_JOURNAL_DATA_FL | FS_SECRM_FL | FS_NOTAIL_FL
    | FS_UNRM_FL | FS_NOATIME_FL | FS_DIRSYNC_FL | FS_SYNC_FL
    | FS_TOPDIR_FL | FS_NOCOW_FL;

    // The extents flag can't be removed, so don't (see chattr(1) and chattr.c).
    orig_attr = 0; // Handle int/long mismatch (see above)
    rc = ioctl(fd, FS_IOC_GETFLAGS, &orig_attr);
    if (rc == -1)
    {
        close(fd);
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
    }
    assert(orig_attr <= UINT_MAX);  // Kernel type is actually int
    attr |= ((unsigned int) orig_attr) & FS_EXTENT_FL;

    rc = ioctl(fd, FS_IOC_SETFLAGS, &attr);
    if (rc == -1)
    {
        close(fd);
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
    }

    close(fd);
    return Py_BuildValue("O", Py_None);
}
#endif /* def BUP_HAVE_FILE_ATTRS */


#ifdef BUP_STAT_NS_FLAVOR_TIM
# define BUP_STAT_ATIME_NS(st) (st)->st_atim.tv_nsec
# define BUP_STAT_MTIME_NS(st) (st)->st_mtim.tv_nsec
# define BUP_STAT_CTIME_NS(st) (st)->st_ctim.tv_nsec
#elif defined BUP_STAT_NS_FLAVOR_TIMENSEC
# define BUP_STAT_ATIME_NS(st) (st)->st_atimensec.tv_nsec
# define BUP_STAT_MTIME_NS(st) (st)->st_mtimensec.tv_nsec
# define BUP_STAT_CTIME_NS(st) (st)->st_ctimensec.tv_nsec
#elif defined BUP_STAT_NS_FLAVOR_TIMESPEC
# define BUP_STAT_ATIME_NS(st) (st)->st_atimespec.tv_nsec
# define BUP_STAT_MTIME_NS(st) (st)->st_mtimespec.tv_nsec
# define BUP_STAT_CTIME_NS(st) (st)->st_ctimespec.tv_nsec
#elif defined BUP_STAT_NS_FLAVOR_NONE
# define BUP_STAT_ATIME_NS(st) 0
# define BUP_STAT_MTIME_NS(st) 0
# define BUP_STAT_CTIME_NS(st) 0
#else
# error "./configure did not define a BUP_STAT_NS_FLAVOR"
#endif


static PyObject *stat_struct_to_py(const struct stat *st,
                                   const char *filename,
                                   int fd)
{
    // We can check the known (via POSIX) signed and unsigned types at
    // compile time, but not (easily) the unspecified types, so handle
    // those via BUP_LONGISH_TO_PY().  Assumes ns values will fit in a
    // long.
    return Py_BuildValue("NKNNNNNL(Nl)(Nl)(Nl)",
                         BUP_LONGISH_TO_PY(st->st_mode),
                         (unsigned PY_LONG_LONG) st->st_ino,
                         BUP_LONGISH_TO_PY(st->st_dev),
                         BUP_LONGISH_TO_PY(st->st_nlink),
                         BUP_LONGISH_TO_PY(st->st_uid),
                         BUP_LONGISH_TO_PY(st->st_gid),
                         BUP_LONGISH_TO_PY(st->st_rdev),
                         (PY_LONG_LONG) st->st_size,
                         BUP_LONGISH_TO_PY(st->st_atime),
                         (long) BUP_STAT_ATIME_NS(st),
                         BUP_LONGISH_TO_PY(st->st_mtime),
                         (long) BUP_STAT_MTIME_NS(st),
                         BUP_LONGISH_TO_PY(st->st_ctime),
                         (long) BUP_STAT_CTIME_NS(st));
}


static PyObject *bup_stat(PyObject *self, PyObject *args)
{
    int rc;
    char *filename;

    if (!PyArg_ParseTuple(args, cstr_argf, &filename))
        return NULL;

    struct stat st;
    rc = stat(filename, &st);
    if (rc != 0)
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, filename);
    return stat_struct_to_py(&st, filename, 0);
}


static PyObject *bup_lstat(PyObject *self, PyObject *args)
{
    int rc;
    char *filename;

    if (!PyArg_ParseTuple(args, cstr_argf, &filename))
        return NULL;

    struct stat st;
    rc = lstat(filename, &st);
    if (rc != 0)
        return PyErr_SetFromErrnoWithFilename(PyExc_OSError, filename);
    return stat_struct_to_py(&st, filename, 0);
}


static PyObject *bup_fstat(PyObject *self, PyObject *args)
{
    int rc, fd;

    if (!PyArg_ParseTuple(args, "i", &fd))
        return NULL;

    struct stat st;
    rc = fstat(fd, &st);
    if (rc != 0)
        return PyErr_SetFromErrno(PyExc_OSError);
    return stat_struct_to_py(&st, NULL, fd);
}


#ifdef HAVE_TM_TM_GMTOFF
static PyObject *bup_localtime(PyObject *self, PyObject *args)
{
    long long lltime;
    time_t ttime;
    if (!PyArg_ParseTuple(args, "L", &lltime))
	return NULL;
    if (!INTEGRAL_ASSIGNMENT_FITS(&ttime, lltime))
        return PyErr_Format(PyExc_OverflowError, "time value too large");

    struct tm tm;
    tzset();
    if(localtime_r(&ttime, &tm) == NULL)
        return PyErr_SetFromErrno(PyExc_OSError);

    // Match the Python struct_time values.
    return Py_BuildValue("[i,i,i,i,i,i,i,i,i,i,s]",
                         1900 + tm.tm_year, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec,
                         tm.tm_wday, tm.tm_yday + 1,
                         tm.tm_isdst, tm.tm_gmtoff, tm.tm_zone);
}
#endif /* def HAVE_TM_TM_GMTOFF */


static unsigned int vuint_encode(long long val, char *buf)
{
    unsigned int len = 0;

    if (val < 0) {
        PyErr_SetString(PyExc_Exception, "vuints must not be negative");
        return 0;
    }

    do {
        buf[len] = val & 0x7f;

        val >>= 7;
        if (val)
            buf[len] |= 0x80;

        len++;
    } while (val);

    return len;
}

static unsigned int vint_encode(long long val, char *buf)
{
    unsigned int len = 1;
    char sign = 0;

    if (val < 0) {
        sign = 0x40;
        val = -val;
    }

    buf[0] = (val & 0x3f) | sign;
    val >>= 6;
    if (val)
        buf[0] |= 0x80;

    while (val) {
        buf[len] = val & 0x7f;
        val >>= 7;
        if (val)
            buf[len] |= 0x80;
        len++;
    }

    return len;
}

static PyObject *bup_vuint_encode(PyObject *self, PyObject *args)
{
    long long val;
    // size the buffer appropriately - need 8 bits to encode each 7
    char buf[(sizeof(val) + 1) / 7 * 8];

    if (!PyArg_ParseTuple(args, "L", &val))
	return NULL;

    unsigned int len = vuint_encode(val, buf);
    if (!len)
        return NULL;

    return PyBytes_FromStringAndSize(buf, len);
}

static PyObject *bup_vint_encode(PyObject *self, PyObject *args)
{
    long long val;
    // size the buffer appropriately - need 8 bits to encode each 7
    char buf[(sizeof(val) + 1) / 7 * 8];

    if (!PyArg_ParseTuple(args, "L", &val))
	return NULL;

    return PyBytes_FromStringAndSize(buf, vint_encode(val, buf));
}

static PyObject *tuple_from_cstrs(char **cstrs)
{
    // Assumes list is null terminated
    size_t n = 0;
    while(cstrs[n] != NULL)
        n++;

    Py_ssize_t sn;
    if (!INTEGRAL_ASSIGNMENT_FITS(&sn, n))
        return PyErr_Format(PyExc_OverflowError, "string array too large");

    PyObject *result = PyTuple_New(sn);
    Py_ssize_t i = 0;
    for (i = 0; i < sn; i++)
    {
        PyObject *gname = Py_BuildValue(cstr_argf, cstrs[i]);
        if (gname == NULL)
        {
            Py_DECREF(result);
            return NULL;
        }
        PyTuple_SET_ITEM(result, i, gname);
    }
    return result;
}

static PyObject *appropriate_errno_ex(void)
{
    switch (errno) {
    case ENOMEM:
        return PyErr_NoMemory();
    case EIO:
    case EMFILE:
    case ENFILE:
        // In 3.3 IOError was merged into OSError.
        return PyErr_SetFromErrno(PyExc_IOError);
    default:
        return PyErr_SetFromErrno(PyExc_OSError);
    }
}


static PyObject *pwd_struct_to_py(const struct passwd *pwd)
{
    // We can check the known (via POSIX) signed and unsigned types at
    // compile time, but not (easily) the unspecified types, so handle
    // those via BUP_LONGISH_TO_PY().
    if (pwd == NULL)
        Py_RETURN_NONE;
    return Py_BuildValue(cstr_argf cstr_argf "OO"
                         cstr_argf cstr_argf cstr_argf,
                         pwd->pw_name,
                         pwd->pw_passwd,
                         BUP_LONGISH_TO_PY(pwd->pw_uid),
                         BUP_LONGISH_TO_PY(pwd->pw_gid),
                         pwd->pw_gecos,
                         pwd->pw_dir,
                         pwd->pw_shell);
}

static PyObject *bup_getpwuid(PyObject *self, PyObject *args)
{
    PyObject *py_uid = NULL;
    if (!PyArg_ParseTuple(args, "O", &py_uid))
	return NULL;
    uid_t uid;
    int overflow;
    if (!BUP_ASSIGN_PYLONG_TO_INTEGRAL(&uid, py_uid, &overflow)) {
        if (overflow)
            return PyErr_Format(PyExc_OverflowError, "uid too large for uid_t");
        return NULL;
    }
    errno = 0;
    struct passwd *pwd = getpwuid(uid);
    if (!pwd && errno)
        return appropriate_errno_ex();
    return pwd_struct_to_py(pwd);
}

static PyObject *bup_getpwnam(PyObject *self, PyObject *args)
{
    PyObject *py_name;
    if (!PyArg_ParseTuple(args, "S", &py_name))
	return NULL;

    char *name = PyBytes_AS_STRING(py_name);
    errno = 0;
    struct passwd *pwd = getpwnam(name);
    if (!pwd && errno)
        return appropriate_errno_ex();
    return pwd_struct_to_py(pwd);
}

static PyObject *grp_struct_to_py(const struct group *grp)
{
    // We can check the known (via POSIX) signed and unsigned types at
    // compile time, but not (easily) the unspecified types, so handle
    // those via BUP_LONGISH_TO_PY().
    if (grp == NULL)
        Py_RETURN_NONE;

    PyObject *members = tuple_from_cstrs(grp->gr_mem);
    if (members == NULL)
        return NULL;
    return Py_BuildValue(cstr_argf cstr_argf "OO",
                         grp->gr_name,
                         grp->gr_passwd,
                         BUP_LONGISH_TO_PY(grp->gr_gid),
                         members);
}

static PyObject *bup_getgrgid(PyObject *self, PyObject *args)
{
    PyObject *py_gid = NULL;
    if (!PyArg_ParseTuple(args, "O", &py_gid))
	return NULL;
    gid_t gid;
    int overflow;
    if (!BUP_ASSIGN_PYLONG_TO_INTEGRAL(&gid, py_gid, &overflow)) {
        if (overflow)
            return PyErr_Format(PyExc_OverflowError, "gid too large for gid_t");
        return NULL;
    }

    errno = 0;
    struct group *grp = getgrgid(gid);
    if (!grp && errno)
        return appropriate_errno_ex();
    return grp_struct_to_py(grp);
}

static PyObject *bup_getgrnam(PyObject *self, PyObject *args)
{
    PyObject *py_name;
    if (!PyArg_ParseTuple(args, "S", &py_name))
	return NULL;

    char *name = PyBytes_AS_STRING(py_name);
    errno = 0;
    struct group *grp = getgrnam(name);
    if (!grp && errno)
        return appropriate_errno_ex();
    return grp_struct_to_py(grp);
}


static PyObject *bup_gethostname(PyObject *mod, PyObject *ignore)
{
#ifdef HOST_NAME_MAX
    char buf[HOST_NAME_MAX + 1] = {};
#else
    /* 'SUSv2 guarantees that "Host names are limited to 255 bytes".' */
    char buf[256] = {};
#endif

    if (gethostname(buf, sizeof(buf) - 1))
        return PyErr_SetFromErrno(PyExc_IOError);
    buf[sizeof(buf) - 1] = 0;
    return PyBytes_FromString(buf);
}


#ifdef BUP_HAVE_READLINE

static char *cstr_from_bytes(PyObject *bytes)
{
    char *buf;
    Py_ssize_t length;
    int rc = PyBytes_AsStringAndSize(bytes, &buf, &length);
    if (rc == -1)
        return NULL;
    size_t c_len;
    if (!INT_ADD_OK(length, 1, &c_len)) {
        PyErr_Format(PyExc_OverflowError,
                     "Cannot convert ssize_t sized bytes object (%zd) to C string",
                     length);
        return NULL;
    }
    char *result = checked_malloc(c_len, sizeof(char));
    if (!result)
        return NULL;
    memcpy(result, buf, length);
    result[length] = 0;
    return result;
}

static char **cstrs_from_seq(PyObject *seq)
{
    char **result = NULL;
    seq = PySequence_Fast(seq, "Cannot convert sequence items to C strings");
    if (!seq)
        return NULL;

    const Py_ssize_t len = PySequence_Fast_GET_SIZE(seq);
    if (len > PY_SSIZE_T_MAX - 1) {
        PyErr_Format(PyExc_OverflowError,
                     "Sequence length %zd too large for conversion to C array",
                     len);
        goto finish;
    }
    result = checked_malloc(len + 1, sizeof(char *));
    if (!result)
        goto finish;
    Py_ssize_t i = 0;
    for (i = 0; i < len; i++)
    {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        if (!item)
            goto abandon_result;
        result[i] = cstr_from_bytes(item);
        if (!result[i]) {
            i--;
            goto abandon_result;
        }
    }
    result[len] = NULL;
    goto finish;

 abandon_result:
    if (result) {
        for (; i > 0; i--)
            free(result[i]);
        free(result);
        result = NULL;
    }
 finish:
    Py_DECREF(seq);
    return result;
}

static char* our_word_break_chars = NULL;

static PyObject *
bup_set_completer_word_break_characters(PyObject *self, PyObject *args)
{
    char *bytes;
    if (!PyArg_ParseTuple(args, cstr_argf, &bytes))
	return NULL;
    char *prev = our_word_break_chars;
    char *next = strdup(bytes);
    if (!next)
        return PyErr_NoMemory();
    our_word_break_chars = next;
    rl_completer_word_break_characters = next;
    if (prev)
        free(prev);
    Py_RETURN_NONE;
}

static PyObject *
bup_get_completer_word_break_characters(PyObject *self, PyObject *args)
{
    return PyBytes_FromString(rl_completer_word_break_characters);
}

static PyObject *bup_get_line_buffer(PyObject *self, PyObject *args)
{
    return PyBytes_FromString(rl_line_buffer);
}

static PyObject *
bup_parse_and_bind(PyObject *self, PyObject *args)
{
    char *bytes;
    if (!PyArg_ParseTuple(args, cstr_argf ":parse_and_bind", &bytes))
	return NULL;
    char *tmp = strdup(bytes); // Because it may modify the arg
    if (!tmp)
        return PyErr_NoMemory();
    int rc = rl_parse_and_bind(tmp);
    free(tmp);
    if (rc != 0)
        return PyErr_Format(PyExc_OSError,
                            "system rl_parse_and_bind failed (%d)", rc);
    Py_RETURN_NONE;
}


static PyObject *py_on_attempted_completion;
static char **prev_completions;

static char **on_attempted_completion(const char *text, int start, int end)
{
    if (!py_on_attempted_completion)
        return NULL;

    char **result = NULL;
    PyObject *py_result = PyObject_CallFunction(py_on_attempted_completion,
                                                cstr_argf "ii",
                                                text, start, end);
    if (!py_result)
        return NULL;
    if (py_result != Py_None) {
        result = cstrs_from_seq(py_result);
        free(prev_completions);
        prev_completions = result;
    }
    Py_DECREF(py_result);
    return result;
}

static PyObject *
bup_set_attempted_completion_function(PyObject *self, PyObject *args)
{
    PyObject *completer;
    if (!PyArg_ParseTuple(args, "O", &completer))
	return NULL;

    PyObject *prev = py_on_attempted_completion;
    if (completer == Py_None)
    {
        py_on_attempted_completion = NULL;
        rl_attempted_completion_function = NULL;
    } else {
        py_on_attempted_completion = completer;
        rl_attempted_completion_function = on_attempted_completion;
        Py_INCREF(completer);
    }
    Py_XDECREF(prev);
    Py_RETURN_NONE;
}


static PyObject *py_on_completion_entry;

static char *on_completion_entry(const char *text, int state)
{
    if (!py_on_completion_entry)
        return NULL;

    PyObject *py_result = PyObject_CallFunction(py_on_completion_entry,
                                                cstr_argf "i", text, state);
    if (!py_result)
        return NULL;
    char *result = (py_result == Py_None) ? NULL : cstr_from_bytes(py_result);
    Py_DECREF(py_result);
    return result;
}

static PyObject *
bup_set_completion_entry_function(PyObject *self, PyObject *args)
{
    PyObject *completer;
    if (!PyArg_ParseTuple(args, "O", &completer))
	return NULL;

    PyObject *prev = py_on_completion_entry;
    if (completer == Py_None) {
        py_on_completion_entry = NULL;
        rl_completion_entry_function = NULL;
    } else {
        py_on_completion_entry = completer;
        rl_completion_entry_function = on_completion_entry;
        Py_INCREF(completer);
    }
    Py_XDECREF(prev);
    Py_RETURN_NONE;
}

static PyObject *
bup_readline(PyObject *self, PyObject *args)
{
    char *prompt;
    if (!PyArg_ParseTuple(args, cstr_argf, &prompt))
	return NULL;
    char *line = readline(prompt);
    if (!line)
        return PyErr_Format(PyExc_EOFError, "readline EOF");
    PyObject *result = PyBytes_FromString(line);
    free(line);
    return result;
}

#endif // defined BUP_HAVE_READLINE

#ifdef BUP_HAVE_ACLS
#include <sys/acl.h>
#include <acl/libacl.h>

// Returns
//   0 for success
//  -1 for errors, with python exception set
//  -2 for ignored errors (not supported)
static int bup_read_acl_to_text(const char *name, acl_type_t type,
                                char **txt, char **num)
{
    acl_t acl;

    acl = acl_get_file(name, type);
    if (!acl) {
        if (errno == EOPNOTSUPP || errno == ENOSYS)
            return -2;
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }

    *num = NULL;
    *txt = acl_to_any_text(acl, "", ',', TEXT_ABBREVIATE);
    if (*txt)
        *num = acl_to_any_text(acl, "", ',', TEXT_ABBREVIATE | TEXT_NUMERIC_IDS);

    if (*txt && *num)
        return 0;

    if (errno == ENOMEM)
        PyErr_NoMemory();
    else
        PyErr_SetFromErrno(PyExc_IOError);

    if (*txt)
        acl_free((acl_t)*txt);
    if (*num)
        acl_free((acl_t)*num);

    return -1;
}

static PyObject *bup_read_acl(PyObject *self, PyObject *args)
{
    char *name;
    int isdir, rv;
    PyObject *ret = NULL;
    char *acl_txt = NULL, *acl_num = NULL;

    if (!PyArg_ParseTuple(args, cstr_argf "i", &name, &isdir))
	return NULL;

    if (!acl_extended_file(name))
        Py_RETURN_NONE;

    rv = bup_read_acl_to_text(name, ACL_TYPE_ACCESS, &acl_txt, &acl_num);
    if (rv)
        goto out;

    if (isdir) {
        char *def_txt = NULL, *def_num = NULL;

        rv = bup_read_acl_to_text(name, ACL_TYPE_DEFAULT, &def_txt, &def_num);
        if (rv)
            goto out;

        ret = Py_BuildValue("[" cstr_argf cstr_argf cstr_argf cstr_argf "]",
                            acl_txt, acl_num, def_txt, def_num);

        if (def_txt)
            acl_free((acl_t)def_txt);
        if (def_num)
            acl_free((acl_t)def_num);
    } else {
        ret = Py_BuildValue("[" cstr_argf cstr_argf "]",
                            acl_txt, acl_num);
    }

out:
    if (acl_txt)
        acl_free((acl_t)acl_txt);
    if (acl_num)
        acl_free((acl_t)acl_num);
    if (rv == -2)
        Py_RETURN_NONE;
    return ret;
}

static int
bup_apply_acl_string(const char *name, acl_type_t type, const char *s)
{
    acl_t acl = acl_from_text(s);
    int ret = 0;

    if (!acl) {
        PyErr_SetFromErrno(PyExc_IOError);
        return -1;
    }

    if (acl_set_file(name, type, acl)) {
        PyErr_SetFromErrno(PyExc_IOError);
        ret = -1;
    }

    acl_free(acl);

    return ret;
}

static PyObject *bup_apply_acl(PyObject *self, PyObject *args)
{
    char *name, *acl, *def = NULL;

    if (!PyArg_ParseTuple(args, cstr_argf cstr_argf "|" cstr_argf, &name, &acl, &def))
	return NULL;

    if (bup_apply_acl_string(name, ACL_TYPE_ACCESS, acl))
        return NULL;

    if (def && bup_apply_acl_string(name, ACL_TYPE_DEFAULT, def))
        return NULL;

    Py_RETURN_NONE;
}
#endif

static PyObject *bup_limited_vint_pack(PyObject *self, PyObject *args)
{
    const char *fmt;
    PyObject *packargs, *result;
    Py_ssize_t sz, i, bufsz;
    char *buf, *pos, *end;

    if (!PyArg_ParseTuple(args, "sO", &fmt, &packargs))
        return NULL;

    if (!PyTuple_Check(packargs))
        return PyErr_Format(PyExc_Exception, "pack() arg must be tuple");

    sz = PyTuple_GET_SIZE(packargs);
    if (sz != (Py_ssize_t)strlen(fmt))
        return PyErr_Format(PyExc_Exception,
                            "number of arguments (%ld) does not match format string (%ld)",
                            (unsigned long)sz, (unsigned long)strlen(fmt));

    if (sz > INT_MAX / 20)
        return PyErr_Format(PyExc_Exception, "format is far too long");

    // estimate no more than 20 bytes for each on average, the maximum
    // vint/vuint we can encode is anyway 10 bytes, so this gives us
    // some headroom for a few strings before we need to realloc ...
    bufsz = sz * 20;
    buf = malloc(bufsz);
    if (!buf)
        return PyErr_NoMemory();

    pos = buf;
    end = buf + bufsz;
    for (i = 0; i < sz; i++) {
        PyObject *item = PyTuple_GET_ITEM(packargs, i);
        const char *bytes;

        switch (fmt[i]) {
        case 'V': {
            long long val = PyLong_AsLongLong(item);
            if (val == -1 && PyErr_Occurred())
                return PyErr_Format(PyExc_OverflowError,
                                    "pack arg %d invalid", (int)i);
            if (end - pos < 10)
                goto overflow;
	    pos += vuint_encode(val, pos);
            break;
        }
        case 'v': {
            long long val = PyLong_AsLongLong(item);
            if (val == -1 && PyErr_Occurred())
                return PyErr_Format(PyExc_OverflowError,
                                    "pack arg %d invalid", (int)i);
            if (end - pos < 10)
                goto overflow;
            pos += vint_encode(val, pos);
            break;
        }
        case 's': {
            bytes = PyBytes_AsString(item);
            if (!bytes)
                goto error;
            if (end - pos < 10)
                goto overflow;
            Py_ssize_t val = PyBytes_GET_SIZE(item);
            pos += vuint_encode(val, pos);
            if (end - pos < val)
                goto overflow;
            memcpy(pos, bytes, val);
            pos += val;
            break;
        }
        default:
            PyErr_Format(PyExc_Exception, "unknown xpack format string item %c",
                         fmt[i]);
            goto error;
        }
    }

    result = PyBytes_FromStringAndSize(buf, pos - buf);
    free(buf);
    return result;

 overflow:
    PyErr_SetString(PyExc_OverflowError, "buffer (potentially) overflowed");
 error:
    free(buf);
    return NULL;
}

static PyMethodDef helper_methods[] = {
    { "write_sparsely", bup_write_sparsely, METH_VARARGS,
      "Write buf excepting zeros at the end. Return trailing zero count." },
    { "selftest", selftest, METH_VARARGS,
	"Check that the rolling checksum rolls correctly (for unit tests)." },
    { "rollsum", rollsum, METH_VARARGS,
       "Return the rolling checksum for the given string." },
    { "bitmatch", bitmatch, METH_VARARGS,
	"Count the number of matching prefix bits between two strings." },
    { "firstword", firstword, METH_VARARGS,
        "Return an int corresponding to the first 32 bits of buf." },
    { "bloom_contains", bloom_contains, METH_VARARGS,
	"Check if a bloom filter of 2^nbits bytes contains an object" },
    { "bloom_add", bloom_add, METH_VARARGS,
	"Add an object to a bloom filter of 2^nbits bytes" },
    { "extract_bits", extract_bits, METH_VARARGS,
	"Take the first 'nbits' bits from 'buf' and return them as an int." },
    { "merge_into", merge_into, METH_VARARGS,
	"Merges a bunch of idx and midx files into a single midx." },
    { "write_idx", write_idx, METH_VARARGS,
	"Write a PackIdxV2 file from an idx list of lists of tuples" },
    { "write_random", write_random, METH_VARARGS,
	"Write random bytes to the given file descriptor" },
    { "random_sha", random_sha, METH_VARARGS,
        "Return a random 20-byte string" },
    { "open_noatime", open_noatime, METH_VARARGS,
	"open() the given filename for read with O_NOATIME if possible" },
#ifdef BUP_HAVE_FILE_ATTRS
    { "get_linux_file_attr", bup_get_linux_file_attr, METH_VARARGS,
      "Return the Linux attributes for the given file." },
#endif
#ifdef BUP_HAVE_FILE_ATTRS
    { "set_linux_file_attr", bup_set_linux_file_attr, METH_VARARGS,
      "Set the Linux attributes for the given file." },
#endif
    { "stat", bup_stat, METH_VARARGS,
      "Extended version of stat." },
    { "lstat", bup_lstat, METH_VARARGS,
      "Extended version of lstat." },
    { "fstat", bup_fstat, METH_VARARGS,
      "Extended version of fstat." },
#ifdef HAVE_TM_TM_GMTOFF
    { "localtime", bup_localtime, METH_VARARGS,
      "Return struct_time elements plus the timezone offset and name." },
#endif
    { "bytescmp", bup_bytescmp, METH_VARARGS,
      "Return a negative value if x < y, zero if equal, positive otherwise."},
    { "getpwuid", bup_getpwuid, METH_VARARGS,
      "Return the password database entry for the given numeric user id,"
      " as a tuple with all C strings as bytes(), or None if the user does"
      " not exist." },
    { "getpwnam", bup_getpwnam, METH_VARARGS,
      "Return the password database entry for the given user name,"
      " as a tuple with all C strings as bytes(), or None if the user does"
      " not exist." },
    { "getgrgid", bup_getgrgid, METH_VARARGS,
      "Return the group database entry for the given numeric group id,"
      " as a tuple with all C strings as bytes(), or None if the group does"
      " not exist." },
    { "getgrnam", bup_getgrnam, METH_VARARGS,
      "Return the group database entry for the given group name,"
      " as a tuple with all C strings as bytes(), or None if the group does"
      " not exist." },
    { "gethostname", bup_gethostname, METH_NOARGS,
      "Return the current hostname (as bytes)" },
#ifdef BUP_HAVE_READLINE
    { "set_completion_entry_function", bup_set_completion_entry_function, METH_VARARGS,
      "Set rl_completion_entry_function.  Called as f(text, state)." },
    { "set_attempted_completion_function", bup_set_attempted_completion_function, METH_VARARGS,
      "Set rl_attempted_completion_function.  Called as f(text, start, end)." },
    { "parse_and_bind", bup_parse_and_bind, METH_VARARGS,
      "Call rl_parse_and_bind." },
    { "get_line_buffer", bup_get_line_buffer, METH_NOARGS,
      "Return rl_line_buffer." },
    { "get_completer_word_break_characters", bup_get_completer_word_break_characters, METH_NOARGS,
      "Return rl_completer_word_break_characters." },
    { "set_completer_word_break_characters", bup_set_completer_word_break_characters, METH_VARARGS,
      "Set rl_completer_word_break_characters." },
    { "readline", bup_readline, METH_VARARGS,
      "Call readline(prompt)." },
#endif // defined BUP_HAVE_READLINE
#ifdef BUP_HAVE_ACLS
    { "read_acl", bup_read_acl, METH_VARARGS,
      "read_acl(name, isdir)\n\n"
      "Read ACLs for the given file/dirname and return the correctly encoded"
      " list [txt, num, def_tx, def_num] (the def_* being empty bytestrings"
      " unless the second argument 'isdir' is True)." },
    { "apply_acl", bup_apply_acl, METH_VARARGS,
      "apply_acl(name, acl, def=None)\n\n"
      "Given a file/dirname (bytes) and the ACLs to restore, do that." },
#endif /* HAVE_ACLS */
    { "vuint_encode", bup_vuint_encode, METH_VARARGS, "encode an int to vuint" },
    { "vint_encode", bup_vint_encode, METH_VARARGS, "encode an int to vint" },
    { "limited_vint_pack", bup_limited_vint_pack, METH_VARARGS,
      "Try to pack vint/vuint/str, throwing OverflowError when unable." },
    { NULL, NULL, 0, NULL },  // sentinel
};

static void test_integral_assignment_fits(void)
{
    assert(sizeof(signed short) == sizeof(unsigned short));
    assert(sizeof(signed short) < sizeof(signed long long));
    assert(sizeof(signed short) < sizeof(unsigned long long));
    assert(sizeof(unsigned short) < sizeof(signed long long));
    assert(sizeof(unsigned short) < sizeof(unsigned long long));
    assert(sizeof(Py_ssize_t) <= sizeof(size_t));
    {
        signed short ss, ssmin = SHRT_MIN, ssmax = SHRT_MAX;
        unsigned short us, usmax = USHRT_MAX;
        signed long long sllmin = LLONG_MIN, sllmax = LLONG_MAX;
        unsigned long long ullmax = ULLONG_MAX;

        assert(INTEGRAL_ASSIGNMENT_FITS(&ss, ssmax));
        assert(INTEGRAL_ASSIGNMENT_FITS(&ss, ssmin));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&ss, usmax));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&ss, sllmin));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&ss, sllmax));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&ss, ullmax));

        assert(INTEGRAL_ASSIGNMENT_FITS(&us, usmax));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&us, ssmin));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&us, sllmin));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&us, sllmax));
        assert(!INTEGRAL_ASSIGNMENT_FITS(&us, ullmax));
    }
}

static int setup_module(PyObject *m)
{
    // FIXME: migrate these tests to configure, or at least don't
    // possibly crash the whole application.  Check against the type
    // we're going to use when passing to python.  Other stat types
    // are tested at runtime.
    assert(sizeof(ino_t) <= sizeof(unsigned PY_LONG_LONG));
    assert(sizeof(off_t) <= sizeof(PY_LONG_LONG));
    assert(sizeof(blksize_t) <= sizeof(PY_LONG_LONG));
    assert(sizeof(blkcnt_t) <= sizeof(PY_LONG_LONG));
    // Just be sure (relevant when passing timestamps back to Python above).
    assert(sizeof(PY_LONG_LONG) <= sizeof(long long));
    assert(sizeof(unsigned PY_LONG_LONG) <= sizeof(unsigned long long));
    // At least for BUP_LONGISH_TO_PY
    assert(sizeof(intmax_t) <= sizeof(long long));
    assert(sizeof(uintmax_t) <= sizeof(unsigned long long));
    // This should be guaranteed by the C standard, but it's cheap to
    // double-check, and we depend on it.
    assert(sizeof(unsigned long) >= sizeof(uint32_t));

    test_integral_assignment_fits();

    // Originally required by append_sparse_region()
    {
        off_t probe;
        if (!INTEGRAL_ASSIGNMENT_FITS(&probe, INT_MAX))
        {
            fprintf(stderr, "off_t can't hold INT_MAX; please report.\n");
            exit(BUP_EXIT_FAILURE);
        }
    }

    char *e;
    {
        PyObject *value;
        value = BUP_LONGISH_TO_PY(INT_MAX);
        PyObject_SetAttrString(m, "INT_MAX", value);
        Py_DECREF(value);
        value = BUP_LONGISH_TO_PY(UINT_MAX);
        PyObject_SetAttrString(m, "UINT_MAX", value);
        Py_DECREF(value);
    }

#ifdef BUP_HAVE_MINCORE_INCORE
    {
        PyObject *value;
        value = BUP_LONGISH_TO_PY(MINCORE_INCORE);
        PyObject_SetAttrString(m, "MINCORE_INCORE", value);
        Py_DECREF(value);
    }
#endif

    e = getenv("BUP_FORCE_TTY");
    get_state(m)->istty2 = isatty(2) || (atoi(e ? e : "0") & 2);
    return 1;
}


static struct PyModuleDef helpers_def = {
    PyModuleDef_HEAD_INIT,
    "_helpers",
    NULL,
    sizeof(state_t),
    helper_methods,
    NULL,
    NULL, // helpers_traverse,
    NULL, // helpers_clear,
    NULL
};

extern PyObject *PyInit__helpers(void);

PyMODINIT_FUNC PyInit__helpers(void)
{
    PyObject *module;

    if (hashsplit_init())
        return NULL;

    module = PyModule_Create(&helpers_def);
    if (module == NULL)
        return NULL;
    if (!setup_module(module))
    {
        Py_DECREF(module);
        return NULL;
    }

    Py_INCREF(&HashSplitterType);
    if (PyModule_AddObject(module, "HashSplitter",
                           (PyObject *) &HashSplitterType) < 0)
    {
        Py_DECREF(&HashSplitterType);
        Py_DECREF(module);
        return NULL;
    }

    Py_INCREF(&RecordHashSplitterType);
    if (PyModule_AddObject(module, "RecordHashSplitter",
                           (PyObject *) &RecordHashSplitterType) < 0)
    {
        Py_DECREF(&RecordHashSplitterType);
        Py_DECREF(&HashSplitterType);
        Py_DECREF(module);
        return NULL;
    }

    return module;
}
