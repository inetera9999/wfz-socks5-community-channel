#ifndef __VARYENTRY_H__
#define __VARYENTRY_H__



#if defined(__GCC_EXT__) 

#define offset_of(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)

#define onemember_of(ptr, type, member) ({                     \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);   \
        (type *)( (char *)__mptr - offset_of(type,member) );   \
})

#define vbody_entry(ptr, type, member) 	onemember_of(ptr, type, member)
#else
#define offset_of(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)

// 修改后的宏（直接计算指针偏移）
#define onemember_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offset_of(type, member)))

#define vbody_entry(ptr, type, member) onemember_of(ptr, type, member)
#endif


#endif