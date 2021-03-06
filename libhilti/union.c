/*
 *
 * Support functions HILTI's union data type.
 *
 */


#include <assert.h>
#include <stdio.h>

#include "clone.h"
#include "rtti.h"
#include "string_.h"
#include "union.h"

#include <stdio.h>

struct __hlt_union {
    int32_t field; // Index into the aux array describing the field that's set, or -1 if unset.
    int8_t data[]; // Data starts here.
};

// One entry in the type parameter array.
struct __field {
    hlt_type_info* type;
    const char* name; // Null for anonymous fields.
};

hlt_type_info* __hlt_union_type(hlt_type_info* type, void* obj)
{
    hlt_union* u = (hlt_union*)obj;
    struct __field* fields = (struct __field*)type->aux;

    if ( ! u || u->field < 0 )
        return 0;

    return fields[u->field].type;
}

void hlt_union_cctor(hlt_type_info* type, void* obj, hlt_execution_context* ctx)
{
    assert(type->type == HLT_TYPE_UNION);

    hlt_union* u = (hlt_union*)obj;
    struct __field* fields = (struct __field*)type->aux;

    if ( ! u )
        return;

    if ( u->field >= 0 )
        __hlt_object_cctor(fields[u->field].type, &u->data, "union-cctor", ctx);
}

void hlt_union_dtor(hlt_type_info* type, void* obj, hlt_execution_context* ctx)
{
    assert(type->type == HLT_TYPE_UNION);

    hlt_union* u = (hlt_union*)obj;
    struct __field* fields = (struct __field*)type->aux;

    if ( ! u )
        return;

    if ( u->field >= 0 )
        __hlt_object_dtor(fields[u->field].type, &u->data, "union-dtor", ctx);
}

void hlt_union_clone_init(void* dstp, const hlt_type_info* ti, void* srcp,
                          __hlt_clone_state* cstate, hlt_exception** excpt,
                          hlt_execution_context* ctx)
{
    hlt_union* src = (hlt_union*)srcp;
    hlt_union* dst = (hlt_union*)dstp;

    struct __field* fields = (struct __field*)ti->aux;

    if ( ! src ) {
        hlt_set_exception(excpt, &hlt_exception_null_reference, 0, ctx);
        return;
    }

    dst->field = src->field;

    if ( src->field >= 0 )
        __hlt_clone(&dst->data, fields[src->field].type, &src->data, cstate, excpt, ctx);
}

hlt_string hlt_union_to_string(const hlt_type_info* type, void* obj, int32_t options,
                               __hlt_pointer_stack* seen, hlt_exception** excpt,
                               hlt_execution_context* ctx)
{
    assert(type->type == HLT_TYPE_UNION);

    hlt_union* u = (hlt_union*)obj;
    struct __field* fields = (struct __field*)type->aux;

    if ( ! u )
        return hlt_string_from_asciiz("(Null)", excpt, ctx);

    if ( u->field < 0 )
        return hlt_string_from_asciiz("<unset>", excpt, ctx);

    int num_fields = 0;

    // Count number of fields.
    for ( struct __field *f = fields; f->type; f++, num_fields++ )
        ;

    const char* name = fields[u->field].name;

    if ( name && name[0] && name[1] && name[0] == '_' && name[1] == '_' )
        // Don't print internal names.
        return 0;

    hlt_string s = 0;

    if ( num_fields > 1 )
        s = hlt_string_from_asciiz("<", excpt, ctx);

    if ( name ) {
        hlt_string n = hlt_string_from_asciiz(name, excpt, ctx);
        s = hlt_string_concat(s, n, excpt, ctx);
        n = hlt_string_from_asciiz("=", excpt, ctx);
        s = hlt_string_concat(s, n, excpt, ctx);
    }

    hlt_string t =
        __hlt_object_to_string(fields[u->field].type, &u->data, options, seen, excpt, ctx);
    s = hlt_string_concat(s, t, excpt, ctx);

    if ( num_fields > 1 ) {
        t = hlt_string_from_asciiz(">", excpt, ctx);
        s = hlt_string_concat(s, t, excpt, ctx);
    }

    return s;
}

hlt_hash hlt_union_hash(const hlt_type_info* type, const void* obj, hlt_exception** excpt,
                        hlt_execution_context* ctx)
{
    assert(type->type == HLT_TYPE_UNION);

    hlt_union* u = (hlt_union*)obj;
    struct __field* fields = (struct __field*)type->aux;

    if ( ! u )
        return 0;

    if ( u->field < 0 )
        return 1;

    hlt_type_info* t = fields[u->field].type;
    return (t->hash)(t, &u->data, excpt, ctx);
}

int8_t hlt_union_equal(const hlt_type_info* type1, const void* obj1, const hlt_type_info* type2,
                       const void* obj2, hlt_exception** excpt, hlt_execution_context* ctx)
{
    assert(type1->type == HLT_TYPE_UNION);
    assert(type1->type == type2->type);

    hlt_union* u1 = (hlt_union*)obj1;
    hlt_union* u2 = (hlt_union*)obj2;

    struct __field* fields = (struct __field*)type1->aux;

    if ( ! u1 )
        return ! u2;

    if ( ! u2 )
        return ! u1;

    if ( u1->field != u2->field )
        return 0;

    hlt_type_info* t = fields[u1->field].type;
    return (t->equal)(t, &u1->data, t, &u2->data, excpt, ctx);
}

int8_t hlt_union_is_set(hlt_union* u, int64_t idx, hlt_exception** excpt,
                        hlt_execution_context* ctx)
{
    if ( idx < 0 )
        return u->field >= 0;

    return u->field == idx;
}

void* hlt_union_get(hlt_union* u, int64_t idx, hlt_exception** excpt, hlt_execution_context* ctx)
{
    if ( idx < 0 || u->field == idx )
        return (u->field >= 0) ? &u->data : 0;
    else
        return 0;
}

hlt_union_field hlt_union_get_type(const hlt_type_info* type, hlt_union* u, int idx,
                                   hlt_exception** excpt, hlt_execution_context* ctx)
{
    hlt_union_field f = {0, 0};

    if ( type->type != HLT_TYPE_UNION ) {
        hlt_set_exception(excpt, &hlt_exception_type_error, 0, ctx);
        return f;
    }

    if ( idx < 0 ) {
        if ( ! u ) {
            hlt_set_exception(excpt, &hlt_exception_value_error, 0, ctx);
            return f;
        }

        if ( u->field < 0 )
            return f;

        idx = u->field;
    }

    struct __field* fields = (struct __field*)type->aux;

    f.type = fields[idx].type;
    f.name = fields[idx].name;
    return f;
}
