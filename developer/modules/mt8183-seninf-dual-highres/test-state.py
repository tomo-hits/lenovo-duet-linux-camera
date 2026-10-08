#!/usr/bin/env python3
# Modified on 2026-10-05: explicit publication license / self-contained regressions.
# SPDX-License-Identifier: GPL-2.0-only
"""Host-test extracted capture control functions; no receiver/HW validation."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import tempfile

base = Path(__file__).resolve().parent
source = (base / "mtk_seninf.c").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\(", source, re.M)
    assert match
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "mtk_seninf_def.h"
#include "mtk_seninf_reg.h"
#define __iomem
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define WARN_ON(x) (!!(x))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define MEDIA_BUS_FMT_SRGGB10_1X10 0x300f
#define V4L2_FIELD_ANY 0
#define V4L2_FIELD_NONE 1
#define V4L2_COLORSPACE_SRGB 8
#define V4L2_XFER_FUNC_DEFAULT 0
#define V4L2_YCBCR_ENC_DEFAULT 0
#define V4L2_QUANTIZATION_DEFAULT 0
#define V4L2_SUBDEV_FORMAT_TRY 0
#define V4L2_SUBDEV_FORMAT_ACTIVE 1
typedef uint32_t u32;
struct v4l2_mbus_framefmt { u32 code,width,height,field,colorspace,xfer_func,ycbcr_enc,quantization; };
struct v4l2_subdev_format { u32 which,pad; struct v4l2_mbus_framefmt format; };
struct v4l2_subdev_state { struct v4l2_mbus_framefmt formats[NUM_PADS]; };
struct v4l2_subdev_mbus_code_enum { u32 pad,index,code; };
struct v4l2_subdev { int dummy; };
struct v4l2_ctrl { int dummy; };
struct device { struct { int lock,usage_count,suspended; } power; };
struct mtk_seninf {
 struct v4l2_subdev subdev; struct device *dev; void *base_reg;
 void *csi2_rx[CFG_CSI_PORT_MAX_NUM]; unsigned int port,mux_sel;
 struct { unsigned short num_data_lanes; } sensor[NUM_SENSORS];
 struct v4l2_subdev_format fmt[NUM_PADS]; int lock; bool streaming; int stop_error;
};
static int locks, pm_locks, pm_gets, pm_puts, writes, checks, on_ret, put_ret, put_suspended;
static bool probe_only;
#define CHECK(x) do { ++checks; assert(x); } while(0)
#define mutex_lock(p) do { (void)(p); assert(!locks++); } while(0)
#define mutex_unlock(p) do { (void)(p); assert(locks-- == 1); } while(0)
#define spin_lock_irqsave(p,f) do { (void)(p); (f)=0; assert(!pm_locks++); } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(p); (void)(f); assert(pm_locks-- == 1); } while(0)
#define atomic_read(p) (*(p))
static bool pm_runtime_suspended(struct device *d) { assert(pm_locks); return d->power.suspended; }
static int pm_runtime_put_sync_suspend(struct device *d) {
 assert(locks && !pm_locks && d->power.usage_count > 0);
 ++pm_puts; --d->power.usage_count; d->power.suspended=put_suspended; return put_ret;
}
static u32 readl(void *p) { u32 n; memcpy(&n,p,4); return n; }
static void writel(u32 n,void *p) { ++writes; memcpy(p,&n,4); }
static struct v4l2_mbus_framefmt *v4l2_subdev_state_get_format(struct v4l2_subdev_state *s,u32 p) { return &s->formats[p]; }
static int mtk_seninf_power_on(struct mtk_seninf *p) {
 if(on_ret) return on_ret;
 ++pm_gets; ++p->dev->power.usage_count; p->dev->power.suspended=0; return 0;
}
'''
default = re.search(r"static const struct v4l2_mbus_framefmt mtk_seninf_default_fmt = \{.*?\n\};", source, re.S).group()
functions = ["mtk_seninf_csi_port_to_seninf", "mtk_seninf_power_off",
             "seninf_s_stream", "seninf_set_fmt", "seninf_get_fmt",
             "seninf_enum_mbus_code", "seninf_set_ctrl"]
body = r'''
static unsigned char registers[0x8000], rx[0x6000];
static struct device dev;
static struct mtk_seninf p;
static void reset(void) {
 memset(&p,0,sizeof(p)); memset(&dev,0,sizeof(dev));
 memset(registers,0xff,sizeof(registers)); memset(rx,0xff,sizeof(rx));
 p.dev=&dev; p.base_reg=registers; p.port=CFG_CSI_PORT_1;
 for (int i=0;i<CFG_CSI_PORT_MAX_NUM;i++) p.csi2_rx[i]=rx;
 p.sensor[CFG_CSI_PORT_1].num_data_lanes=1;
 pm_gets=pm_puts=writes=locks=pm_locks=on_ret=put_ret=0; put_suspended=1; probe_only=false;
}
int main(void) {
 reset(); probe_only=true;
 CHECK(seninf_s_stream(&p.subdev,1)==-EACCES); CHECK(!pm_gets && !writes);
 reset(); p.port=NUM_SENSORS;
 CHECK(seninf_s_stream(&p.subdev,1)==-ENOLINK); CHECK(!pm_gets && !writes);
 reset(); p.sensor[1].num_data_lanes=2;
 CHECK(seninf_s_stream(&p.subdev,1)==-ENOLINK);
 reset(); p.mux_sel=1; CHECK(seninf_s_stream(&p.subdev,1)==-ENOLINK);
 reset(); on_ret=-EIO;
 CHECK(seninf_s_stream(&p.subdev,1)==-EIO); CHECK(!p.streaming && !pm_gets && !pm_puts);
 CHECK(!seninf_s_stream(&p.subdev,0) && !pm_puts);
 reset(); CHECK(!seninf_s_stream(&p.subdev,0) && !pm_puts && !writes);
 CHECK(!seninf_s_stream(&p.subdev,1)); CHECK(p.streaming && pm_gets==1);
 CHECK(!seninf_s_stream(&p.subdev,1) && pm_gets==1);
 CHECK(!seninf_s_stream(&p.subdev,0)); CHECK(!p.streaming && !p.stop_error && pm_puts==1 && dev.power.usage_count==0);
 CHECK(readl(registers+0x2000+SENINF1_CSI2_INT_EN)==0);
 CHECK(readl(registers+0x2000+SENINF1_CSI2_INT_EN_EXT)==0);
 CHECK(readl(registers+SENINF1_MUX_INTEN)==0);
 CHECK((readl(registers+0x2000+SENINF1_CSI2_CTL)&31)==0);
 CHECK((readl(rx+MIPI_RX_ANA00_CSI0A)&12)==0 && (readl(rx+MIPI_RX_ANA00_CSI0B)&12)==0);
 int n=writes; CHECK(!seninf_s_stream(&p.subdev,0) && pm_puts==1 && writes==n);
 int errors[]={-EIO,-ETIMEDOUT,-EBUSY,-EAGAIN,-EACCES};
 for(unsigned i=0;i<ARRAY_SIZE(errors);i++) {
  reset(); CHECK(!seninf_s_stream(&p.subdev,1)); put_ret=errors[i]; put_suspended=0;
  CHECK(seninf_s_stream(&p.subdev,0)==errors[i]);
  CHECK(!p.streaming && p.stop_error==errors[i] && pm_puts==1 && dev.power.usage_count==0);
  n=writes; CHECK(seninf_s_stream(&p.subdev,0)==errors[i] && pm_puts==1 && writes==n);
  CHECK(seninf_s_stream(&p.subdev,1)==errors[i] && pm_gets==1 && writes==n);
 }
 reset(); CHECK(!seninf_s_stream(&p.subdev,1)); put_suspended=0;
 CHECK(seninf_s_stream(&p.subdev,0)==-EBUSY && pm_puts==1 && !p.streaming);
 reset(); CHECK(!seninf_s_stream(&p.subdev,1)); ++dev.power.usage_count;
 CHECK(seninf_s_stream(&p.subdev,0)==-EBUSY && pm_puts==1 && dev.power.usage_count==1);
 reset(); CHECK(!seninf_s_stream(&p.subdev,1)); put_ret=1;
 CHECK(!seninf_s_stream(&p.subdev,0) && !p.stop_error && pm_puts==1);
 reset(); struct v4l2_subdev_state state={0};
 struct v4l2_subdev_format fmt={.which=V4L2_SUBDEV_FORMAT_ACTIVE,.pad=1,.format=mtk_seninf_default_fmt};
 CHECK(!seninf_set_fmt(&p.subdev,NULL,&fmt)); CHECK(p.fmt[1].format.code==MEDIA_BUS_FMT_SRGGB10_1X10);
 fmt.pad=4; CHECK(!seninf_set_fmt(&p.subdev,NULL,&fmt));
 fmt.which=V4L2_SUBDEV_FORMAT_TRY; CHECK(!seninf_set_fmt(&p.subdev,&state,&fmt)); CHECK(state.formats[4].width==1600);
 fmt.which=V4L2_SUBDEV_FORMAT_ACTIVE; fmt.format.code=0; CHECK(seninf_set_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 fmt.format=mtk_seninf_default_fmt; fmt.format.width=1599; CHECK(seninf_set_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 fmt.format=mtk_seninf_default_fmt; fmt.format.height=1201; CHECK(seninf_set_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 fmt.format=mtk_seninf_default_fmt; fmt.format.field=2; CHECK(seninf_set_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 fmt.format=mtk_seninf_default_fmt; fmt.pad=NUM_PADS; CHECK(seninf_set_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 fmt.pad=1; fmt.which=9; CHECK(seninf_set_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 fmt.which=V4L2_SUBDEV_FORMAT_ACTIVE; p.streaming=true; CHECK(seninf_set_fmt(&p.subdev,NULL,&fmt)==-EBUSY);
 struct v4l2_subdev_mbus_code_enum code={.pad=4,.index=0};
 CHECK(!seninf_enum_mbus_code(&p.subdev,&state,&code) && code.code==MEDIA_BUS_FMT_SRGGB10_1X10);
 code.index=1; CHECK(seninf_enum_mbus_code(&p.subdev,&state,&code)==-EINVAL);
 fmt.pad=1; fmt.which=V4L2_SUBDEV_FORMAT_ACTIVE;
 CHECK(!seninf_get_fmt(&p.subdev,NULL,&fmt) && fmt.format.width==1600 && fmt.format.code==MEDIA_BUS_FMT_SRGGB10_1X10);
 fmt.which=9; CHECK(seninf_get_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 fmt.which=V4L2_SUBDEV_FORMAT_ACTIVE; fmt.pad=NUM_PADS; CHECK(seninf_get_fmt(&p.subdev,NULL,&fmt)==-EINVAL);
 struct v4l2_ctrl ctrl={0}; n=writes;
 CHECK(seninf_set_ctrl(&ctrl)==-EOPNOTSUPP && writes==n);
 CHECK(!locks && !pm_locks);
 printf("%d\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix="seninf-capture-") as directory:
    target = Path(directory)
    (target / "test.c").write_text(prelude + default + "\n" + "\n".join(function(n) for n in functions) + body)
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-fsanitize=address,undefined", "-I", str(base), str(target / "test.c"), "-o", str(target / "test")], check=True)
    count = int(subprocess.check_output([str(target / "test")], text=True).strip())
print(json.dumps({"result": "PASS", "checks": count, "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                  "extracted_functions": functions, "author_test": True,
                  "scope": "Mock PM/MMIO control and strict format only; not KCFI, ARM64, sensor or receiver hardware validation."}, indent=2))
