CUDA_PATH = @CUDA_PATH@

AM_CPPFLAGS  = -I$(srcdir)
AM_CPPFLAGS += -I$(srcdir)/include
AM_CPPFLAGS += -I$(srcdir)/src
#AM_CPPFLAGS += -I$(CUDA_PATH)/include
AM_CPPFLAGS += -D__STDC_FORMAT_MACROS

#AM_LDFLAGS   = -L$(CUDA_PATH)/lib64
LIBGDSTOOLS = @LIBGDSTOOLS@

lib_LTLIBRARIES = src/libgdsync.la

ACLOCAL_AMFLAGS = -Iconfig
AM_CFLAGS = -g -Wall

EXTRA_DIST = autogen.sh

include_HEADERS = include/gdsync.h
libgdsyncincludedir = $(includedir)/gdsync
libgdsyncinclude_HEADERS = include/gdsync/core.h include/gdsync/device.cuh  include/gdsync/mlx5.h include/gdsync/tools.h

src_libgdsync_la_CFLAGS = $(AM_CFLAGS)
src_libgdsync_la_SOURCES = src/gdsync.cpp src/memmgr.cpp src/mem.cpp src/objs.cpp src/apis.cpp src/mlx5.cpp include/gdsync.h 
src_libgdsync_la_LDFLAGS = -version-info 2:0:1

noinst_HEADERS = src/mem.hpp src/memmgr.hpp src/objs.hpp src/rangeset.hpp src/utils.hpp src/archutils.h src/mlnxutils.h

# if enabled at configure time

if TEST_ENABLE

bin_PROGRAMS = tests/gds_kernel_latency tests/gds_poll_lat tests/gds_kernel_loopback_latency tests/gds_sanity
noinst_PROGRAMS = tests/rstest

tests_gds_kernel_latency_SOURCES = tests/gds_kernel_latency.c tests/gpu_kernels.cu tests/pingpong.c tests/gpu.cpp
tests_gds_kernel_latency_LDADD = $(top_builddir)/src/libgdsync.la -lmpi $(LIBGDSTOOLS) -lgdrapi -lcuda -lcudart

tests_rstest_SOURCES = tests/rstest.cpp
tests_rstest_LDADD = 

#tests_gds_poll_lat_CFLAGS = -DUSE_PROF -DUSE_PERF -I/ivylogin/home/drossetti/work/p4/cuda_a/sw/dev/gpu_drv/cuda_a/drivers/gpgpu/cuda/inc
#tests_gds_poll_lat_SOURCES = tests/gds_poll_lat.c tests/gpu.cpp tests/gpu_kernels.cu tests/perfutil.c tests/perf.c
tests_gds_poll_lat_SOURCES = tests/gds_poll_lat.c tests/gpu.cpp tests/gpu_kernels.cu
tests_gds_poll_lat_LDADD = $(top_builddir)/src/libgdsync.la $(LIBGDSTOOLS) -lgdrapi -lmpi -lcuda -lcudart

tests_gds_sanity_SOURCES = tests/gds_sanity.c tests/gpu.cpp tests/gpu_kernels.cu
tests_gds_sanity_LDADD = $(top_builddir)/src/libgdsync.la $(LIBGDSTOOLS) -lgdrapi -lmpi -lcuda -lcudart

tests_gds_kernel_loopback_latency_SOURCES = tests/gds_kernel_loopback_latency.c tests/pingpong.c tests/gpu.cpp tests/gpu_kernels.cu
tests_gds_kernel_loopback_latency_LDADD = $(top_builddir)/src/libgdsync.la $(LIBGDSTOOLS) -lgdrapi -lcuda -lcudart


SUFFIXES= .cu

.cu.o:
	$(NVCC) $(CPPFLAGS) $(NVCCFLAGS)  -c -o $@ $<


.cu.lo:
	$(LIBTOOL) --tag=CC --mode=compile $(NVCC) -o $@ -c $< $(CPPFLAGS) $(NVCCFLAGS)

endif
