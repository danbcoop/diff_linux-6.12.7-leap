// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/slab.h>

#include "backend_none.h"

static void release_params_none(struct zcomp_params *params)
{
}

static int setup_params_none(struct zcomp_params *params)
{
	return 0;
}

static void destroy_none(struct zcomp_ctx *ctx)
{
	kfree(ctx->context);
}

static int create_none(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kzalloc(1, GFP_KERNEL);
	if (!ctx->context)
		return -ENOMEM;
	return 0;
}

static int compress_none(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req)
{
	memcpy(req->dst, req->src, req->src_len);
	return 0;
}

static int decompress_none(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	memcpy(req->dst, req->src, req->src_len);
	return 0;
}

const struct zcomp_ops backend_none = {
	.compress	= compress_none,
	.decompress	= decompress_none,
	.create_ctx	= create_none,
	.destroy_ctx	= destroy_none,
	.setup_params	= setup_params_none,
	.release_params	= release_params_none,
	.name		= "none",
};