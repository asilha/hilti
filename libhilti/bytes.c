//

#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "bytes.h"
#include "memory_.h"
#include "exceptions.h"
#include "string_.h"
#include "globals.h"
#include "hutil.h"
#include "threading.h"
#include "int.h"
#include "autogen/hilti-hlt.h"

// Bytes object cannot be changed anymore.
static const int _BYTES_FLAG_FROZEN = 1;

// Data of this node contains a separator object. It's fine to cast to
// __hlt_bytes_object in that case. The other data pointers inside the bytes
// object aren't valid in this case, and set to null.
static const int _BYTES_FLAG_OBJECT = 2;

struct __hlt_bytes {
    __hlt_gchdr __gchdr;       // Header for memory management.
    __hlt_thread_mgr_blockable blockable; // For blocking until changed.
    int8_t flags;              // Flags as combiniation of _BYTES_FLAG_* values.
    struct __hlt_bytes* next;  // Next part. Ref counted.
    int8_t* start;             // Pointer to first data byte.
    int8_t* end;               // Pointer to one after last data byte used so far.
    int8_t* reserved;          // Pointer to one after the last data byte available.
    int8_t* to_free;           // Need to free data pointed to when dtoring.
    int8_t data[0];            // Inline data starts here if free is zero.
};

// Specialized bytes object storing a separator object.
struct __hlt_bytes_object {
    struct __hlt_bytes b;       // Common header.
    const hlt_type_info* type;  // Type of the object;
    char object[0];             // Object's storage starts here, with size determined by type.
};

typedef struct __hlt_bytes_object __hlt_bytes_object;

static hlt_iterator_bytes GenericEndPos = { 0, 0 };

static inline hlt_bytes_size min(hlt_bytes_size a, hlt_bytes_size b)
{
    return a < b ? a : b;
}

static inline __hlt_bytes_object* __get_object(const hlt_bytes* b)
{
    return (b->flags & _BYTES_FLAG_OBJECT) ? (__hlt_bytes_object*) b : 0;
}

static inline hlt_bytes* __tail(hlt_bytes* b, int8_t consider_object)
{
    if ( ! b )
        return 0;

    while ( b->next ) {
        if ( ! consider_object && __get_object(b->next) )
            break;

        b = b->next;
    }

    return b;
}

static inline int8_t __at_object(const hlt_iterator_bytes i)
{
    return i.bytes && __get_object(i.bytes) != 0;
}

static inline int8_t __is_end(const hlt_iterator_bytes p)
{
    return p.bytes == 0 || (p.bytes == __tail(p.bytes, false) && p.cur >= p.bytes->end) || __at_object(p);
}

static inline int8_t __is_frozen(const hlt_bytes* b)
{
    return b ? (b->flags & _BYTES_FLAG_FROZEN) : false;
}

// For debugging.
static void __print_bytes(const char* prefix, const hlt_bytes* b)
{
    if ( ! b ) {
        fprintf(stderr, "%s: (null)\n", prefix);
        return;
    }

    fprintf(stderr, "%s: %p b( ", prefix, b);
    for ( ; b; b = b->next ) {
        fprintf(stderr, "#%ld:%p-%p \"", b->end - b->start, b->start, b->end);

        if ( __get_object(b) ) {
            fprintf(stderr, " [[object]] ");
            continue;
        }

        for ( int i = 0; i < min(b->end - b->start, 20); i++) {
            if ( isprint(b->start[i]) )
                fprintf(stderr, "%c", b->start[i]);
            else
                fprintf(stderr, "\\x%2x", b->start[i]);
        }

        if ( (b->end - b->start) > 20 )
            fprintf(stderr, "...");

        fprintf(stderr, "\" ");
    }

    fprintf(stderr, ")\n");
}

static void __print_iterator(const char* prefix, hlt_iterator_bytes i)
{
    fprintf(stderr, "%s: i(#%p@%lu) is_end=%d at_object=%d\n", prefix, i.bytes, i.bytes ? i.cur - i.bytes->start : 0, __is_end(i), __at_object(i));
    __print_bytes("  -> ", i.bytes);
}

static inline int8_t __is_empty(const hlt_bytes* b, int8_t consider_objects)
{
    for ( const hlt_bytes* c = b; c; c = c->next ) {
        if ( __get_object(c) )
            return ! consider_objects;

        if ( c->start != c->end )
            return 0;
    }

    return 1;
}

// TODO: Frequent enough to want a double-linked list?
static inline hlt_bytes* __pred(hlt_bytes* b, hlt_bytes* p)
{
    for ( ; b && b->next != p; b = b->next )
        ;

    return b;
}

// Does not ref the iterator.
static inline hlt_iterator_bytes __create_iterator(hlt_bytes* bytes, int8_t* cur)
{
    hlt_iterator_bytes i = { bytes, cur };
    return i;
}

// This version does not adjust the reference count and must be called only
// when the potentiall changed iterator will not be visible to the HILTI
// layer.
static inline void __normalize_iter(hlt_iterator_bytes* pos)
{
    if ( ! pos->bytes || __at_object(*pos) )
        return;

    // If the pos was previously an end position but now new data has been
    // added, adjust it so that it's pointing to the next byte. Note we do
    // not adjust the reference count in this version.
    while ( pos->cur >= pos->bytes->end && pos->bytes->next && ! __at_object(*pos) ) {
        pos->bytes = pos->bytes->next;
        pos->cur = pos->bytes->start;
    }
}

// This version adjust the reference count and should be called only when the
// potentiall changed iterator will be visible to the HILTI layer.
static inline void __normalize_iter_hilti(hlt_iterator_bytes* pos)
{
    if ( ! pos->bytes || __at_object(*pos) )
        return;

    // If the pos was previously an end position but now new data has been
    // added, adjust it so that it's pointing to the next byte.
    while ( pos->cur >= pos->bytes->end && pos->bytes->next ) {
        GC_ASSIGN(pos->bytes, pos->bytes->next, hlt_bytes);
        pos->cur = pos->bytes->start;
    }
}

// c already ref'ed.
static void __add_chunk(hlt_bytes* tail, hlt_bytes* c)
{
    assert(c);
    assert(tail);
    assert(! tail->next);
    assert(! __is_frozen(tail));

    tail->next = c; // Consumes ref cnt.
}

static hlt_bytes* __hlt_bytes_new(const int8_t* data, hlt_bytes_size len, hlt_bytes_size reserve)
{
    if ( ! reserve )
        reserve = (len ? len : 32);

    assert(reserve >= len);

    hlt_bytes* b = GC_NEW_CUSTOM_SIZE_NO_INIT(hlt_bytes, sizeof(hlt_bytes) + reserve);
    b->flags = 0;
    b->next = 0;
    b->start = b->data;
    b->end = b->start + len;
    b->reserved = b->start + reserve;
    b->to_free = 0;

    hlt_thread_mgr_blockable_init(&b->blockable);

    if ( data )
        memcpy(b->data, data, len);

    return b;
}

static hlt_bytes* __hlt_bytes_new_object(const hlt_type_info* type, void* obj, hlt_execution_context* ctx)
{
    __hlt_bytes_object* b = GC_NEW_CUSTOM_SIZE(hlt_bytes, sizeof(__hlt_bytes_object) + type->size);
    b->b.next = 0;
    b->b.flags = _BYTES_FLAG_OBJECT;
    b->type = type;

    hlt_thread_mgr_blockable_init(&b->b.blockable);

    if ( obj ) {
        memcpy(&b->object, obj, type->size);
        GC_CCTOR_GENERIC(&b->object, type);
    }

    return &b->b;
}

static hlt_bytes* __hlt_bytes_new_reuse(int8_t* data, hlt_bytes_size len)
{
    hlt_bytes* b = GC_NEW_NO_INIT(hlt_bytes);
    b->flags = 0;
    b->next = 0;
    b->start = data;
    b->end = data + len;
    b->reserved = data + len;
    b->to_free = data;

    hlt_thread_mgr_blockable_init(&b->blockable);

    return b;
}

hlt_bytes_size __hlt_bytes_len(hlt_bytes* b)
{
    hlt_bytes_size len = 0;

    for ( ; b && ! __get_object(b) ; b = b->next )
        len += (b->end - b->start);

    return len;
}

#if 0

// Turns out that we don't need this with the current object semantics. Note,
// if reenabling this code later: it hasn't been fully tested and may not
// work right in all cases.

// Duplicates b1 and, optionally, b2 into a preallocated bytes object. It's
// ok if dst isn't large enough, then we'll add chunks as necessary. The
// duplication includes all included separator objects.
static inline void __hlt_bytes_duplicate_into_helper(hlt_bytes* dst, hlt_bytes* src, int8_t* start, hlt_execution_context* ctx)
{
    // Copy data into object as long as possible.

    __hlt_bytes_object* o;
    hlt_bytes* b;

    hlt_bytes_size available = (dst->reserved - dst->start);
    int8_t* p = dst->start;

    for ( b = src; b && available && ! __get_object(b); b = b->next, start = 0 ) {

        if ( ! start )
            start = b->start;

        assert(start);

        hlt_bytes_size len = (b->end - b->start);
        hlt_bytes_size n = min(available, len);
        memcpy(p, start, n);
        dst->end = p + n;

        p += n;
        available -= n;

        if ( len != n ) {
            // Not enough space.
            start += n;
            break;
        }
    }

    if ( ! b )
        // All copied. Done.
        return;

    while ( b && (o = __get_object(b)) ) {
        // Duplicate object.
        void* p = &o->object;
        __hlt_bytes_object* no = (__hlt_bytes_object*)__hlt_bytes_new_object(o->type, &p, ctx);
        __add_chunk(dst, &no->b);
        dst = &no->b;
        b = b->next;
        start = b->start;
    }

    if ( ! b )
        // All copied now. Done.
        return;

    // Create a new bytes object, copy the rest in there, and append it.
    hlt_bytes_size len = __hlt_bytes_len(b);

    if ( len ) {
        hlt_bytes* ext = __hlt_bytes_new(0, len, 0);
        __add_chunk(dst, ext);
        __hlt_bytes_duplicate_into_helper(ext, b, start, ctx);
    }
}

static inline void __hlt_bytes_duplicate_into(hlt_bytes* dst, hlt_bytes* src, hlt_execution_context* ctx)
{
    __hlt_bytes_duplicate_into_helper(dst, src, src->start, ctx);
}

static inline void __hlt_bytes_duplicate_into_2(hlt_bytes* dst, hlt_bytes* src1, hlt_bytes* src2, hlt_execution_context* ctx)
{
    // We temporarily connect the two.
    hlt_bytes* t = __tail(src1, true);
    t->next = src2;
    __hlt_bytes_duplicate_into_helper(dst, src1, src1->start, ctx);
    t->next = 0;
}

#endif

void hlt_bytes_dtor(hlt_type_info* ti, hlt_bytes* b)
{
    b->start = b->end = 0;
    GC_CLEAR(b->next, hlt_bytes);

    __hlt_bytes_object* obj = __get_object(b);

    if ( obj ) {
        GC_DTOR_GENERIC(&obj->object, obj->type);
    }

    else if ( b->to_free )
        hlt_free(b->to_free);
}

void hlt_iterator_bytes_dtor(hlt_type_info* ti, hlt_iterator_bytes* p)
{
    if ( ! p->bytes )
        return;

    GC_CLEAR(p->bytes, hlt_bytes);
}

void hlt_iterator_bytes_cctor(hlt_type_info* ti, hlt_iterator_bytes* p)
{
    if ( ! p->bytes )
        return;

    GC_CCTOR(p->bytes, hlt_bytes)
}

hlt_bytes* hlt_bytes_new(hlt_exception** excpt, hlt_execution_context* ctx)
{
    return __hlt_bytes_new(0, 0, 0);
}

hlt_bytes* hlt_bytes_new_from_data(int8_t* data, hlt_bytes_size len, hlt_exception** excpt, hlt_execution_context* ctx)
{
    return __hlt_bytes_new_reuse(data, len);
}

hlt_bytes* hlt_bytes_new_from_data_copy(const int8_t* data, hlt_bytes_size len, hlt_exception** excpt, hlt_execution_context* ctx)
{
    return __hlt_bytes_new(data, len, 0);
}

void* hlt_bytes_clone_alloc(const hlt_type_info* ti, void* srcp, __hlt_clone_state* cstate, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes* src = *(hlt_bytes**)srcp;

    __hlt_bytes_object *so = __get_object(src);

    if ( so )
        return __hlt_bytes_new_object(so->type, 0, ctx);
    else
        return __hlt_bytes_new(0, src->end - src->start, 0);
}

void hlt_bytes_clone_init(void* dstp, const hlt_type_info* ti, void* srcp, __hlt_clone_state* cstate, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes* src = *(hlt_bytes**)srcp;
    hlt_bytes* dst = *(hlt_bytes**)dstp;

    assert(src && dst);

    dst->flags = src->flags;

    int first = 1;

    for ( ; src; src = src->next ) {

        hlt_bytes* b = 0;

        __hlt_bytes_object *so = __get_object(src);

        if ( so ) {
            if ( first ) {
                // Already allocated.
                __hlt_bytes_object* no = (__hlt_bytes_object*) dst;
                memcpy(&no->object, &so->object, no->type->size);
                GC_CCTOR_GENERIC(&no->object, no->type);
                b = &no->b;
            }

            else {
                void* p = &so->object;
                __hlt_bytes_object* no = (__hlt_bytes_object*)__hlt_bytes_new_object(so->type, p, ctx);
                b = &no->b;
            }
        }

        else {
            if  ( first ) {
                // Already allocated.
                hlt_bytes_size n = (src->end - src->start);
                memcpy(dst->start, src->start, n);
                b = dst;
            }

            else {
                hlt_bytes_size n = (src->end - src->start);
                b = __hlt_bytes_new(src->start, n, 0);
            }
        }

        if ( ! first ) {
            __add_chunk(dst, b);
            dst = b;
        }

        else
            first = 0;

    }
}

// Returns the number of bytes stored.
hlt_bytes_size hlt_bytes_len(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    return __hlt_bytes_len(b);
}

// Returns true if empty.
int8_t hlt_bytes_empty(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    return __is_empty(b, false);
}

void __hlt_bytes_append_raw(hlt_bytes* b, int8_t* raw, hlt_bytes_size len, hlt_exception** excpt, hlt_execution_context* ctx, int8_t reuse)
{
    if ( ! len )
        return;

    if ( __is_frozen(b) ) {
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return;
    }

    // TODO: Copy into available space if large enough.

    hlt_bytes* c;

    if ( reuse )
        c = __hlt_bytes_new_reuse(raw, len);
    else
        c = __hlt_bytes_new(raw, len, 0);

    __add_chunk(__tail(b, true), c);

    hlt_thread_mgr_unblock(&b->blockable, ctx);
}

// Appends one Bytes object to another.
void hlt_bytes_append(hlt_bytes* b, hlt_bytes* other, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return;
    }

    if ( __is_frozen(b) ) {
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return;
    }

    hlt_bytes_size len = hlt_bytes_len(other, excpt, ctx);

    if ( ! len )
        return;

    // TODO: Copy into available space if large enough.

    hlt_bytes* dst = __hlt_bytes_new(0, len, 0);
    int8_t* p = dst->start;

    for ( ; other && ! __get_object(other); other = other->next ) {
        hlt_bytes_size n = other->end - other->start;
        memcpy(p, other->start, n);
        p += n;
    }

    __add_chunk(__tail(b, true), dst);

    hlt_thread_mgr_unblock(&b->blockable, ctx);
}

void hlt_bytes_append_raw(hlt_bytes* b, int8_t* raw, hlt_bytes_size len, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return;
    }

    __hlt_bytes_append_raw(b, raw, len, excpt, ctx, 1);
}

void hlt_bytes_append_raw_copy(hlt_bytes* b, int8_t* raw, hlt_bytes_size len, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return;
    }

    __hlt_bytes_append_raw(b, raw, len, excpt, ctx, 0);
}

hlt_bytes* hlt_bytes_concat(hlt_bytes* b1, hlt_bytes* b2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! (b1 && b2) ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_bytes_size len1 = hlt_bytes_len(b1, excpt, ctx);
    hlt_bytes_size len2 = hlt_bytes_len(b2, excpt, ctx);

    hlt_bytes* dst = __hlt_bytes_new(0, len1 + len2, 0);

    int8_t* p = dst->start;

    for ( ; b1 && ! __get_object(b1); b1 = b1->next ) {
        hlt_bytes_size n = b1->end - b1->start;
        memcpy(p, b1->start, n);
        p += n;
    }

    for ( ; b2 && ! __get_object(b2); b2 = b2->next ) {
        hlt_bytes_size n = b2->end - b2->start;
        memcpy(p, b2->start, n);
        p += n;
    }

    return dst;
}

// Doesn't ref the iterator.
void __hlt_bytes_find_byte(hlt_iterator_bytes* p, hlt_bytes* b, int8_t chr, hlt_exception** excpt, hlt_execution_context* ctx)
{
    for ( ; b && ! __get_object(b); b = b->next ) {
        int8_t* c = memchr(b->start, chr, b->end - b->start);

        if ( c ) {
            *p = __create_iterator(b, c);
            return;
        }
    }

    *p = GenericEndPos;
}

// Doesn't ref the iterator.
void __hlt_bytes_end(hlt_iterator_bytes* p, hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        *p = GenericEndPos;
        return;
    }

    p->bytes = __tail(b, false);
    p->cur = p->bytes->end;
}

// Doesn't ref the iterator.
void __hlt_bytes_begin(hlt_iterator_bytes* p, hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( __is_empty(b, false) ) {
        __hlt_bytes_end(p, b, excpt, ctx);
        return;
    }

    p->bytes = b;
    p->cur = b->start;
    __normalize_iter(p);
}

// Doesn't ref the iterator.
void __hlt_bytes_find_byte_from(hlt_iterator_bytes* p, hlt_iterator_bytes i, int8_t chr, hlt_exception** excpt, hlt_execution_context* ctx)
{
    // First chunk.
    hlt_bytes* b = i.bytes;
    int8_t* c = memchr(i.cur, chr, b->end - i.cur);

    if ( ! c ) {
        // Subsequent chunks.
        for ( b = i.bytes->next; b && ! __get_object(b); b = b->next ) {
            int8_t* c = memchr(b->start, chr, b->end - b->start);

            if ( c )
                break;
        }
    }

    if ( c )
        *p = __create_iterator(b, c);
    else
        __hlt_bytes_end(p, i.bytes, excpt, ctx);
}

hlt_iterator_bytes hlt_bytes_find_byte(hlt_bytes* b, int8_t chr, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return GenericEndPos;
    }

    hlt_iterator_bytes p;
    __hlt_bytes_find_byte(&p, b, chr, excpt, ctx);
    GC_CCTOR(p, hlt_iterator_bytes);
    return p;
}

void __hlt_iterator_bytes_incr(hlt_iterator_bytes* p, hlt_exception** excpt, hlt_execution_context* ctx, int8_t adj_ref)
{
    if ( __is_end(*p) )
        // Fail silently.
        return;

    // Can we stay inside the same chunk?
    if ( p->cur + 1 < p->bytes->end ) {
        ++p->cur;
        return;
    }

    if ( ! p->bytes->next ) {
        // End reached.
        p->cur = p->bytes->end;
        return;
    }

    // Switch chunk.
    if ( adj_ref ) {
        GC_ASSIGN(p->bytes, p->bytes->next, hlt_bytes);
    }

    else
        p->bytes = p->bytes->next;

    p->cur = p->bytes->start;
}

void __hlt_iterator_bytes_incr_by(hlt_iterator_bytes* p, int64_t n, hlt_exception** excpt, hlt_execution_context* ctx, int8_t adj_ref)
{
    if ( __is_end(*p) )
        // Fail silently.
        return;

    while ( 1 ) {

        // Can we stay inside the same chunk?
        if ( p->cur + n < p->bytes->end ) {
            p->cur += n;
            return;
        }

        if ( ! p->bytes->next || __get_object(p->bytes) ) {
            // End reached.
            p->cur = p->bytes->end;
            return;
        }

        // Switch chunk.
        n -= (p->bytes->end - p->cur);

        if ( adj_ref ) {
            GC_ASSIGN(p->bytes, p->bytes->next, hlt_bytes);
        }

        else
            p->bytes = p->bytes->next;

        p->cur = p->bytes->start;
    }

    // Cannot reach.
    assert(false);
}

int8_t __hlt_iterator_bytes_deref(hlt_iterator_bytes p, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( __is_end(p) ) {
        // Position is out range.
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return 0;
    }

    return *p.cur;
}

int8_t __hlt_iterator_bytes_eq(hlt_iterator_bytes p1, hlt_iterator_bytes p2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( __is_end(p1) && __is_end(p2) )
        return 1;

    return p1.bytes == p2.bytes && p1.cur == p2.cur;
}

// Returns the number of bytes from p1 to p2 (not counting p2).
hlt_bytes_size __hlt_iterator_bytes_diff(hlt_iterator_bytes p1, hlt_iterator_bytes p2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( __is_end(p1) ) {
        if ( ! __is_end(p2) && ! (__at_object(p1) || __at_object(p2)) ) {
            // Invalid starting position.
            hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        }

        return 0;
    }

    if ( __is_end(p2) )
        return hlt_bytes_len(p1.bytes, excpt, ctx) - (p1.cur - p1.bytes->start);

    if ( __hlt_iterator_bytes_eq(p1, p2, excpt, ctx) )
        return 0;

    // Special case: both inside same chunk.
    if ( p1.bytes == p2.bytes ) {
        hlt_bytes_size n = p2.cur - p1.cur;
        return (n > 0 ? n : 0);
    }

    // Count first bytes.
    hlt_bytes_size n = p1.bytes->end - p1.cur;

    // Count intermediary bytes.

    int8_t p2_is_end = __is_end(p2);
    hlt_bytes* c = p1.bytes->next;

    for ( ; c != p2.bytes || (p2_is_end && c && ! __get_object(c)); c = c->next )
        n += (c->end - c->start);

    // Count final bytes.

    if ( c )
        n += (p2.cur - p2.bytes->start);

    else if ( ! p2_is_end ) {
        // Incompatible iterators.
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return 0;
    }

    return n;
}

// Doesn't ref the iterator.
int8_t __hlt_bytes_find_bytes(hlt_iterator_bytes* p, hlt_bytes* b, hlt_bytes* other, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( __is_empty(other, false) ) {
        // Empty pattern returns start position.
        __hlt_bytes_begin(p, b, excpt, ctx);
        return 1;
    }

    hlt_iterator_bytes begin;
    hlt_iterator_bytes end;
    __hlt_bytes_begin(&begin, other, excpt, ctx);
    __hlt_bytes_end(&end, other, excpt, ctx);


    int8_t chr = *begin.cur;

    hlt_iterator_bytes i;
    __hlt_bytes_begin(&i, b, excpt, ctx);

    while ( 1 ) {
        __hlt_bytes_find_byte_from(p, i, chr, excpt, ctx);

        if ( __is_end(*p) ) {
            // Not found.
            break;
        }

        if ( hlt_bytes_match_at(*p, other, excpt, ctx) )
            // Found.
            return 1;

        __hlt_iterator_bytes_incr(&i, excpt, ctx, 0);

        if ( __is_end(i) )
            // Not found.
            break;
    }

    __hlt_bytes_end(p, b, excpt, ctx);
    return 0;
}

int8_t hlt_bytes_contains_bytes(hlt_bytes* b, hlt_bytes* other, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! (b && other) ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_iterator_bytes p;
    return __hlt_bytes_find_bytes(&p, b, other, excpt, ctx);
}

hlt_iterator_bytes hlt_bytes_find_bytes(hlt_bytes* b, hlt_bytes* other, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! (b && other) ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return GenericEndPos;
    }

    hlt_iterator_bytes p;
    __hlt_bytes_find_bytes(&p, b, other, excpt, ctx);
    GC_CCTOR(p, hlt_iterator_bytes);
    return p;
}

static int8_t __hlt_bytes_match_at(hlt_iterator_bytes p, hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( __is_end(p) )
        return __is_empty(b, false);

    if ( __is_empty(b, false) )
        return 0;

    hlt_iterator_bytes c1 = p;
    hlt_iterator_bytes c2;

    __hlt_bytes_begin(&c2, b, excpt, ctx);

    while ( 1 ) {
        // Extract bytes.
        int8_t b1 = *(c1.cur);
        int8_t b2 = *(c2.cur);

        if ( b1 != b2 )
            return 0;

        __hlt_iterator_bytes_incr(&c1, excpt, ctx, 0);
        __hlt_iterator_bytes_incr(&c2, excpt, ctx, 0);

        // End of pattern reached?
        if ( __is_end(c2) )
            // Found.
            return 1;

        // End of input reached?
        if ( __is_end(c1) )
            // Not found, but could have if more input available.
            return -1;
    }

    // Can't be reached.
    assert(0);
}

hlt_bytes_find_at_iter_result hlt_bytes_find_bytes_at_iter(hlt_iterator_bytes i, hlt_bytes* needle, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes_find_at_iter_result r;
    r.iter = i;

    if ( ! needle ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        r.iter = GenericEndPos;
        return r;
    }

    if ( __is_empty(needle, false) ) {
        // Empty pattern returns start position.
        r.success = 1;
        r.iter = i;
        goto done;
    }

    hlt_iterator_bytes nbegin;
    __hlt_bytes_begin(&nbegin, needle, excpt, ctx);
    int8_t chr = *nbegin.cur;

    __normalize_iter(&r.iter);

    while ( 1 ) {
        __hlt_bytes_find_byte_from(&r.iter, r.iter, chr, excpt, ctx);

        if ( __is_end(r.iter) ) {
            // Not found. Leave r.iter at end.
            r.success = 0;
            break;
        }

        int rc = __hlt_bytes_match_at(r.iter, needle, excpt, ctx);

        if ( rc > 0 ) {
            // Found. Leave r.iter at start position.
            r.success = 1;
            break;
        }

        if ( rc == -1 ) {
            // Out of input, may still match. Leave r.iter at start position.
            r.success = 0;
            break;
        }

        // Not found, advance.

        __hlt_iterator_bytes_incr(&r.iter, excpt, ctx, 0);

        if ( __is_end(r.iter) ) {
            // Not found. Leave r.iter at end.
            r.success = 0;
            break;
        }
    }

done:
    GC_CCTOR(r.iter, hlt_iterator_bytes);
    return r;
}

hlt_bytes* hlt_bytes_clone(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes* dst = 0;
    hlt_clone_deep(&dst, &hlt_type_info_hlt_bytes, &b, excpt, ctx);
    return dst;
}

static int8_t* __hlt_bytes_sub_raw_internal(int8_t* buffer, hlt_bytes_size buffer_size, hlt_bytes_size len, hlt_iterator_bytes p1, hlt_iterator_bytes p2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    // Some here is a bit awkward and only to retain the old semantics of
    // to_raw_buffer() that if the buffer gets exceeded, we fill it to the
    // max and return null; and a pointer to one beyond last byte written
    // otherwise.

    if ( ! buffer_size )
        return len ? 0 : buffer;

    if ( ! len )
        return buffer;

    if ( __is_end(p1) ) {
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return 0;
    }

    if ( ! p2.bytes )
        __hlt_bytes_end(&p2, p1.bytes, excpt, ctx);

    int8_t* p = buffer;

    // Special case: both inside same chunk.
    if ( p1.bytes == p2.bytes ) {
        hlt_bytes_size n = min((p2.cur - p1.cur), buffer_size);

        if ( n )
            memcpy(p, p1.cur, n);

        return buffer_size <= len ? p + n : 0;
    }

    // First block.
    hlt_bytes_size n = min((p1.bytes->end - p1.cur), buffer_size - (p - buffer));

    if ( n ) {
        memcpy(p, p1.cur, n);
        p += n;
    }

    // Intermediate blocks.
    int8_t p2_is_end = __is_end(p2);
    hlt_bytes* c = p1.bytes->next;

    for ( ; c && (c != p2.bytes || p2_is_end) && ! __get_object(c); c = c->next ) {
        hlt_bytes_size n = min((c->end - c->start), buffer_size - (p - buffer));

        if ( n ) {
            memcpy(p, c->start, n);
            p += n;
        }
    }

    if ( c ) {
        hlt_bytes_size n = min((p2.cur - p2.bytes->start), buffer_size - (p - buffer));

        if ( n ) {
            memcpy(p, p2.bytes->start, n);
            p += n;
        }
    }

    else if ( ! p2_is_end ) {
        // Incompatible iterators.
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return 0;
    }

    return buffer_size <= len ? p : 0;
}

hlt_bytes* hlt_bytes_sub(hlt_iterator_bytes p1, hlt_iterator_bytes p2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&p1);
    __normalize_iter(&p2);

    hlt_bytes_size len = __hlt_iterator_bytes_diff(p1, p2, excpt, ctx);
    hlt_bytes* dst = __hlt_bytes_new(0, len, 0);

    if ( ! len )
        return dst;

    if ( __hlt_bytes_sub_raw_internal(dst->start, len, len, p1, p2, excpt, ctx) )
        return dst;

    // Exception.
    GC_DTOR(dst, hlt_bytes);
    return 0;
}

int8_t* hlt_bytes_sub_raw(hlt_iterator_bytes p1, hlt_iterator_bytes p2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&p1);
    __normalize_iter(&p2);

    hlt_bytes_size len = __hlt_iterator_bytes_diff(p1, p2, excpt, ctx);
    int8_t* dst = hlt_malloc(len);

    if ( __hlt_bytes_sub_raw_internal(dst, len, len, p1, p2, excpt, ctx) )
        return dst;

    // Exception.
    hlt_free(dst);
    return 0;
}

int8_t hlt_bytes_match_at(hlt_iterator_bytes p, hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    __normalize_iter(&p);

    return __hlt_bytes_match_at(p, b, excpt, ctx) > 0;
}

int8_t* hlt_bytes_to_raw(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_iterator_bytes begin;
    hlt_iterator_bytes end;

    __hlt_bytes_begin(&begin, b, excpt, ctx);
    __hlt_bytes_end(&end, b, excpt, ctx);

    return hlt_bytes_sub_raw(begin, end, excpt, ctx);
}

int8_t* hlt_bytes_to_raw_buffer(hlt_bytes* b, int8_t* buffer, hlt_bytes_size buffer_size, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_bytes_size len = hlt_bytes_len(b, excpt, ctx);

    hlt_iterator_bytes begin;
    hlt_iterator_bytes end;

    __hlt_bytes_begin(&begin, b, excpt, ctx);
    __hlt_bytes_end(&end, b, excpt, ctx);

    return __hlt_bytes_sub_raw_internal(buffer, buffer_size, len, begin, end, excpt, ctx);
}

int8_t __hlt_bytes_extract_one_slowpath(hlt_iterator_bytes* p, hlt_iterator_bytes end, hlt_exception** excpt, hlt_execution_context* ctx)
{
    assert(p);
    assert(p->bytes);

    __normalize_iter_hilti(p);
    __normalize_iter(&end);

    if ( __is_end(*p) || hlt_iterator_bytes_eq(*p, end, excpt, ctx) ) {
        GC_DTOR(*p, hlt_iterator_bytes);
        hlt_set_exception(excpt, &hlt_exception_would_block, 0);
        return 0;
    }

    // Extract byte.
    int8_t b = *(p->cur);

    // Increase iterator.
    if ( p->cur < p->bytes->end - 1 )
        // We stay inside chunk.
        ++p->cur;

    else {
        if ( p->bytes->next ) {
            // Switch to next chunk.
            GC_ASSIGN(p->bytes, p->bytes->next, hlt_bytes);
            p->cur = p->bytes->start;
        }
        else
            // End reached.
            p->cur = p->bytes->end;
    }

    return b;
}

// We optimize this for the common case and keep it small for inlining.
int8_t __hlt_bytes_extract_one(hlt_iterator_bytes* p, hlt_iterator_bytes end, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( p->bytes && ! __get_object(p->bytes) ) {
        if ( (p->bytes == end.bytes && (p->cur < end.cur - 1)) ||
             (p->bytes != end.bytes && (p->cur < p->bytes->end - 1)) )
                return *(p->cur++);
    }

    return __hlt_bytes_extract_one_slowpath(p, end, excpt, ctx);
}

hlt_iterator_bytes hlt_bytes_offset(hlt_bytes* b, hlt_bytes_size p, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return GenericEndPos;
    }

    if ( ! p )
        return hlt_bytes_begin(b, excpt, ctx);

    if ( p < 0 )
        p += hlt_bytes_len(b, excpt, ctx);

    if ( p < 0 )
        return hlt_bytes_end(b, excpt, ctx);

    hlt_bytes* c;
    for ( c = b; c && p >= (c->end - c->start) && ! __get_object(c); c = c->next ) {
        p -= (c->end - c->start);
    }

    if ( ! c ) {
        // Position is out range, return end().
        return hlt_bytes_end(b, excpt, ctx);
    }

    hlt_iterator_bytes i;
    GC_INIT(i.bytes, c, hlt_bytes);
    i.cur = c->start + p;
    return i;
}

void __hlt_bytes_normalize_iter(hlt_iterator_bytes* pos)
{
    return __normalize_iter(pos);
}

hlt_iterator_bytes hlt_bytes_begin(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return GenericEndPos;
    }

    hlt_iterator_bytes p;
    __hlt_bytes_begin(&p, b, excpt, ctx);
    GC_CCTOR(p, hlt_iterator_bytes);
    return p;
}

hlt_iterator_bytes hlt_bytes_end(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return GenericEndPos;
    }

    hlt_iterator_bytes p;
    __hlt_bytes_end(&p, b, excpt, ctx);
    GC_CCTOR(p, hlt_iterator_bytes);
    return p;
}

hlt_iterator_bytes hlt_bytes_generic_end(hlt_exception** excpt, hlt_execution_context* ctx)
{
    return GenericEndPos;
}

void hlt_bytes_freeze(hlt_bytes* b, int8_t freeze, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return;
    }

    for ( hlt_bytes* c = b; c; c = c->next ) {
        if ( freeze )
            c->flags |= _BYTES_FLAG_FROZEN;
        else
            c->flags &= ~_BYTES_FLAG_FROZEN;
    }

    hlt_thread_mgr_unblock(&b->blockable, ctx);
}

void hlt_bytes_trim(hlt_bytes* b, hlt_iterator_bytes p, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&p);

    __hlt_bytes_object *o = p.bytes ? __get_object(p.bytes) : 0;

    if ( __is_end(p) && ! o )
        return;

    // Check if within first block, we just adjust the start pointer there
    // then.
    if ( p.bytes == b ) {
        b->start = p.cur;
        return;
    }

    // Find predecesor.
    hlt_bytes* pred = __pred(b, p.bytes);

    if ( ! pred ) {
        // Invalid iterator for this object.
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return;
    }

    // We need to keep the start block so that our object pointer remains the
    // same, but we empty it out and then delete intermediary blocks.
    b->start = b->end;
    GC_ASSIGN(b->next, p.bytes, hlt_bytes);
    b->next->start = p.cur;

    // Don't need old start node data anymore;
    if ( (o = __get_object(b)) ) {
        GC_DTOR_GENERIC(&o->object, o->type);
    }

    else if ( b->to_free ) {
        hlt_free(b->to_free);
        b->to_free = 0;
    }
}

int8_t hlt_bytes_is_frozen(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    return __is_frozen(b);
}

void hlt_iterator_bytes_clone_init(void* dstp, const hlt_type_info* ti, void* srcp, __hlt_clone_state* cstate, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_iterator_bytes* src = (hlt_iterator_bytes*)srcp;
    hlt_iterator_bytes* dst = (hlt_iterator_bytes*)srcp;

    // We can only clone the and eob iterator.
    if ( __is_end(*src) ) {
        *dst = GenericEndPos;
        return;
    }

    hlt_string msg = hlt_string_from_asciiz("iterator<bytes> supports cloning only for end position", excpt, ctx);
    hlt_set_exception(excpt, &hlt_exception_cloning_not_supported, msg);
}

hlt_iterator_bytes hlt_iterator_bytes_copy(hlt_iterator_bytes pos, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&pos);

    if ( __is_end(pos) )
        return GenericEndPos;

    hlt_iterator_bytes copy;
    GC_INIT(copy.bytes, pos.bytes, hlt_bytes);
    copy.cur = pos.cur;
    return copy;
}

int8_t hlt_iterator_bytes_is_frozen(hlt_iterator_bytes pos, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&pos);

    if ( ! pos.bytes )
        return 0;

    return __is_frozen(pos.bytes);
}

int8_t hlt_iterator_bytes_deref(hlt_iterator_bytes pos, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&pos);
    return __hlt_iterator_bytes_deref(pos, excpt, ctx);
}

hlt_iterator_bytes hlt_iterator_bytes_incr(hlt_iterator_bytes old, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&old);

    hlt_iterator_bytes ni = old;
    GC_CCTOR(ni, hlt_iterator_bytes);
    __hlt_iterator_bytes_incr(&ni, excpt, ctx, 1);
    return ni;
}

hlt_iterator_bytes hlt_iterator_bytes_incr_by(hlt_iterator_bytes old, int64_t n, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&old);

    hlt_iterator_bytes ni = old;
    GC_CCTOR(ni, hlt_iterator_bytes);
    __hlt_iterator_bytes_incr_by(&ni, n, excpt, ctx, 1);

    return ni;
}

#if 0
// This is a bit tricky because pos_end() doesn't return anything suitable
// for iteration.  Let's leave this out for now until we know whether we
// actually need reverse iteration.
void hlt_iterator_bytes_decr(hlt_iterator_bytes* pos, hlt_exception** excpt, hlt_execution_context* ctx)
{
    XXX
}
#endif

int8_t hlt_iterator_bytes_eq(hlt_iterator_bytes pos1, hlt_iterator_bytes pos2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&pos1);
    __normalize_iter(&pos2);

    return __hlt_iterator_bytes_eq(pos1, pos2, excpt, ctx);
}

// Returns the number of bytes from pos1 to pos2 (not counting pos2).
hlt_bytes_size hlt_iterator_bytes_diff(hlt_iterator_bytes pos1, hlt_iterator_bytes pos2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&pos1);
    __normalize_iter(&pos2);

    return __hlt_iterator_bytes_diff(pos1, pos2, excpt, ctx);
}

hlt_string hlt_bytes_to_string(const hlt_type_info* type, const void* obj, int32_t options, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_string dst = 0;

    hlt_bytes* b = *((hlt_bytes**)obj);

    if ( ! b )
        return hlt_string_from_asciiz("(Null)", excpt, ctx);

    // __print_bytes("debug:", b);

    char hex[5] = { '\\', 'x', 'X', 'X', '\0' };
    char buffer[256];
    uint64_t i = 0;

    for ( const hlt_bytes* c = b; c; c = c->next ) {

        __hlt_bytes_object* o = __get_object(c);

        if ( o ) {
            hlt_string prefix = hlt_string_from_asciiz("<obj:", excpt, ctx);
            hlt_string postfix = hlt_string_from_asciiz(">", excpt, ctx);

            __hlt_pointer_stack seen;
            __hlt_pointer_stack_init(&seen);
            hlt_string tmp = hlt_object_to_string(o->type, &o->object, 0, &seen, excpt, ctx);
            __hlt_pointer_stack_destroy(&seen);

            dst = hlt_string_concat_and_unref(dst, prefix, excpt, ctx);
            dst = hlt_string_concat_and_unref(dst, tmp, excpt, ctx);
            dst = hlt_string_concat_and_unref(dst, postfix, excpt, ctx);
            continue;
        }

        for ( int8_t* p = c->start; p < c->end; ++p ) {
            if ( i > sizeof(buffer) - 10 ) {
                hlt_string tmp = hlt_string_from_data((int8_t*)buffer, i, excpt, ctx);
                dst = hlt_string_concat_and_unref(dst, tmp, excpt, ctx);
                i = 0;
            }

            unsigned char c = *(unsigned char *)p;

            if ( isprint((char)c) && c < 128 )
                buffer[i++] = c;

            else {
                hlt_util_uitoa_n(c, hex + 2, 3, 16, 1);
                strcpy(buffer + i, hex);
                i += sizeof(hex) - 1;
            }
        }

        if ( i ) {
            hlt_string tmp = hlt_string_from_data((int8_t*)buffer, i, excpt, ctx);
            dst = hlt_string_concat_and_unref(dst, tmp, excpt, ctx);
            i = 0;
        }

    }

    return dst;
}

hlt_hash hlt_bytes_hash(const hlt_type_info* type, const void* obj, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes* b = *((hlt_bytes**)obj);

    hlt_hash hash = 0;

    for ( ; b; b = b->next ) {

        __hlt_bytes_object* o = __get_object(b);

        if ( o ) {
            hash += (o->type->hash)(o->type, &o->object, 0, 0);
            continue;
        }

        hlt_bytes_size n = (b->end - b->start);

        if ( n )
            hash = hlt_hash_bytes(b->start, n, hash);
    }

    return hash;
}

int8_t hlt_bytes_equal(const hlt_type_info* type1, const void* obj1, const hlt_type_info* type2, const void* obj2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes* b1 = *((hlt_bytes**)obj1);
    hlt_bytes* b2 = *((hlt_bytes**)obj2);

    hlt_execution_context* c= hlt_global_execution_context();
    hlt_exception* e = 0;

    return hlt_bytes_cmp(b1, b2, &e, c) == 0;
}

void* hlt_bytes_blockable(const hlt_type_info* type, const void* obj, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes* b = *((hlt_bytes**)obj);
    return &b->blockable;
}

void* hlt_bytes_iterate_raw(hlt_bytes_block* block, void* cookie, hlt_iterator_bytes p1, hlt_iterator_bytes p2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&p1);
    __normalize_iter(&p2);

    if ( ! cookie ) {

        if ( __hlt_iterator_bytes_eq(p1, p2, excpt, ctx) ) {
            block->start = block->end = p1.cur;
            block->next = 0;
            return 0;
        }

        if ( __is_end(p1) && ! __is_end(p2) ) {
            hlt_set_exception(excpt, &hlt_exception_value_error, 0);
            return 0;
        }

        if ( p1.bytes != p2.bytes  ) {
            block->start = p1.cur;
            block->end = p1.bytes->end;
            block->next = p1.bytes->next;
        }

        else {
            block->start = p1.cur;
            block->end = p2.cur;
            block->next = 0;
            }
    }

    else {
        if ( ! block->next || __get_object(block->next) )
            return 0;

        if ( block->next != p2.bytes ) {
            block->start = block->next->start;
            block->end = block->next->end;
            block->next = block->next->next;
        }

        else {
            block->start = p2.bytes->start;
            block->end = p2.cur;
            block->next = 0;
        }
    }

    // We don't care what we return if we haven't reached the end yet as long
    // as it's not NULL.
    return block->next && ! __get_object(block->next) ? block : 0;
}

int8_t hlt_bytes_cmp(hlt_bytes* b1, hlt_bytes* b2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! (b1 && b2) ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    // Bail out quickly if one is empty but the other isn't.
    if ( __is_empty(b1, false) )
        return __is_empty(b2, false) ? 0 : 1;

    if ( __is_empty(b2, false) )
        return __is_empty(b1, false) ? 0 : -1;

    // FIXME: This is not the smartest implementation. Rather than using the
    // iterator functions, we should compare larger chunks at once.
    hlt_iterator_bytes p1;
    hlt_iterator_bytes p2;

    __hlt_bytes_begin(&p1, b1, excpt, ctx);
    __hlt_bytes_begin(&p2, b2, excpt, ctx);

    int8_t result = 0;

    while ( 1 ) {

        if ( __is_end(p1) )
            return (__is_end(p2) ? 0 : 1);

        if ( __is_end(p2) )
            return -1;

        int8_t c1 = __hlt_iterator_bytes_deref(p1, excpt, ctx);
        int8_t c2 = __hlt_iterator_bytes_deref(p2, excpt, ctx);

        if ( c1 != c2 )
            return (c1 > c2 ? -1 : 1);

        __hlt_iterator_bytes_incr(&p1, excpt, ctx, 0);
        __hlt_iterator_bytes_incr(&p2, excpt, ctx, 0);
    }

    // Cannot be reached.
    assert(0);
}

int64_t hlt_bytes_to_int(hlt_bytes* b, int64_t base, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_iterator_bytes cur;
    __hlt_bytes_begin(&cur, b, excpt, ctx);

    int64_t value = 0;
    int64_t t = 0;
    int8_t first = 1;
    int8_t negate = 0;

    while ( ! __is_end(cur) ) {
        int8_t ch = __hlt_iterator_bytes_deref(cur, excpt, ctx);

        if ( isdigit(ch) )
            t = ch - '0';

        else if ( isalpha(ch) )
            t = 10 + tolower(ch) - 'a';

        else if ( first && ch == '-' )
            negate = 1;

        else {
            hlt_set_exception(excpt, &hlt_exception_value_error, 0);
            return 0;
        }

        if ( t >= base ) {
            hlt_set_exception(excpt, &hlt_exception_value_error, 0);
            return 0;
        }

        value = (value * base) + t;
        first = 0;

        __hlt_iterator_bytes_incr(&cur, excpt, ctx, 0);
    }

    return negate ? -value : value;
}

int64_t hlt_bytes_to_int_binary(hlt_bytes* b, hlt_enum byte_order, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_bytes_size len = hlt_bytes_len(b, excpt, ctx);

    if ( len > 8 ) {
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return 0;
    }

    hlt_iterator_bytes cur;
    __hlt_bytes_begin(&cur, b, excpt, ctx);

    int64_t i = 0;

    while ( ! __is_end(cur) ) {
        uint8_t ch = __hlt_iterator_bytes_deref(cur, excpt, ctx);
        i = (i << 8) | ch;
        __hlt_iterator_bytes_incr(&cur, excpt, ctx, 0);
        }

    if ( hlt_enum_equal(byte_order, Hilti_ByteOrder_Little, excpt, ctx) )
        i = hlt_int_flip(i, len, excpt, ctx);

    return i;
}

hlt_bytes* hlt_bytes_lower(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_bytes_size len = hlt_bytes_len(b, excpt, ctx);
    hlt_bytes* dst = __hlt_bytes_new(0, len, 0);
    int8_t* p = dst->start;

    hlt_iterator_bytes cur;
    __hlt_bytes_begin(&cur, b, excpt, ctx);

    while ( ! __is_end(cur) ) {
        int8_t ch = __hlt_iterator_bytes_deref(cur, excpt, ctx);
        *p++ = tolower(ch);
        __hlt_iterator_bytes_incr(&cur, excpt, ctx, 0);
    }

    return dst;
}

hlt_bytes* hlt_bytes_upper(hlt_bytes* b, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! b ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    hlt_bytes_size len = hlt_bytes_len(b, excpt, ctx);
    hlt_bytes* dst = __hlt_bytes_new(0,len, 0);
    int8_t* p = dst->start;

    hlt_iterator_bytes cur;
    __hlt_bytes_begin(&cur, b, excpt, ctx);

    while ( ! __is_end(cur) ) {
        int8_t ch = __hlt_iterator_bytes_deref(cur, excpt, ctx);
        *p++ = toupper(ch);
        __hlt_iterator_bytes_incr(&cur, excpt, ctx, 0);
    }

    return dst;
}

int8_t hlt_bytes_starts_with(hlt_bytes* b, hlt_bytes* s, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( ! (b && s) ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return 0;
    }

    // FIXME: This is not the smartest implementation. Rather than using the
    // iterator functions, we should compare larger chunks at once.

    hlt_bytes_size len_b = hlt_bytes_len(b, excpt, ctx);
    hlt_bytes_size len_s = hlt_bytes_len(s, excpt, ctx);

    if ( len_s > len_b )
        return 0;

    hlt_iterator_bytes cur_b;
    hlt_iterator_bytes cur_s;
    __hlt_bytes_begin(&cur_b, b, excpt, ctx);
    __hlt_bytes_begin(&cur_s, s, excpt, ctx);

    while ( ! __is_end(cur_s) ) {
        int8_t c1 = __hlt_iterator_bytes_deref(cur_b, excpt, ctx);
        int8_t c2 = __hlt_iterator_bytes_deref(cur_s, excpt, ctx);

        if ( c1 != c2 )
            return 0;

        __hlt_iterator_bytes_incr(&cur_b, excpt, ctx, 0);
        __hlt_iterator_bytes_incr(&cur_s, excpt, ctx, 0);
    }

    return 1;
}

static int8_t split1(hlt_bytes_pair* result, hlt_bytes* b, hlt_bytes* sep, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes_size sep_len = hlt_bytes_len(sep, excpt, ctx);

    hlt_iterator_bytes i;
    hlt_iterator_bytes j;

    if ( sep_len ) {
        hlt_iterator_bytes sep_end;
        __hlt_bytes_end(&sep_end, sep, excpt, ctx);

        __hlt_bytes_find_bytes(&i, b, sep, excpt, ctx);

        if ( __is_end(i) )
            goto not_found;

        j = i;
        __hlt_iterator_bytes_incr_by(&j, sep_len, excpt, ctx, 0);
    }

    else {
        int looking_for_ws = 1;

        // Special-case: empty separator, split at white-space.
        for ( hlt_bytes* c = b; c && ! __get_object(c); c = c->next ) {
            for ( int8_t* p = c->start; p < c->end; p++ ) {
                if ( looking_for_ws ) {
                    if ( isspace(*p) ) {
                        i.bytes = c;
                        i.cur = p;
                        j = i;
                        __hlt_iterator_bytes_incr(&j, excpt, ctx, 0);
                        looking_for_ws = 0;
                    }
                }

                else {
                    if ( ! isspace(*p) ) {
                        j.bytes = c;
                        j.cur = p;
                        goto done;
                    }
                }
            }
        }

done:
        if ( looking_for_ws )
            goto not_found;
    }

    hlt_iterator_bytes begin;
    hlt_iterator_bytes end;

    __hlt_bytes_begin(&begin, b, excpt, ctx);
    __hlt_bytes_end(&end, b, excpt, ctx);

    result->first = hlt_bytes_sub(begin, i, excpt, ctx);
    result->second = hlt_bytes_sub(j, end, excpt, ctx);

    return 1;

not_found:
    // Not found, return (b, b"").
    GC_CCTOR(b, hlt_bytes);
    result->first = b;
    result->second = hlt_bytes_new(excpt, ctx);
    return 0;
}

hlt_bytes_pair hlt_bytes_split1(hlt_bytes* b, hlt_bytes* sep, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes_pair result;
    split1(&result, b, sep, excpt, ctx);
    return result;
}

hlt_vector* hlt_bytes_split(hlt_bytes* b, hlt_bytes* sep, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_bytes* def = hlt_bytes_new(excpt, ctx);
    hlt_vector* v = hlt_vector_new(&hlt_type_info_hlt_bytes, &def, 0, excpt, ctx);
    GC_DTOR(def, hlt_bytes);

    GC_CCTOR(b, hlt_bytes);

    while ( 1 ) {
        hlt_bytes_pair result;
        int found = split1(&result, b, sep, excpt, ctx);

        if ( ! found ) {
            GC_DTOR(result.first, hlt_bytes);
            GC_DTOR(result.second, hlt_bytes);
            hlt_vector_push_back(v, &hlt_type_info_hlt_bytes, &b, excpt, ctx);
            break;
        }

        hlt_vector_push_back(v, &hlt_type_info_hlt_bytes, &result.first, excpt, ctx);
        GC_DTOR(result.first, hlt_bytes);

        GC_DTOR(b, hlt_bytes);
        b = result.second;
    }

    GC_DTOR(b, hlt_bytes);

    return v;
}

static inline int strip_it(int8_t ch, hlt_bytes* pat)
{
    if ( ! pat )
        return isspace(ch);

    for ( hlt_bytes* c = pat; c && ! __get_object(c); c = c->next ) {
        if ( memchr(c->start, ch, c->end - c->start) )
            return 1;
    }

    return 0;
}

hlt_bytes* hlt_bytes_strip(hlt_bytes* b, hlt_enum side, hlt_bytes* pat, hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_iterator_bytes start;
    hlt_iterator_bytes end;

    hlt_bytes* c = 0;
    int8_t* p = 0;

    if ( hlt_enum_equal(side, Hilti_Side_Left, excpt, ctx) ||
         hlt_enum_equal(side, Hilti_Side_Both, excpt, ctx) ) {

        for ( c = b; c && ! __get_object(c); c = c->next ) {
            for ( p = c->start; p < c->end; p++ ) {
                if ( ! strip_it(*p, pat) )
                    goto end_loop1;
            }
        }

end_loop1:
        start = __create_iterator(c, p);
    }

    else
        start = __create_iterator(b, b->start);

    if ( hlt_enum_equal(side, Hilti_Side_Right, excpt, ctx) ||
         hlt_enum_equal(side, Hilti_Side_Both, excpt, ctx) ) {

        for ( c = __tail(b, false); c && ! __get_object(c); c = __pred(b, c) ) {
            for ( p = c->end - 1; p >= c->start; --p ) {
                if ( ! strip_it(*p, pat) ) {
                    ++p;
                    goto end_loop2;
                }
            }
        }

end_loop2:
        end = __create_iterator(c, p);
    }

    else
        end = GenericEndPos;

    return hlt_bytes_sub(start, end, excpt, ctx);
}

hlt_bytes* hlt_bytes_join(hlt_bytes* sep, hlt_list* l, hlt_exception** excpt, hlt_execution_context* ctx)
{
    const hlt_type_info* ti = hlt_list_type(l, excpt, ctx);
    hlt_iterator_list i = hlt_list_begin(l, excpt, ctx);
    hlt_iterator_list end = hlt_list_end(l, excpt, ctx);

    int first = 1;
    hlt_bytes* b = hlt_bytes_new(excpt, ctx);

    while ( ! hlt_iterator_list_eq(i, end, excpt, ctx) ) {

        if ( ! first )
            hlt_bytes_append(b, sep, excpt, ctx);

        // FIXME: The charset handling isn't really right here. We should
        // probably change the to_string methods to take a flag indicating
        // "binary is ok", and then have a hlt_bytes_from_object() function
        // that just passes that back. Or maybe we even need a separate
        // to_bytes() function?
        void* obj = hlt_iterator_list_deref(i, excpt, ctx);

        __hlt_pointer_stack seen;
        __hlt_pointer_stack_init(&seen);
        hlt_string so = hlt_object_to_string(ti, obj, 0, &seen, excpt, ctx);
        __hlt_pointer_stack_destroy(&seen);

        hlt_bytes* bo = hlt_string_encode(so, Hilti_Charset_UTF8, excpt, ctx);
        hlt_bytes_append(b, bo, excpt, ctx);

        GC_DTOR_GENERIC(obj, ti);
        GC_DTOR(so, hlt_string);
        GC_DTOR(bo, hlt_bytes);

        hlt_iterator_list j = i;
        i = hlt_iterator_list_incr(i, excpt, ctx);
        GC_DTOR(j, hlt_iterator_list);

        first = 0;
    }

    GC_DTOR(i, hlt_iterator_list);
    GC_DTOR(end, hlt_iterator_list);

    return b;
}

void hlt_bytes_append_object(hlt_bytes* b, const hlt_type_info* type, void* obj, hlt_exception** excpt, hlt_execution_context* ctx)
{
    assert(type);

    if ( ! (b && obj) ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0);
        return;
    }

    hlt_bytes* c = __hlt_bytes_new_object(type, obj, ctx);
    __add_chunk(__tail(b, true), c);

    hlt_thread_mgr_unblock(&b->blockable, ctx);
}

void* hlt_bytes_retrieve_object(hlt_iterator_bytes i, const hlt_type_info* type, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&i);

    if ( ! __at_object(i) ) {
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return 0;
    }

    __hlt_bytes_object* o = __get_object(i.bytes);
    assert(o && o->type);

    if ( type && ! hlt_type_equal(type, o->type) ) {
        hlt_set_exception(excpt, &hlt_exception_type_error, 0);
        return 0;
    }

    GC_CCTOR_GENERIC(&o->object, o->type);
    return &o->object;
}

int8_t hlt_bytes_at_object(hlt_iterator_bytes i, const hlt_type_info* type, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&i);
    return __at_object(i);
}

int8_t hlt_bytes_at_object_of_type(hlt_iterator_bytes i, const hlt_type_info* type, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&i);

    __hlt_bytes_object* o = __get_object(i.bytes);

    if ( ! o )
        return false;

    return type == o->type;
}

hlt_iterator_bytes hlt_bytes_skip_object(hlt_iterator_bytes old, hlt_exception** excpt, hlt_execution_context* ctx)
{
    __normalize_iter(&old);

    if ( ! __at_object(old) ) {
        hlt_set_exception(excpt, &hlt_exception_value_error, 0);
        return GenericEndPos;
    }

    hlt_iterator_bytes ni = old;
    GC_CCTOR(ni, hlt_iterator_bytes);

    GC_ASSIGN(ni.bytes, old.bytes->next, hlt_bytes);

    if ( ni.bytes )
        ni.cur = ni.bytes->start;

    return ni;
}
