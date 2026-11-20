/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding Common V2 HFS+/HFSX byte primitives with no libc dependency.
 */
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *restrict dst,const void *restrict src,size_t n)
{
    unsigned char *d=dst;const unsigned char *s=src;
    for(size_t i=0;i<n;++i)d[i]=s[i];
    return dst;
}
void *memset(void *dst,int value,size_t n)
{
    unsigned char *d=dst;
    for(size_t i=0;i<n;++i)d[i]=(unsigned char)value;
    return dst;
}
int memcmp(const void *a,const void *b,size_t n)
{
    const unsigned char *p=a,*q=b;
    for(size_t i=0;i<n;++i)
        if(p[i]!=q[i])return (int)p[i]-(int)q[i];
    return 0;
}
size_t strlen(const char *str)
{
    size_t i=0;while(str[i])++i;return i;
}
