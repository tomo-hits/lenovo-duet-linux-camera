/* SPDX-License-Identifier: GPL-2.0-only */
#include "camera_softisp.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
 u8 *raw=calloc(1,2400000),*out=calloc(1,960000);assert(raw&&out);
 assert(dcv_softisp(raw,2399999,out,960000)<0);assert(dcv_softisp(raw,2400000,out,959999)<0);
 assert(!dcv_softisp(raw,2400000,out,960000));for(int i=0;i<960000;i++)assert(out[i]==(i%2?128:16));
 memset(raw,255,2400000);assert(!dcv_softisp(raw,2400000,out,960000));for(int i=0;i<960000;i++)assert(out[i]==(i%2?128:235));
 free(out);free(raw);puts("PASS integer ISP strict size, black, white full-frame boundaries (no camera)");
}
