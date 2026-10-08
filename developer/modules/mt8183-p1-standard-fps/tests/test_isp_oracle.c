/* SPDX-License-Identifier: GPL-2.0-only */
#include "camera_softisp.h"
#include "duet_softisp.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
 u8 *raw=malloc(2400000),*actual=malloc(960000),*oracle=malloc(960000);assert(raw&&actual&&oracle);
 struct duet_isp_config c;struct duet_isp *isp;duet_isp_config_default(&c,1600,1200);c.format=DUET_ISP_YUYV;assert(!duet_isp_create(&isp,&c));
 unsigned int seed=17;for(int k=0;k<3;k++){
  for(int i=0;i<2400000;i++){seed=seed*1664525U+1013904223U;raw[i]=(u8)(seed>>24);}
  assert(!dcv_softisp(raw,2400000,actual,960000));assert(!duet_isp_process(isp,raw,2400000,oracle,960000));
  assert(!memcmp(actual,oracle,960000));
 }
 duet_isp_destroy(isp);free(oracle);free(actual);free(raw);puts("PASS integer kernel ISP vs existing userspace ISP: 3 random synthetic full frames, 2880000 output bytes identical");
}
