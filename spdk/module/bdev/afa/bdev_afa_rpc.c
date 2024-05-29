#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_afa.h"

#define RPC_MAX_BASE_BDEVS 255


/*
 * Base bdevs in RPC bdev_afa_create
 */
struct rpc_construct_afa_base_bdevs {
	/* Number of base bdevs */
	size_t         num_base_bdevs;

	/* List of base bdevs names */
	char             *base_bdev_names[RPC_MAX_BASE_BDEVS];
};


/*
 * Input structure for RPC rpc_bdev_afa_create
 */
struct rpc_construct_afa {
	char        *name;
	uint32_t    chunk_size_kb;
	uint8_t     num_data_base_bdevs;
    uint8_t     num_parity_base_bdevs;
    uint8_t     num_spare_base_bdevs;
    struct rpc_construct_afa_base_bdevs base_bdevs;
};


static void
free_rpc_construct_afa(struct rpc_construct_afa *req)
{
	size_t i;

	free(req->name);
	for (i = 0; i < req->base_bdevs.num_base_bdevs; i++) {
		free(req->base_bdevs.base_bdev_names[i]);
	}
}

/*
 * Decoder function for RPC bdev_afa_create to decode base bdevs list
 */
static int
decode_base_bdevs(const struct spdk_json_val *val, void *out)
{
	struct rpc_construct_afa_base_bdevs *base_bdevs = out;
	return spdk_json_decode_array(val, spdk_json_decode_string, base_bdevs->base_bdev_names,
				      RPC_MAX_BASE_BDEVS, &base_bdevs->num_base_bdevs, sizeof(char *));
}


static const struct spdk_json_object_decoder rpc_construct_afa_decoders[] = {
	{"name", offsetof(struct rpc_construct_afa, name), spdk_json_decode_string},
	{"chunk_size_kb", offsetof(struct rpc_construct_afa, chunk_size_kb), spdk_json_decode_uint64},
    {"num_data_base_bdevs", offsetof(struct rpc_construct_afa, num_data_base_bdevs), spdk_json_decode_uint8},
    {"num_parity_base_bdevs", offsetof(struct rpc_construct_afa, num_parity_base_bdevs), spdk_json_decode_uint8},
    {"num_spare_base_bdevs", offsetof(struct rpc_construct_afa, num_spare_base_bdevs), spdk_json_decode_uint8},
    {"base_bdevs", offsetof(struct rpc_construct_afa, base_bdevs), decode_base_bdevs},
};


static void
rpc_bdev_afa_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
    struct rpc_construct_afa req = {};
    struct afa_bdev_opts opts = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int i, rc = 0;

    if (spdk_json_decode_object(params, rpc_construct_afa_decoders,
				    SPDK_COUNTOF(rpc_construct_afa_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_afa, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

    if (req.chunk_size_kb == 0) {
		spdk_jsonrpc_send_error_response(request, EINVAL, "chunk size not specified");
		goto cleanup;
	}

    if (req.chunk_size_kb < 4) {
		spdk_jsonrpc_send_error_response(request, EINVAL, "chunk size is too small");
		goto cleanup;
	}

    if (req.num_data_base_bdevs == 0) {
		spdk_jsonrpc_send_error_response(request, EINVAL, "at least one data base bdev is needed");
		goto cleanup;
	}

    if (req.num_data_base_bdevs + req.num_parity_base_bdevs + req.num_spare_base_bdevs != req.base_bdevs.num_base_bdevs) {
		spdk_jsonrpc_send_error_response(request, EINVAL, "the number of base bdevs is error");
		goto cleanup;
	}

	if(req.base_bdevs.num_base_bdevs > MAX_NUM_BASE_BDEV) {
		spdk_jsonrpc_send_error_response(request, EINVAL, "too many base bdevs");
		goto cleanup;
	}

    opts.name = req.name;
	opts.chunk_size_kb = req.chunk_size_kb;
	opts.num_data_base_bdevs = req.num_data_base_bdevs;
	opts.num_parity_base_bdevs = req.num_parity_base_bdevs;
	opts.num_spare_base_bdevs = req.num_spare_base_bdevs;
	
	for(i = 0; i < req.base_bdevs.num_base_bdevs; i++) {
		opts.base_bdevs[i].name = req.base_bdevs.base_bdev_names[i];
	}

	rc = bdev_afa_create(&bdev, &opts);

	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, bdev->name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_construct_afa(&req);
	return;

cleanup:
	free_rpc_construct_afa(&req);
}
SPDK_RPC_REGISTER("bdev_afa_create", rpc_bdev_afa_create, SPDK_RPC_RUNTIME)


struct rpc_delete_afa {
	char *name;
};


static void
free_rpc_delete_afa(struct rpc_delete_afa *req)
{
	free(req->name);
}


static const struct spdk_json_object_decoder rpc_delete_afa_decoders[] = {
	{"name", offsetof(struct rpc_delete_afa, name), spdk_json_decode_string},
};


static void
rpc_bdev_afa_delete_cb(void *cb_arg, int rc)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (rc == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	}
}


static void
rpc_bdev_afa_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_delete_afa req = {};

	if (spdk_json_decode_object(params, rpc_delete_afa_decoders,
				    SPDK_COUNTOF(rpc_delete_afa_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_afa_delete(req.name, rpc_bdev_afa_delete_cb, request);

	free_rpc_delete_afa(&req);

	return;

cleanup:
	free_rpc_delete_afa(&req);
}
SPDK_RPC_REGISTER("bdev_afa_delete", rpc_bdev_afa_delete, SPDK_RPC_RUNTIME)



/* list all existing afa bdevs */
static void
rpc_bdev_afa_list_all(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{	
	struct spdk_json_write_ctx  *w;
	struct afa_bdev *afa_bdev;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(afa_bdev, &g_afa_bdev_head, tailq) {
		spdk_json_write_string(w, afa_bdev->bdev.name);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_afa_list_all", rpc_bdev_afa_list_all, SPDK_RPC_RUNTIME)


/*
 * Input structure for bdev_afa_list_base_bdevs RPC
 */
struct rpc_bdev_afa_list_base_bdevs {
	char *name;
};

/**
 * rpc_bdev_afa_list_base_bdevs function frees RPC bdev_afa_list_base_bdevs related parameters
 * \param req - pointer to RPC request
 */
static void
free_rpc_bdev_afa_list_base_bdevs(struct rpc_bdev_afa_list_base_bdevs *req)
{
	free(req->name);
}

/*
 * Decoder object for RPC bdev_afa_list_base_bdevs
 */
static const struct spdk_json_object_decoder rpc_bdev_afa_list_base_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_bdev_afa_list_base_bdevs, name), spdk_json_decode_string},
};


/* list all base bdevs of the afa */
static void
rpc_bdev_afa_list_base_bdevs(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_afa_list_base_bdevs   req = {};
	struct spdk_json_write_ctx  *w;
	struct afa_bdev *afa_bdev;
	struct afa_base_bdev *base_bdev;

	if (spdk_json_decode_object(params, rpc_bdev_afa_list_base_bdevs_decoders,
				    SPDK_COUNTOF(rpc_bdev_afa_list_base_bdevs_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(afa_bdev, &g_afa_bdev_head, tailq) {
		if(!strcmp(req.name, afa_bdev->bdev.name)) {
			AFA_FOR_EACH_BASE_BDEV(afa_bdev, base_bdev) {
				spdk_json_write_string(w, base_bdev->bdev->name);
			}
			break;
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free_rpc_bdev_afa_list_base_bdevs(&req);
}
SPDK_RPC_REGISTER("bdev_afa_list_base_bdevs", rpc_bdev_afa_list_base_bdevs, SPDK_RPC_RUNTIME)