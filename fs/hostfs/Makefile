#
# Copyright (C) 2000 Jeff Dike (jdike@karaya.com)
# Licensed under the GPL
#

hostfs-objs := hostfs_kern.o

hostfs-builtin-$(CONFIG_HOSTFS) += hostfs_user.o hostfs_user_exp.o

obj-y := $(hostfs-builtin-y) $(hostfs-builtin-m)
obj-$(CONFIG_HOSTFS) += hostfs.o

ifeq ($(CONFIG_LKL),y)
# hostfs_user.c is host-side code: it calls libc (open, pread, readdir, ...) on
# behalf of the guest kernel. UML compiles such files with USER_CFLAGS, which
# replaces the kernel flag set entirely so libc headers remain visible. LKL has
# no UML user/kernel split, so provide the equivalent locally: -nostdinc must
# not apply to this one file, and the UML user.h preinclude is not needed.
USER_CFLAGS_HOSTFS := -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
$(obj)/hostfs_user.o: c_flags = -Wp,-MD,$(depfile) $(USER_CFLAGS_HOSTFS) \
	$(CFLAGS_hostfs_user.o)
else
include $(srctree)/arch/um/scripts/Makefile.rules
endif
