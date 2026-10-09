/* OSFatal's colors/message become a portable terminal diagnostic. The ABI
 * adapter reports guest/native traces and terminates the process. */
#include "mh3_system_fatal.h"
#include <stdio.h>
#include <string.h>

mh3_res_status mh3_system_fatal_format(mh3_res_span foreground,mh3_res_span background,
    mh3_res_span message,mh3_res_mut_span output,size_t *required) {
    if(!required) return MH3_RES_INVALID;
    if(!foreground.data || foreground.size<4 || !background.data || background.size<4 ||
       !message.data || !message.size) return MH3_RES_RANGE;
    const uint8_t *end=memchr(message.data,0,message.size);
    if(!end) return MH3_RES_INVALID;
    const size_t length=(size_t)(end-message.data);
    char prefix[64];
    const int count=snprintf(prefix,sizeof(prefix),
        "MH3 OSFatal (fg=%02X%02X%02X%02X bg=%02X%02X%02X%02X): ",
        (unsigned)foreground.data[0],(unsigned)foreground.data[1],
        (unsigned)foreground.data[2],(unsigned)foreground.data[3],
        (unsigned)background.data[0],(unsigned)background.data[1],
        (unsigned)background.data[2],(unsigned)background.data[3]);
    if(count<0 || (size_t)count>=sizeof(prefix) || length>SIZE_MAX-(size_t)count-1)
        return MH3_RES_RANGE;
    *required=(size_t)count+length+1;
    if(!output.data || output.size<*required) return MH3_RES_RANGE;
    memmove(output.data+(size_t)count,message.data,length+1);
    memcpy(output.data,prefix,(size_t)count);
    return MH3_RES_OK;
}
