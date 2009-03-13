/* $Id$
 * 
 * Private interface that is supposed to be used only from other libhilti
 * functions, or from code generated by the HILTI compiler. In other words,
 * the elements of this interface are never exposed to a HILTI user. 
 * 
 */

#ifndef HILTI_INTERN_H
#define HILTI_INTERN_H

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "hilti.h"


///////////////////////////////////////////////////////////////////////////////
// Predefined exceptions.
///////////////////////////////////////////////////////////////////////////////

    // %doc-std-exceptions-start
// A division by zero has occured.
extern __hlt_exception __hlt_exception_division_by_zero;

// A value looks different than expected. 
extern __hlt_exception __hlt_exception_value_error;

// A function received different arguments than it expected. 
extern __hlt_exception __hlt_exception_wrong_arguments;

// An memory allocation has failed due to resource exhaustion.
extern __hlt_exception __hlt_exception_out_of_memory;

// Fall-back exception if nothing else is specified.
extern __hlt_exception __hlt_exception_unspecified;
    // %doc-std-exceptions-end

// Internal function to report an uncaugt exception.
extern void __hlt_exception_print_uncaught(__hlt_exception exception);

///////////////////////////////////////////////////////////////////////////////
// Run-time information about HILTI types.
///////////////////////////////////////////////////////////////////////////////

   // %doc-__HLT_TYPE-start
// Unique id values to identify a type. These numbers must match the type's
// _id class member in the Python module hilti.core.Type.
#define __HLT_TYPE_ERROR   0 
#define __HLT_TYPE_INTEGER 1 
#define __HLT_TYPE_DOUBLE  2 
#define __HLT_TYPE_BOOL    3 
#define __HLT_TYPE_STRING  4 
#define __HLT_TYPE_TUPLE   5
   // %doc-__HLT_TYPE-end

   // %doc-hlt_type_info-start
struct __hlt_type_info {

    // The type's __HLT_TYPE_* id.
    int16_t type; 
    
    // A readable version of the type's name. 
    const char* tag;

    // Number of type parameters.
    int16_t num_params;
    
    // List of type operations defined in libhilti functions. Pointers may be
    // zero to indicate that a type does not support an operation. 
    
    // Returns a readable representation of a value. 'type' is the type
    // information for the type being converted. 'obj' is a pointer to the
    // value stored with the C type as HILTI uses normally for values of that
    // type. 'options' is currently unused and will be always zero for now.
    // In the future, we might use 'options' to pass in hints about the
    // prefered format. 'expt' can be set to raise an exception.
    __hlt_string* (*to_string)(const __hlt_type_info* type, void* obj, int32_t options, __hlt_exception* expt);
    int64_t (*to_int64)(const __hlt_type_info* type, void* obj, __hlt_exception* expt);
    double (*to_double)(const __hlt_type_info* type, void* obj, __hlt_exception* expt);
    
    // Type-parameters start here. The format is type-specific.
    char type_params[];
};
   // %doc-hlt_type_info-end

///////////////////////////////////////////////////////////////////////////////
// Replacement functions for malloc/calloc/realloc that provide automatic
// garbage collection.
///////////////////////////////////////////////////////////////////////////////

// Allocates n bytes of memory and promises that they will never contain any
// pointers.
extern void* __hlt_gc_malloc_atomic(size_t n);

// Allocates n bytes of memory, which may be used to store pointers to other
// objects. 
// 
// Todo: At some point, we'll likely change this interface to require a
// pointer map to be passed in.
extern void* __hlt_gc_malloc_non_atomic(size_t n);

// Allocates n bytes of zero-initialized memory and promises that they will
// never contain any pointers.
extern void* __hlt_gc_calloc_atomic(size_t count, size_t n);

// Allocates n bytes of zero-initialized memory, which may be used to store
// pointers to other objects. 
// 
// Todo: At some point, we'll likely change this interface to require a
// pointer map to be passed in.
extern void* __hlt_gc_calloc_non_atomic(size_t count, size_t n);

// Reallocates the memory to a chunk of size n and promises that they never
// contain any pointers. The original memory must have been allocated with a
// *_atomic function as well.
extern void* __hlt_gc_realloc_atomic(void* ptr, size_t n);

// Reallocates the memory to a chunk of size n, which may be used to store
// pointers to other objects. The original memory must have been allocated
// with a *_non_atomic function as well.
extern void* __hlt_gc_realloc_non_atomic(void* ptr, size_t n);

///////////////////////////////////////////////////////////////////////////////
// Support functions for HILTI's integer data type.
///////////////////////////////////////////////////////////////////////////////

extern const __hlt_string* __hlt_int_to_string(const __hlt_type_info* type, void* obj, int32_t options, __hlt_exception* excpt);
extern int64_t __hlt_int_to_int64(const __hlt_type_info* type, void* obj, __hlt_exception* expt);

///////////////////////////////////////////////////////////////////////////////
// Support functions for HILTI's double data type.
///////////////////////////////////////////////////////////////////////////////

extern const __hlt_string* __hlt_double_to_string(const __hlt_type_info* type, void* obj, int32_t options, __hlt_exception* excpt);
extern double __hlt_double_to_double(const __hlt_type_info* type, void* obj, __hlt_exception* expt);

///////////////////////////////////////////////////////////////////////////////
// Support functions for HILTI's boolean data type.
///////////////////////////////////////////////////////////////////////////////

extern const __hlt_string* __hlt_bool_to_string(const __hlt_type_info* type, void* obj, int32_t options, __hlt_exception* excpt);
extern int64_t __hlt_bool_to_int64(const __hlt_type_info* type, void* obj, __hlt_exception* expt);

///////////////////////////////////////////////////////////////////////////////
// Support functions for HILTI's string data type.
///////////////////////////////////////////////////////////////////////////////

    // %doc-hlt_string-start
typedef int32_t __hlt_string_size;

struct __hlt_string {
    __hlt_string_size len;
    int8_t bytes[];
} __attribute__((__packed__));
    // %doc-hlt_string-end

extern const __hlt_string* __hlt_string_to_string(const __hlt_type_info* type, void* obj, int32_t options, __hlt_exception* excpt);
extern __hlt_string_size __hlt_string_len(const __hlt_string* s, __hlt_exception* excpt);
extern const __hlt_string* __hlt_string_concat(const __hlt_string* s1, const __hlt_string* s2, __hlt_exception* excpt);
extern const __hlt_string* __hlt_string_substr(const __hlt_string* s1, __hlt_string_size pos, __hlt_string_size len, __hlt_exception* excpt);
extern __hlt_string_size __hlt_string_find(const __hlt_string* s, const __hlt_string* pattern, __hlt_exception* excpt);
extern int __hlt_string_cmp(const __hlt_string* s1, const __hlt_string* s2, __hlt_exception* excpt);
extern const __hlt_string* __hlt_string_sprintf(const __hlt_string* fmt, const __hlt_type_info* type, void* (*tuple[]), __hlt_exception* excpt);

extern const __hlt_string* __hlt_string_from_asciiz(const char* asciiz, __hlt_exception* excpt);
extern const __hlt_string* __hlt_string_from_data(const int8_t* data, __hlt_string_size len, __hlt_exception* excpt);

///////////////////////////////////////////////////////////////////////////////
// Support functions for HILTI's tuple data type.
///////////////////////////////////////////////////////////////////////////////

extern const __hlt_string* __hlt_tuple_to_string(const __hlt_type_info* type, void* (*obj[]), int32_t options, __hlt_exception* excpt);

#endif    
