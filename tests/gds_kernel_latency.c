/*
 * GPUDirect Async latency benchmark
 * 
 *
 * based on OFED libibverbs ud_pingpong test.
 * minimally changed to use MPI for bootstrapping, 
 */
/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <assert.h>

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <gdsync.h>

#include "pingpong.h"
#include "gpu.h"
#include "test_utils.h"

//-----------------------------------------------------------------------------

#include <mpi.h>

#define MPI_CHECK(stmt)                                             \
do {                                                                \
    int result = (stmt);                                            \
    if (MPI_SUCCESS != result) {                                    \
        char string[MPI_MAX_ERROR_STRING];                          \
        int resultlen = 0;                                          \
        MPI_Error_string(result, string, &resultlen);               \
        fprintf(stderr, " (%s:%d) MPI check failed with %d (%*s)\n", \
                __FILE__, __LINE__, result, resultlen, string);      \
        exit(-1);                                                   \
    }                                                               \
} while(0)


//-----------------------------------------------------------------------------

struct prof prof;
int prof_idx = 0;

//-----------------------------------------------------------------------------

#if 0
#define dbg(FMT, ARGS...)  do {} while(0)
#else
#define dbg_msg(FMT, ARGS...)   fprintf(stderr, "DBG [%s] " FMT, __FUNCTION__ ,##ARGS)
#define dbg(FMT, ARGS...)  dbg_msg("DBG:  ", FMT, ## ARGS)
#endif

#define min(A,B) ((A)<(B)?(A):(B))

#define USE_CUDA_PROFILER 1

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

static int page_size;

#define MAX_EVENTS 1024
cudaEvent_t start_time[MAX_EVENTS], stop_time[MAX_EVENTS];
float elapsed_time = 0.0;
int event_idx = 0;
int gds_enable_event_prof = 0;
int gds_qpt = IBV_QPT_UD; //UD by default

struct pingpong_context {
	struct ibv_context	*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr;
	struct ibv_cq		*tx_cq;
	struct ibv_cq		*rx_cq;
	struct ibv_qp		*qp;
	struct gds_qp		*gds_qp;
	struct ibv_ah		*ah;
	void			*buf;
	char			*txbuf;
        char                    *rxbuf;
        char                    *rx_flag;
	int			 size;
        int                      calc_size;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
	int                      gpu_id;
	int                      kernel_duration;
	int                      peersync;
	int                      peersync_gpu_cq;
        int                      consume_rx_cqe;
        int                      use_desc_apis;
};

static int my_rank, comm_size;

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};

static inline unsigned long align_to(unsigned long val, unsigned long pow2)
{
	return (val + pow2 - 1) & ~(pow2 - 1);
}


static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size, int calc_size,
					    int rx_depth, int port,
					    int use_event,
					    int gpu_id,
                                            int peersync,
                                            int peersync_gpu_cq,
                                            int peersync_gpu_dbrec,
                                            int consume_rx_cqe,
                                            int sched_mode,
                                            int use_desc_apis)
{
	struct pingpong_context *ctx;

	if (gpu_id >=0 && gpu_init(gpu_id, sched_mode)) {
		fprintf(stderr, "error in GPU init.\n");
		return NULL;
	}

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = size;
	ctx->calc_size = calc_size;
	ctx->rx_depth = rx_depth;
	ctx->gpu_id   = gpu_id;
        ctx->use_desc_apis = use_desc_apis;

        size_t alloc_size = 3 * align_to(size + 40, page_size);
	if (ctx->gpu_id >= 0)
		ctx->buf = gpu_malloc(page_size, alloc_size);
	else
		ctx->buf = memalign(page_size, alloc_size);

	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_ctx;
	}
        printf("ctx buf=%p\n", ctx->buf);
        ctx->rxbuf = (char*)ctx->buf;
        ctx->txbuf = (char*)ctx->buf + align_to(size + 40, page_size);
        //ctx->rx_flag = (char*)ctx->buf + 2 * align_to(size + 40, page_size);

        ctx->rx_flag =  memalign(page_size, alloc_size);
        if (!ctx->rx_flag) {
                fprintf(stderr, "Couldn't allocate rx_flag buf\n");  
                goto clean_ctx;
        }

	ctx->kernel_duration = 0;
	ctx->peersync = peersync;
        ctx->peersync_gpu_cq = peersync_gpu_cq;
        ctx->consume_rx_cqe = consume_rx_cqe;

        // must be ZERO!!! for rx_flag to work...
	if (ctx->gpu_id >= 0)
		gpu_memset(ctx->buf, 0, alloc_size);
	else
		memset(ctx->buf, 0, alloc_size);

        memset(ctx->rx_flag, 0, alloc_size);
        gpu_register_host_mem(ctx->rx_flag, alloc_size);

        // pipe-cleaner
        gpu_launch_kernel(ctx->calc_size, ctx->peersync);
        gpu_launch_kernel(ctx->calc_size, ctx->peersync);
        gpu_launch_kernel(ctx->calc_size, ctx->peersync);
        CUCHECK(cuCtxSynchronize());

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_buffer;
	}

	if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			goto clean_device;
		}
	} else
		ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, alloc_size, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_pd;
	}

        int gds_flags = 0;
        if (peersync_gpu_cq)
                gds_flags |= GDS_CREATE_QP_RX_CQ_ON_GPU;
        if (peersync_gpu_dbrec)
                gds_flags |= GDS_CREATE_QP_WQ_DBREC_ON_GPU;

        gds_qp_init_attr_t attr = {
                .send_cq = 0,
                .recv_cq = 0,
                .cap     = {
                        .max_send_wr  = rx_depth,
                        .max_recv_wr  = rx_depth,
                        .max_send_sge = 1,
                        .max_recv_sge = 1
                },
                .qp_type = gds_qpt,
        };

        ctx->gds_qp = gds_create_qp(ctx->pd, ctx->context, &attr, gpu_id, gds_flags);

        if (!ctx->gds_qp)  {
                fprintf(stderr, "Couldn't create QP\n");
                goto clean_mr;
	}
        ctx->qp = ctx->gds_qp->qp;
        ctx->tx_cq = ctx->gds_qp->qp->send_cq;
        ctx->rx_cq = ctx->gds_qp->qp->recv_cq;

	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qkey            = 0x11111111,
			.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE
		};

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  ((IBV_QPT_UD == gds_qpt) ? IBV_QP_QKEY : IBV_QP_ACCESS_FLAGS))) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
	}

	return ctx;

clean_qp:
	gds_destroy_qp(ctx->gds_qp);

clean_mr:
	ibv_dereg_mr(ctx->mr);

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_device:
	ibv_close_device(ctx->context);

clean_buffer:
	if (ctx->gpu_id >= 0)
		gpu_free(ctx->buf); 
	else 
		free(ctx->buf);

clean_ctx:
	if (ctx->gpu_id >= 0)
		gpu_finalize();
	free(ctx);

	return NULL;
}

int pp_close_ctx(struct pingpong_context *ctx)
{
	if (gds_destroy_qp(ctx->gds_qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
	}

	if (IBV_QPT_UD == gds_qpt) {
		if (ibv_destroy_ah(ctx->ah)) {
			fprintf(stderr, "Couldn't destroy AH\n");
		}
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
	}

	if (ctx->gpu_id >= 0)
		gpu_free(ctx->buf); 
	else 
		free(ctx->buf);

	if (ctx->gpu_id >= 0)
		gpu_finalize();

	free(ctx);

	return 0;
}

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->rxbuf,
		.length = ctx->size + 40,
		.lkey	= ctx->mr->lkey
	};

	if (IBV_QPT_UD != gds_qpt) list.length = ctx->size;
	
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static int pp_post_gpu_send(struct pingpong_context *ctx, uint32_t qpn)
{
        int ret = 0;
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->txbuf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	gds_send_wr ewr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.exp_opcode = IBV_EXP_WR_SEND,
		.exp_send_flags = IBV_EXP_SEND_SIGNALED,
		.wr         = {
			.ud = {
				 .ah          = ctx->ah,
				 .remote_qpn  = qpn,
				 .remote_qkey = 0x11111111
			 }
		},
		.comp_mask = 0
	};
	
	if (IBV_QPT_UD != gds_qpt) {
		memset(&ewr, 0, sizeof(ewr));
		ewr.num_sge = 1;
		ewr.exp_send_flags = IBV_EXP_SEND_SIGNALED;
		ewr.exp_opcode = IBV_EXP_WR_SEND;
		ewr.wr_id = PINGPONG_SEND_WRID;
		ewr.sg_list = &list;
		ewr.next = NULL;
	}

	gds_send_wr *bad_ewr;
	return gds_stream_queue_send(gpu_stream, ctx->gds_qp, &ewr, &bad_ewr);
}

static int pp_prepare_gpu_send(struct pingpong_context *ctx, uint32_t qpn, gds_send_request_t *req)
{
        int ret = 0;
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->txbuf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	gds_send_wr ewr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.exp_opcode = IBV_EXP_WR_SEND,
		.exp_send_flags = IBV_EXP_SEND_SIGNALED,
		.wr         = {
			.ud = {
				 .ah          = ctx->ah,
				 .remote_qpn  = qpn,
				 .remote_qkey = 0x11111111
			 }
		},
		.comp_mask = 0
	};
	
	if (IBV_QPT_UD != gds_qpt) {
		memset(&ewr, 0, sizeof(ewr));
		ewr.num_sge = 1;
		ewr.exp_send_flags = IBV_EXP_SEND_SIGNALED;
		ewr.exp_opcode = IBV_EXP_WR_SEND;
		ewr.wr_id = PINGPONG_SEND_WRID;
		ewr.sg_list = &list;
		ewr.next = NULL;
	}
	gds_send_wr *bad_ewr;
	return gds_prepare_send(ctx->gds_qp, &ewr, &bad_ewr, req);
}


static int pp_post_work(struct pingpong_context *ctx, int n_posts, int rcnt, uint32_t qpn, int is_client)
{
	int i, ret = 0;
        int posted_recv = 0;

        //printf("post_work posting %d\n", n_posts);

        if (n_posts <= 0)
                return 0;

        posted_recv = pp_post_recv(ctx, n_posts);
        if (posted_recv < 0) {
                fprintf(stderr,"ERROR: can't post recv (%d) n_posts=%d is_client=%d\n", 
                        posted_recv, n_posts, is_client);
                exit(EXIT_FAILURE);
                return 0;
        } else if (posted_recv != n_posts) {
                fprintf(stderr,"ERROR: couldn't post all recvs (%d posted, %d requested)\n", posted_recv, n_posts);
                if (!posted_recv)
                        return 0;
        }
        
        PROF(&prof, prof_idx++);

        gds_send_request_t send_rq[posted_recv];
        gds_wait_request_t wait_tx_rq[posted_recv];
        gds_wait_request_t wait_rx_rq[posted_recv];
        gds_descriptor_t descs[2];

	for (i = 0; i < posted_recv; ++i) {
                if (is_client) {
			if (gds_enable_event_prof && (event_idx < MAX_EVENTS)) {
				cudaEventRecord(start_time[event_idx], gpu_stream);
			}
                        if (ctx->use_desc_apis) {
                                int k = 0;
                                ret = pp_prepare_gpu_send(ctx, qpn, &send_rq[i]);
                                if (ret) {
                                        i = -ret;
                                        break;
                                }
                                descs[k].tag = GDS_TAG_SEND;
                                descs[k].send = &send_rq[i];
                                ++k;

                                ret = gds_prepare_wait_cq(&ctx->gds_qp->recv_cq, &wait_rx_rq[i], 0);
                                if (ret) {
                                        i = -ret;
                                        break;
                                }
                                descs[k].tag = GDS_TAG_WAIT;
                                descs[k].wait = &wait_rx_rq[i];
                                ++k;

                                ret = gds_stream_post_descriptors(gpu_stream, k, descs, 0);
                                if (ret) {
                                        i = -ret;
                                        break;
                                }
                        } else {
                                ret = pp_post_gpu_send(ctx, qpn);
                                if (ret) {
                                        fprintf(stderr,"ERROR: can't post GPU send (%d) posted_recv=%d posted_so_far=%d is_client=%d \n",
                                                ret, posted_recv, i, is_client);
                                        i = -ret;
                                        break;
                                }

                                ret = gds_stream_wait_cq(gpu_stream, &ctx->gds_qp->recv_cq, ctx->consume_rx_cqe);
                                if (ret) {
                                        // TODO: rollback gpu send and wait send_cq
                                        fprintf(stderr,"ERROR: error in gpu_post_poll_cq (%d)\n", ret);
                                        i = -ret;
                                        break;
                                }
                        }
			if (gds_enable_event_prof && (event_idx < MAX_EVENTS)) {
				cudaEventRecord(stop_time[event_idx], gpu_stream);
				event_idx++;
			} 
                        if (ctx->calc_size)
                                gpu_launch_kernel(ctx->calc_size, ctx->peersync);
                } else {
                        // no point in using descriptor APIs here, as kernel launch 
                        // would be sitting in between
                        ret = gds_stream_wait_cq(gpu_stream, &ctx->gds_qp->recv_cq, ctx->consume_rx_cqe);
                        if (ret) {
                                fprintf(stderr, "ERROR: error in gpu_post_poll_cq (%d)\n", ret);
                                i = -ret;
                                break;
                        }
                        if (ctx->calc_size)
                                gpu_launch_kernel(ctx->calc_size, ctx->peersync);

			if (gds_enable_event_prof && (event_idx < MAX_EVENTS)) {
				cudaEventRecord(start_time[event_idx], gpu_stream);
			} 
                        ret = pp_post_gpu_send(ctx, qpn);
                        if (ret) {
                                // TODO: rollback gpu send and kernel launch
                                fprintf(stderr, "ERROR: can't post GPU send\n");
                                i = -ret;
                                break;
                        }
			if (gds_enable_event_prof && (event_idx < MAX_EVENTS)) {
				cudaEventRecord(stop_time[event_idx], gpu_stream);
				event_idx++;
			} 
                }
        }

        PROF(&prof, prof_idx++);

        gpu_post_release_tracking_event();
        //sleep(1);
	return i;
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>      size of message to exchange (default 1024)\n");
	printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
	printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
	printf("  -e, --events           sleep on CQ events (default poll)\n");
	printf("  -g, --gid-idx=<gid index> local port gid index\n");
	printf("  -S, --gpu-calc-size=<size>  size of GPU compute buffer (default 128KB)\n");
	printf("  -G, --gpu-id           use specified GPU (default 0)\n");
	printf("  -B, --batch-length=<n> max batch length (default 20)\n");
	printf("  -P, --peersync            enable GPUDirect PeerSync support (default enabled)\n");
	printf("  -C, --peersync-gpu-cq     enable GPUDirect PeerSync GPU CQ support (default disabled)\n");
	printf("  -D, --peersync-gpu-dbrec  enable QP DBREC on GPU memory (default disabled)\n");
	printf("  -U, --peersync-desc-apis  use batched descriptor APIs (default disabled)\n");
	printf("  -Q, --consume-rx-cqe      enable GPU consumes RX CQE support (default disabled)\n");
	printf("  -T, --time-gds-ops        evaluate time needed to execute gds operations using cuda events\n");
	printf("  -K, --qp-kind             select IB transport kind used by GDS QPs. (-K 1) for UD, (-K 2) for RC\n");
	printf("  -M, --gpu-sched-mode      set CUDA context sched mode, default (A)UTO, (S)PIN, (Y)IELD, (B)LOCKING\n");
}

int main(int argc, char *argv[])
{
	struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest     my_dest;
	struct pingpong_dest    *rem_dest = NULL;
	struct timeval           rstart, start, end;
	const char              *ib_devname = NULL;
	char                    *servername = NULL;
	int                      port = 18515;
	int                      ib_port = 1;
	int                      size = 1024;
	int                      calc_size = 128*1024;
	int                      rx_depth = 2*512;
	int                      iters = 1000;
	int                      use_event = 0;
	int                      routs;
        int                      nposted;
	int                      rcnt, scnt;
	int                      num_cq_events = 0;
	int                      sl = 0;
	int			 gidx = -1;
	char			 gid[INET6_ADDRSTRLEN];
	int                      gpu_id = 0;
        int                      peersync = 1;
        int                      peersync_gpu_cq = 0;
        int                      peersync_gpu_dbrec = 0;
        int                      warmup = 10;
        int                      max_batch_len = 20;
        int                      consume_rx_cqe = 0;
	int                      gds_qp_type = 1;
        int                      sched_mode = CU_CTX_SCHED_AUTO;
        int                      ret = 0;
        int                      use_desc_apis = 0;

        MPI_CHECK(MPI_Init(&argc, &argv));
        MPI_CHECK(MPI_Comm_size(MPI_COMM_WORLD, &comm_size));
        MPI_CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &my_rank));

        if (comm_size != 2) { 
                fprintf(stderr, "this test requires exactly two processes \n");
                MPI_Abort(MPI_COMM_WORLD, -1);
        }

        fprintf(stdout, "libgdsync build version 0x%08x, major=%d minor=%d\n", GDS_API_VERSION, GDS_API_MAJOR_VERSION, GDS_API_MINOR_VERSION);

        int version;
        ret = gds_query_param(GDS_PARAM_VERSION, &version);
        if (ret) {
                fprintf(stderr, "error querying libgdsync version\n");
                MPI_Abort(MPI_COMM_WORLD, -1);
        }
        fprintf(stdout, "libgdsync queried version 0x%08x\n", version);
        if (!GDS_API_VERSION_COMPATIBLE(version)) {
                fprintf(stderr, "incompatible libgdsync version 0x%08x\n", version);
                MPI_Abort(MPI_COMM_WORLD, -1);
        }

	srand48(getpid() * time(NULL));

	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",  .has_arg = 1, .val = 'i' },
			{ .name = "size",     .has_arg = 1, .val = 's' },
			{ .name = "rx-depth", .has_arg = 1, .val = 'r' },
			{ .name = "iters",    .has_arg = 1, .val = 'n' },
			{ .name = "sl",       .has_arg = 1, .val = 'l' },
			{ .name = "events",   .has_arg = 0, .val = 'e' },
			{ .name = "gid-idx",  .has_arg = 1, .val = 'g' },
			{ .name = "gpu-id",          .has_arg = 1, .val = 'G' },
			{ .name = "peersync",        .has_arg = 0, .val = 'P' },
			{ .name = "peersync-gpu-cq", .has_arg = 0, .val = 'C' },
			{ .name = "peersync-gpu-dbrec", .has_arg = 1, .val = 'D' },
                        { .name = "peersync-desc-apis", .has_arg = 0, .val = 'U' },
			{ .name = "gpu-calc-size",   .has_arg = 1, .val = 'S' },
			{ .name = "batch-length",    .has_arg = 1, .val = 'B' },
			{ .name = "consume-rx-cqe",  .has_arg = 0, .val = 'Q' },
			{ .name = "time-gds-ops",  .has_arg = 0, .val = 'T' },
			{ .name = "qp-kind",          .has_arg = 1, .val = 'K' },
			{ .name = "gpu-sched-mode",  .has_arg = 1, .val = 'M' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:s:r:n:l:eg:G:K:S:B:PCDQTM:U", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = strtol(optarg, NULL, 0);
			if (port < 0 || port > 65535) {
				usage(argv[0]);
                                ret = 1;
                                exit(EXIT_FAILURE);
			}
			break;

		case 'd':
			ib_devname = strdupa(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 0) {
				usage(argv[0]);
				ret = 1;
                                exit(EXIT_FAILURE);
			}
			break;

		case 's':
			size = strtol(optarg, NULL, 0);
			break;

		case 'S':
			calc_size = strtol(optarg, NULL, 0);
			break;

		case 'r':
			rx_depth = strtol(optarg, NULL, 0);
			break;

		case 'n':
			iters = strtol(optarg, NULL, 0);
			break;

		case 'l':
			sl = strtol(optarg, NULL, 0);
			break;

		case 'e':
			++use_event;
			break;

		case 'g':
			gidx = strtol(optarg, NULL, 0);
			break;

		case 'G':
			gpu_id = strtol(optarg, NULL, 0);
                        printf("INFO: gpu id=%d\n", gpu_id);
			break;

		case 'B':
			max_batch_len = strtol(optarg, NULL, 0);
                        printf("INFO: max_batch_len=%d\n", max_batch_len);
			break;

		case 'P':
			peersync = !peersync;
                        printf("INFO: switching PeerSync %s\n", peersync?"ON":"OFF");
			break;
			
		case 'K':
			gds_qp_type = (int) strtol(optarg, NULL, 0);
                        switch (gds_qp_type) {
                        case 1: printf("INFO: GDS_QPT %s\n","UD"); gds_qpt = IBV_QPT_UD; break;
                        case 2: printf("INFO: GDS_QPT %s\n","RC"); gds_qpt = IBV_QPT_RC; break;
                        default: printf("ERROR: unexpected value 1 for UD or 2 for RC \n"); exit(EXIT_FAILURE); break;
                        }
			break;
		case 'Q':
			consume_rx_cqe = !consume_rx_cqe;
                        printf("INFO: switching consume_rx_cqe %s\n", consume_rx_cqe?"ON":"OFF");
			break;
			
		case 'T':
			gds_enable_event_prof = !gds_enable_event_prof;
                        printf("INFO: gds_enable_event_prof %s\n", gds_enable_event_prof?"ON":"OFF");
			break;

		case 'C':
			peersync_gpu_cq = !peersync_gpu_cq;
                        printf("INFO: switching %s PeerSync GPU CQ\n", peersync_gpu_cq?"ON":"OFF");
			break;

		case 'D':
			peersync_gpu_dbrec= !peersync_gpu_dbrec;
                        printf("INFO: switching %s PeerSync GPU QP DBREC\n", peersync_gpu_dbrec?"ON":"OFF");
			break;

		case 'M':
                {
                        char m = *optarg;
                        printf("INFO: sched mode '%c'\n", m);
                        switch (m) {
                        case 'S': sched_mode = CU_CTX_SCHED_SPIN; break;
                        case 'Y': sched_mode = CU_CTX_SCHED_YIELD; break;
                        case 'B': sched_mode = CU_CTX_SCHED_BLOCKING_SYNC; break;
                        case 'A': sched_mode = CU_CTX_SCHED_AUTO; break;
                        default: printf("ERROR: unexpected value %c\n", m); exit(EXIT_FAILURE); break;
                        }
                }
                break;

                case 'U':
                        use_desc_apis = 1;
                        printf("INFO: use_desc_apis=%d\n", use_desc_apis);
                        break;
                        
		default:
			usage(argv[0]);
			return 1;
		}
	}
        assert(comm_size == 2);
        char hostnames[comm_size][MPI_MAX_PROCESSOR_NAME];
        int name_len;
        MPI_CHECK(MPI_Get_processor_name(hostnames[my_rank], &name_len));
        assert(name_len < MPI_MAX_PROCESSOR_NAME);

        MPI_CHECK(MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 
                                hostnames, MPI_MAX_PROCESSOR_NAME, MPI_CHAR, MPI_COMM_WORLD));

        if (my_rank == 1) {
		servername = hostnames[0];
                printf("[%d] pid=%d server:%s\n", my_rank, getpid(), servername);
        } else {
                printf("[%d] pid=%d client:%s\n", my_rank, getpid(), hostnames[1]);
        }

        const char *tags = NULL;
        if (peersync) {
                tags = "wait trk|pollrxcq|polltxcq|postrecv|postwork| poketrk";
        } else {
                tags = "krn laun|krn sync|postsend|<------>|<------>| sent ev";
        }
        prof_init(&prof, 10000, 10000, "10us", 60, 2, tags);
        //prof_init(&prof, 100, 100, "100ns", 25*4, 2, tags);
        prof_disable(&prof);

	page_size = sysconf(_SC_PAGESIZE);

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return 1;
	}

        if (!ib_devname) {
                const char *value = getenv("USE_HCA"); 
                if (value != NULL) {
                        printf("[%d] USE_HCA: <%s>\n", my_rank, value);
                        ib_devname = value;
                }
        } else {
                printf("[%d] requested IB device: <%s>\n", my_rank, ib_devname);
        }

	{
		const char *value = getenv("GDS_ENABLE_EVENT_PROF"); 
		if (value != NULL) {
			gds_enable_event_prof = atoi(value);
		}
	}

	if (!ib_devname) {
                printf("[%d] picking 1st available device\n", my_rank);
		ib_dev = *dev_list;
		if (!ib_dev) {
			fprintf(stderr, "[%d] No IB devices found\n", my_rank);
			return 1;
		}
	} else {
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

        {
                const char *env = getenv("USE_GPU");
                if (env) {
                        gpu_id = atoi(env);
                        printf("USE_GPU=%s(%d)\n", env, gpu_id);
                }
        }
	ctx = pp_init_ctx(ib_dev, size, calc_size, rx_depth, ib_port, 0, gpu_id, peersync, peersync_gpu_cq, peersync_gpu_dbrec, consume_rx_cqe, sched_mode, use_desc_apis);
	if (!ctx)
		return 1;

	int nrecv = pp_post_recv(ctx, max_batch_len);
	if (nrecv < max_batch_len) {
		fprintf(stderr, "Couldn't post receive (%d)\n", nrecv);
		return 1;
	}

	if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return 1;
	}
	my_dest.lid = ctx->portinfo.lid;
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = (IBV_QPT_UD == gds_qpt) ? (lrand48() & 0xffffff) : 0;

	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
			fprintf(stderr, "Could not get local gid for gid index "
								"%d\n", gidx);
			return 1;
		}
	} else
		memset(&my_dest.gid, 0, sizeof my_dest.gid);

	printf("[%d]  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
	       my_rank, my_dest.lid, my_dest.qpn, my_dest.psn, gid);
	inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);

	struct pingpong_dest all_dest[4] = {{0,}};
        all_dest[my_rank] = my_dest;
        MPI_CHECK(MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 
                                all_dest, sizeof(all_dest[0]), MPI_CHAR, MPI_COMM_WORLD));
        rem_dest = &all_dest[my_rank?0:1];
	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);

	printf("[%d] remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       my_rank, rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

        if (IBV_QPT_UD == gds_qpt) {
                struct ibv_qp_attr attr = {
                        .qp_state		= IBV_QPS_RTR
                };

                if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
                        fprintf(stderr, "Failed to modify QP to RTR\n");
                        return 1;
                }

                MPI_Barrier(MPI_COMM_WORLD);

                attr.qp_state	    = IBV_QPS_RTS;
                attr.sq_psn	    = my_dest.psn;

                if (ibv_modify_qp(ctx->qp, &attr,
                                  IBV_QP_STATE              |
                                  IBV_QP_SQ_PSN)) {
                        fprintf(stderr, "Failed to modify QP to RTS\n");
                        return 1;
                }

                MPI_Barrier(MPI_COMM_WORLD);

                struct ibv_ah_attr ah_attr = {
                        .is_global     = 0,
                        .dlid          = rem_dest->lid,
                        .sl            = sl,
                        .src_path_bits = 0,
                        .port_num      = ib_port
                };
                if (rem_dest->gid.global.interface_id) {
                        ah_attr.is_global = 1;
                        ah_attr.grh.hop_limit = 1;
                        ah_attr.grh.dgid = rem_dest->gid;
                        ah_attr.grh.sgid_index = gidx;
                }

                ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
                if (!ctx->ah) {
                        fprintf(stderr, "Failed to create AH\n");
                        return 1;
                }

        }
	else {
                struct ibv_qp_attr attr = {
			.qp_state       = IBV_QPS_RTR,
			.path_mtu       = ctx->portinfo.active_mtu,
			.dest_qp_num    = rem_dest->qpn,
			.rq_psn         = rem_dest->psn,
			.ah_attr.dlid   = rem_dest->lid,
			.max_dest_rd_atomic     = 1,
			.min_rnr_timer          = 12,
			.ah_attr.is_global      = 0,
			.ah_attr.sl             = 0,
			.ah_attr.src_path_bits  = 0,
			.ah_attr.port_num       = ib_port
                };

                if (ibv_modify_qp(ctx->qp, &attr, (IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU
						   | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN
						   | IBV_QP_MIN_RNR_TIMER | IBV_QP_MAX_DEST_RD_ATOMIC))) {
                        fprintf(stderr, "Failed to modify QP to RTR\n");
                        return 1;
                }
		
		memset(&attr, 0, sizeof(struct ibv_qp_attr));
		attr.qp_state       = IBV_QPS_RTS;
		attr.sq_psn         = 0;
		attr.timeout        = 20;
		attr.retry_cnt      = 7;
		attr.rnr_retry      = 7;
		attr.max_rd_atomic  = 1;

		if (ibv_modify_qp(ctx->qp, &attr, (IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT
						     | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY
						   | IBV_QP_MAX_QP_RD_ATOMIC))) {
                        fprintf(stderr, "Failed to modify QP to RTS\n");
                        return 1;
		}
	}

        MPI_Barrier(MPI_COMM_WORLD);

	if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		ret = 1;
                goto out;
	}

        // for performance reasons, multiple batches back-to-back are posted here
	rcnt = scnt = 0;
        nposted = 0;
        routs = 0;
        const int n_batches = 3;
        //int prev_batch_len = 0;
        int last_batch_len = 0;
        int n_post = 0;
        int n_posted;
        int batch;
	int ii;

	if (gds_enable_event_prof) {
		for (ii = 0; ii < MAX_EVENTS; ii++) {
			cudaEventCreate(&start_time[ii]);
			cudaEventCreate(&stop_time[ii]);
		}
	}

        for (batch=0; batch<n_batches; ++batch) {
                n_post = min(min(ctx->rx_depth/2, iters-nposted), max_batch_len);
                n_posted = pp_post_work(ctx, n_post, 0, rem_dest->qpn, servername?1:0);
                if (n_posted != n_post) {
                        fprintf(stderr, "ERROR: Couldn't post work, got %d requested %d\n", n_posted, n_post);
                        ret = 1;
                        goto out;
                }
                routs += n_posted;
                nposted += n_posted;
                //prev_batch_len = last_batch_len;
                last_batch_len = n_posted;
                printf("[%d] batch %d: posted %d sequences\n", my_rank, batch, n_posted);
        }

	ctx->pending = PINGPONG_RECV_WRID;

        float pre_post_us = 0;

	if (gettimeofday(&end, NULL)) {
		perror("gettimeofday");
		ret = 1;
                goto out;
	}
	{
		float usec = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_usec - start.tv_usec);
		printf("pre-posting took %.2f usec\n", usec);
                pre_post_us = usec;
	}

        if (!my_rank) {
                puts("");
                printf("batch info: rx+kernel+tx %d per batch\n", n_posted); // this is the last actually
                printf("pre-posted %d sequences in %d batches\n", nposted, 2);
                printf("GPU kernel calc buf size: %d\n", ctx->calc_size);
                printf("iters=%d tx/rx_depth=%d\n", iters, ctx->rx_depth);
                printf("\n");
                printf("testing....\n");
                fflush(stdout);
        }

	if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		return 1;
	}
        prof_enable(&prof);
        prof_idx = 0;
        int got_error = 0;
        int iter = 0;
	while ((rcnt < iters || scnt < iters) && !got_error) {
                ++iter;
                PROF(&prof, prof_idx++);

                //printf("before tracking\n"); fflush(stdout);
                int ret = gpu_wait_tracking_event(1000*1000);
                if (ret == ENOMEM) {
                        printf("gpu_wait_tracking_event nothing to do (%d)\n", ret);
                } else if (ret == EAGAIN) {
                        printf("gpu_wait_tracking_event timout (%d), retrying\n", ret);
                        prof_reset(&prof);
                        continue;
                } else if (ret) {
                        fprintf(stderr, "gpu_wait_tracking_event failed (%d)\n", ret);
                        got_error = ret;
                }
                //gpu_infoc(20, "after tracking\n");

                PROF(&prof, prof_idx++);

                // don't call poll_cq on events which are still being polled by the GPU
                int n_rx_ev = 0;
                if (!ctx->consume_rx_cqe) {
                        struct ibv_wc wc[max_batch_len];
                        int ne = 0, i;

                        ne = ibv_poll_cq(ctx->rx_cq, max_batch_len, wc);
                        if (ne < 0) {
                                fprintf(stderr, "poll RX CQ failed %d\n", ne);
                                return 1;
                        }
                        n_rx_ev += ne;
                        //if (ne) printf("ne=%d\n", ne);
                        for (i = 0; i < ne; ++i) {
                                if (wc[i].status != IBV_WC_SUCCESS) {
                                        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                                                ibv_wc_status_str(wc[i].status),
                                                wc[i].status, (int) wc[i].wr_id);
                                        return 1;
                                }

                                switch ((int) wc[i].wr_id) {
                                case PINGPONG_RECV_WRID:
                                        ++rcnt;
                                        break;
                                default:
                                        fprintf(stderr, "Completion for unknown wr_id %d\n",
                                                (int) wc[i].wr_id);
                                        return 1;
                                }
                        }
                } else {
                        n_rx_ev = last_batch_len;
                        rcnt += last_batch_len;
                }

                PROF(&prof, prof_idx++);
                int n_tx_ev = 0;
                {
                        struct ibv_wc wc[max_batch_len];
                        int ne, i;

                        ne = ibv_poll_cq(ctx->tx_cq, max_batch_len, wc);
                        if (ne < 0) {
                                fprintf(stderr, "poll TX CQ failed %d\n", ne);
                                return 1;
                        }
                        n_tx_ev += ne;
                        for (i = 0; i < ne; ++i) {
                                if (wc[i].status != IBV_WC_SUCCESS) {
                                        fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                                                ibv_wc_status_str(wc[i].status),
                                                wc[i].status, (int) wc[i].wr_id);
                                        return 1;
                                }

                                switch ((int) wc[i].wr_id) {
                                case PINGPONG_SEND_WRID:
                                        ++scnt;
                                        break;
                                default:
                                        fprintf(stderr, "Completion for unknown wr_id %d\n",
                                                (int) wc[i].wr_id);
                                        ret = 1;
                                        goto out;
                                }
                        }
                }
                PROF(&prof, prof_idx++);
                if (1 && (n_tx_ev || n_rx_ev)) {
                        //fprintf(stderr, "iter=%d n_rx_ev=%d, n_tx_ev=%d\n", iter, n_rx_ev, n_tx_ev); fflush(stdout);
                }
                if (n_tx_ev || n_rx_ev) {
                        // update counters
                        routs -= last_batch_len;
                        //prev_batch_len = last_batch_len;
                        if (n_tx_ev != last_batch_len)
                                fprintf(stderr, "[%d] unexpected tx ev %d, batch len %d\n", iter, n_tx_ev, last_batch_len);
                        if (n_rx_ev != last_batch_len)
                                fprintf(stderr, "[%d] unexpected rx ev %d, batch len %d\n", iter, n_rx_ev, last_batch_len);
                        if (nposted < iters) {
                                //fprintf(stdout, "rcnt=%d scnt=%d routs=%d nposted=%d\n", rcnt, scnt, routs, nposted); fflush(stdout);
                                // potentially submit new work
                                n_post = min(min(ctx->rx_depth/2, iters-nposted), max_batch_len);
                                int n = pp_post_work(ctx, n_post, nposted, rem_dest->qpn, servername?1:0);
                                if (n != n_post) {
                                        fprintf(stderr, "ERROR: post_work error (%d) rcnt=%d n_post=%d routs=%d\n", n, rcnt, n_post, routs);
                                        return 1;
                                }
                                last_batch_len = n;
                                routs += n;
                                nposted += n;
                                //fprintf(stdout, "n_post=%d n=%d\n", n_post, n);
                        }
                }
                //usleep(10);
                PROF(&prof, prof_idx++);
		prof_update(&prof);
		prof_idx = 0;

                //fprintf(stdout, "%d %d\n", rcnt, scnt); fflush(stdout);


                if (got_error) {
                        fprintf(stderr, "exiting for error\n");
                        return 1;
                }
	}

	if (gettimeofday(&end, NULL)) {
		perror("gettimeofday");
		ret = 1;
	}


        int rid;
        for (rid = 0; rid < comm_size; ++rid) {
                MPI_Barrier(MPI_COMM_WORLD);
                if (my_rank == rid) {
                        float usec = (end.tv_sec - start.tv_sec) * 1000000 +
                                (end.tv_usec - start.tv_usec) + pre_post_us;
                        long long bytes = (long long) size * iters * 2;

                        printf("[%d] %lld bytes in %.2f seconds = %.2f Mbit/sec\n",
                               my_rank, bytes, usec / 1000000., bytes * 8. / usec);
                        printf("[%d] %d iters in %.2f seconds = %.2f usec/iter\n",
                               my_rank, iters, usec / 1000000., usec / iters);

                        if (prof_enabled(&prof)) {
                                printf("[%d] dumping prof\n", my_rank);
                                prof_dump(&prof);
                        }
                }
        }

	//ibv_ack_cq_events(ctx->cq, num_cq_events);

	//expect work to be completed by now

	if (gds_enable_event_prof) {
		for (ii = 0; ii < event_idx; ii++) {
			cudaEventElapsedTime(&elapsed_time, start_time[ii], stop_time[ii]);
			fprintf(stderr, "[%d] size = %d, time = %f\n", my_rank, ctx->size, 1000 * elapsed_time);
		}
		for (ii = 0; ii < MAX_EVENTS; ii++) {
			cudaEventDestroy(stop_time[ii]);
			cudaEventDestroy(start_time[ii]);
		}
	} 

        MPI_Barrier(MPI_COMM_WORLD);
	if (pp_close_ctx(ctx))
		ret = 1;

	ibv_free_device_list(dev_list);
	//free(rem_dest);

        MPI_Barrier(MPI_COMM_WORLD);
        MPI_Finalize();
out:
	return ret;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
