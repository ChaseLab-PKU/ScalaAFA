#include "bdev_afa.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/bdev_module.h"
#include "spdk/likely.h"
#include <stdlib.h>

struct afa_tailq		g_afa_bdev_head = TAILQ_HEAD_INITIALIZER(g_afa_bdev_head);
uint64_t global_wrt_cnt = 0;
uint64_t global_redirect_cnt = 0;


/* For implicit declaration */
static void afa_bdev_handle_read_request(struct afa_bdev_io *io);
static void choose_parity_idxs(struct afa_bdev *afa_bdev, struct afa_io_channel *ch, uint32_t sc_no);
static void handle_gc_flag(struct afa_bdev *afa_bdev, struct spdk_bdev_io *io, uint8_t idx);
static void afa_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static void afa_bdev_finish(void);
static void afa_bdev_examine(struct spdk_bdev *bdev);
static void afa_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx);
static int afa_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size);
static int afa_bdev_destruct(void *ctx);
static int afa_bdev_initialize(void);
static int afa_bdev_get_ctx_size(void);
static int afa_phase_switching(void *ctx);
static int afa_sc_garbage_collection(void *ctx);
static int afa_base_bdev_gc_probe(void *ctx);
static int k_num_without_array(int k, int len, uint8_t *array);
static bool afa_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static bool afa_bdev_io_complete_part(struct afa_bdev_io *afa_io, uint64_t completed, enum spdk_bdev_io_status status);
static bool can_avoid_gc(struct afa_bdev *afa_bdev, uint32_t sc_no, uint8_t *slot);

static struct spdk_io_channel *afa_bdev_get_io_channel(void *ctx);


/* g_afa_bdev_fn_table is the function table for afa bdev */
static const struct spdk_bdev_fn_table g_afa_bdev_fn_table = {
	.destruct			= afa_bdev_destruct,
	.submit_request		= afa_bdev_submit_request,
	.io_type_supported	= afa_bdev_io_type_supported,
	.get_io_channel		= afa_bdev_get_io_channel,
	.get_memory_domains	= afa_bdev_get_memory_domains,
};


static struct spdk_bdev_module afa_if = {
	.name = "afa",
	.module_init = afa_bdev_initialize,
	.module_fini = afa_bdev_finish,
	.get_ctx_size = afa_bdev_get_ctx_size,
	.examine_config = afa_bdev_examine,
	.async_init = false,
	.async_fini = false,
};

SPDK_BDEV_MODULE_REGISTER(afa, &afa_if)

static inline int victim_sc_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next < curr);
}

static inline pqueue_pri_t victim_sc_get_pri(void *a)
{
    return ((struct stripe_chunk_info *)a)->invalid_chunks;
}

static inline void victim_sc_set_pri(void *a, pqueue_pri_t pri)
{
	((struct stripe_chunk_info *)a)->invalid_chunks = pri;
}

static inline size_t victim_sc_get_pos(void *a)
{
    return ((struct stripe_chunk_info *)a)->pos;
}

static inline void victim_sc_set_pos(void *a, size_t pos)
{
    ((struct stripe_chunk_info *)a)->pos = pos;
}


void 
merge_list_insert(struct mlh *phead, uint32_t begin, uint32_t end)
{
	struct interval *p, *q, *nitv;
	
	p = phead->first;
	if(p == NULL) {
		nitv = calloc(1, sizeof(struct interval));
		nitv->begin = begin;
		nitv->end = end;
		phead->first = nitv;
		return;
	}

	if(begin < p->begin) {
		if(end == p->begin) p->begin = begin;
		else {
			nitv = calloc(1, sizeof(struct interval));
			nitv->begin = begin;
			nitv->end = end;
			nitv->next = p;
			phead->first = nitv;
		}
		return;
	}

	q = phead->first->next;
	if(q == NULL) {
		assert(begin >= p->end);
		if(begin == p->end) p->end = end;
		else {
			nitv = calloc(1, sizeof(struct interval));
			nitv->begin = begin;
			nitv->end = end;
			nitv->next = NULL;
			p->next = nitv;
		}
		return;
	}

	for(; q != NULL; p = p->next, q = q->next) {
		assert(begin >= p->end);
		if(begin < q->begin) {
			if(begin == p->end && end == q->begin) {
				p->end = q->end;
				p->next = q->next;
				free(q);
			}
			else if(begin == p->end) p->end = end;
			else if(end == q->begin) q->begin = begin;
			else {
				nitv = calloc(1, sizeof(struct interval));
				nitv->begin = begin;
				nitv->end = end;
				nitv->next = p;
				p->next = nitv;
			}
			return;
		}
	}

	assert(q == NULL);
	if(end == p->end) p->end = end;
	else {
		nitv = calloc(1, sizeof(struct interval));
		nitv->begin = begin;
		nitv->end = end;
		nitv->next = NULL;
		p->next = nitv;
	}
}


uint32_t
merge_list_get(struct mlh *phead, uint32_t num_want, struct mlh *res)
{
	struct interval  *p;
	uint32_t num_get = 0;

	while(phead->first != NULL && num_get < num_want) {
		p = phead->first;
		if(p->end - p->begin > num_want - num_get) {
			merge_list_insert(res, p->begin, p->begin + num_want - num_get);
			p->begin += num_want - num_get;
			num_get = num_want;
			break;
		}
		merge_list_insert(res, p->begin, p->end);
		num_get += p->end - p->begin;
		phead->first = p->next;
		free(p);
	}

	return num_get;
}


/**
 * free resource of base bdev for afa bdev
 * \param afa_bdev - pointer to afa bdev
 * \param base_bdev- afa base bdev
 */
static void
afa_base_bdev_free_resource(struct afa_bdev *afa_bdev, struct afa_base_bdev *base_bdev)
{
	free(base_bdev->info->name);
	free(base_bdev->info);

	assert(afa_bdev->info->num_base_bdevs);
}


/**
 * free resource of afa bdev
 * \param afa_bdev - pointer to afa bdev
 */
static void
afa_bdev_free_resource(void *io_device)
{
	struct afa_bdev *afa_bdev = io_device;
	SPDK_DEBUGLOG(bdev_afa, "afa_bdev_free_resource, %p name %s\n", afa_bdev, afa_bdev->info->name);
	TAILQ_REMOVE(&g_afa_bdev_head, afa_bdev, tailq);

	free(afa_bdev->info->name);
	free(afa_bdev->info);
	free(afa_bdev->cmt);
	free(afa_bdev->rcmt_bm);
	free(afa_bdev->scis);	
	free(afa_bdev);
}


/**
 * Destruct afa bdev.
 *
 * \param ctx pointer to afa_bdev
 * \return 0 - success, non zero - failure
 */
static int
afa_bdev_destruct(void *ctx)
{
	struct afa_bdev *afa_bdev = ctx;
	struct afa_base_bdev *base_bdev;

	SPDK_DEBUGLOG(bdev_afa, "afa_bdev_destruct\n");

	afa_bdev->destruct_called = true;
	AFA_FOR_EACH_BASE_BDEV(afa_bdev, base_bdev) {
		afa_base_bdev_free_resource(afa_bdev, base_bdev);
		spdk_bdev_module_release_bdev(base_bdev->bdev);
	}

	TAILQ_REMOVE(&g_afa_bdev_head, afa_bdev, tailq);
	
	SPDK_DEBUGLOG(bdev_afa, "afa bdev base bdevs is 0, going to free all in destruct\n");
	
	// BUG here
	// afa_bdev_free_resource(afa_bdev);

	spdk_io_device_unregister(afa_bdev,afa_bdev_free_resource);

	return 0;
}


void
afa_bdev_io_complete(struct afa_bdev_io *afa_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(afa_io);

	spdk_bdev_io_complete(bdev_io, status);
}


/**
 * Callback function to spdk_bdev_io_get_buf.
 * \param ch - pointer to afa bdev io channel
 * \param bdev_io - pointer to parent bdev_io on afa bdev device
 * \param success - True if buffer is allocated or false otherwise.
 */
static void
afa_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct afa_bdev_io *afa_io = (struct afa_bdev_io *)bdev_io->driver_ctx;

	if (!success) afa_bdev_io_complete(afa_io, SPDK_BDEV_IO_STATUS_FAILED);
	else afa_bdev_handle_read_request(afa_io);
}


static void
afa_base_read_io_complete(struct spdk_bdev_io *base_io, bool success, void *cb_arg)
{	
	struct read_io_cb_arg *arg = cb_arg;
	struct afa_bdev_io *afa_io = arg->afa_io;

	handle_gc_flag(afa_io->afa_bdev, base_io, arg->base_bdev_idx);

	afa_bdev_io_complete_part(afa_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);

	spdk_bdev_free_io(base_io);

	free(arg);
}


static void
afa_base_write_io_complete(struct spdk_bdev_io *base_io, bool success, void *cb_arg)
{
	struct write_io_cb_arg 	*arg = cb_arg;
	struct afa_bdev 		*afa_bdev = arg->afa_io->afa_bdev;
	uint32_t				org_afa_chunk_no, org_sc_no;
	
	if(arg->change_cmt) {
		if(afa_bdev->cmt[arg->user_chunk_no] != UINT32_MAX) {
			org_afa_chunk_no = afa_bdev->cmt[arg->user_chunk_no];
			org_sc_no = org_afa_chunk_no / afa_bdev->info->num_data_base_bdevs;
			afa_bdev->rcmt_bm[org_afa_chunk_no] = 2;
			afa_bdev->rcmt[org_afa_chunk_no] = UINT32_MAX;
			if(!afa_bdev->scis[org_sc_no].pos) {
				afa_bdev->scis[org_sc_no].invalid_chunks++;
				pqueue_insert(afa_bdev->victim_sc_pq, &afa_bdev->scis[org_sc_no]);
			}
			else {
				pqueue_change_priority(afa_bdev->victim_sc_pq, afa_bdev->scis[org_sc_no].invalid_chunks + 1, &afa_bdev->scis[org_sc_no]);
			}
		}
		arg->afa_io->afa_bdev->cmt[arg->user_chunk_no] = arg->afa_chunk_no;
		arg->afa_io->afa_bdev->rcmt_bm[arg->afa_chunk_no] = 1;
		arg->afa_io->afa_bdev->rcmt[arg->afa_chunk_no] = arg->user_chunk_no;
	}

	handle_gc_flag(afa_bdev, base_io, arg->base_bdev_idx);

	afa_bdev_io_complete_part(arg->afa_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);

	spdk_bdev_free_io(base_io);

	free(arg);
}

static void
handle_base_bdev_gc_stop(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct afa_io_channel *afa_ch = spdk_io_channel_get_ctx(ch);
	struct afa_bdev *afa_bdev = spdk_io_channel_get_io_device(ch);
	struct interval *p, *new, *pre = NULL;
	uint32_t j;
	uint8_t tmp;

	p = afa_ch->partial_sc_lp.first;
	while(p != NULL) {
		for(j = p->begin; j < p->end; j++) {
			if(can_avoid_gc(afa_bdev, j, &tmp)) {
				if(j == p->begin) {
					if(j + 1 == p->end) {
						if(pre == NULL) {
							afa_ch->partial_sc_lp.first = NULL;
							free(p);
						}
						else {
							pre->next = p->next;
							free(p);
						}
					}
					else p->begin++;
				}
				else if(j == p->end - 1) p->end--;
				else {
					new = calloc(1, sizeof(struct interval));
					new->begin = j + 1;
					new->end = p->end;
					new->next = p->next;
					p->end = j;
					p->next = new;
				}
				afa_ch->num_partial_sc_lp--;
				
				merge_list_insert(&afa_ch->partial_sc, j, j + 1);
				afa_ch->num_partial_sc++;
			}
		}
		pre = p;
		p = p->next;
	}

}

static void
handle_base_bdev_gc_stop_done(struct spdk_io_channel_iter *i, int status)
{	
	// Do nothing.
}

static void
handle_gc_flag(struct afa_bdev *afa_bdev, struct spdk_bdev_io *io, uint8_t idx)
{
	uint8_t gc_flag = io->u.bdev.afa.gc_flag;

	if(gc_flag == 0 && afa_bdev->base_bdevs[idx].info->state == GC) {
		afa_bdev->base_bdevs[idx].info->state = NORMAL;
		afa_bdev->num_gcing_base_bdevs--;

		spdk_for_each_channel(afa_bdev, handle_base_bdev_gc_stop, NULL, handle_base_bdev_gc_stop_done);
	}
	else if(gc_flag == 1 && afa_bdev->base_bdevs[idx].info->state == NORMAL) {
		afa_bdev->base_bdevs[idx].info->state = GC;
		afa_bdev->num_gcing_base_bdevs++;
	}
	
}


/**
 * afa_bdev_io_complete_part - signal the completion of a part of the expected
 * base bdev IOs and complete the afa_io if this is the final expected IO.
 * The caller should first set afa_io->base_bdev_io_remaining. This function
 * will decrement this counter by the value of the 'completed' parameter and
 * complete the afa_io if the counter reaches 0. 
 * \param afa_io - pointer to afa_bdev_io
 * \param completed - the number of the base io that has been completed
 * \param status - status of the base IO
 * \return true - if the afa_io is completed; false -  otherwise
 */
static bool
afa_bdev_io_complete_part(struct afa_bdev_io *afa_io, uint64_t completed, enum spdk_bdev_io_status status)
{
	assert(afa_io->base_bdev_io_remaining >= completed);
	afa_io->base_bdev_io_remaining -= completed;

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		afa_io->base_bdev_io_status = status;
	}

	if (afa_io->base_bdev_io_remaining == 0) {
		afa_bdev_io_complete(afa_io, afa_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}


/* get a free stripe chunk */
static uint32_t
get_one_free_stripe_chunk(struct afa_bdev *afa_bdev, struct afa_io_channel *ch)
{
	struct mlh res;
	uint32_t sc_no;
	uint32_t get_num;
	struct spdk_thread *t;

	res.first = NULL;
	get_num = merge_list_get(&ch->free_sc, 1, &res);
	ch->num_free_sc -= get_num;

	if(ch->num_free_sc < LOW_THRESHOLD_SUPP_SC) {
		spdk_thread_send_msg(afa_bdev->daemon, afa_handle_ask_sc, ch);
	}
	
	if(get_num == 0) {
		t = spdk_get_thread();
		spdk_set_thread(afa_bdev->daemon);
		afa_handle_ask_sc(ch);
		spdk_set_thread(t);
		res.first = NULL;
		get_num = merge_list_get(&ch->free_sc, 1, &res);
		ch->num_free_sc -= get_num;
	}
	
	assert(get_num == 1);
	assert(res.first->begin + 1 == res.first->end);
	assert(res.first->next == NULL);
	
	sc_no = res.first->begin;
	
	free(res.first);

	return sc_no;
}


static bool 
can_avoid_gc(struct afa_bdev *afa_bdev, uint32_t sc_no, uint8_t *slot)
{
	uint8_t k = afa_bdev->info->num_data_base_bdevs;
	uint8_t m = afa_bdev->info->num_parity_base_bdevs;
	uint8_t i, idx;

	for(i = 0; i < m; ++i) {
		idx = afa_bdev->scis[sc_no].parity_idxs[i];
		if(afa_bdev->base_bdevs[idx].info->state == GC) {
			return false;
		}
	}

	for(i = 0; i < k; i++) {
		if(afa_bdev->scis[sc_no].bitmap[i] == 0) {
			idx = k_num_without_array(i, m, afa_bdev->scis[sc_no].parity_idxs);;
			if(afa_bdev->base_bdevs[idx].info->state == NORMAL){
				*slot = i;
				return true;
			}
		}
	}

	return false;
}


static uint32_t
get_one_empty_chunk_wo_gc_avoid(struct afa_bdev *afa_bdev, struct afa_io_channel *ch, uint8_t *slot) {
	struct mlh res;
	struct sc_entry *sc;
	uint32_t sc_no;
	uint32_t get_num = 0;
	uint8_t k = afa_bdev->info->num_data_base_bdevs;
	uint8_t i, tmp;

	res.first = NULL;

	if(ch->num_partial_sc_lp > 0 && ch->partial_sc_lp.first != NULL) {
		get_num = merge_list_get(&ch->partial_sc_lp, 1, &res);
		ch->num_partial_sc_lp -= 1;
		sc_no = res.first->begin;

		assert(get_num == 1);
		assert(res.first == NULL || res.first->begin + 1 == res.first->end);
		assert(res.first->next == NULL);

		free(res.first);
	}
	else if(ch->num_partial_sc > 0) {
		get_num = merge_list_get(&ch->partial_sc, 1, &res);
		ch->num_partial_sc -= 1;
		sc_no = res.first->begin;

		assert(get_num == 1);
		assert(res.first == NULL || res.first->begin + 1 == res.first->end);
		assert(res.first->next == NULL);

		free(res.first);
	}
	else {
		sc_no = get_one_free_stripe_chunk(afa_bdev, ch);
		choose_parity_idxs(afa_bdev, ch, sc_no);
		afa_bdev->scis[sc_no].bitmap = calloc(afa_bdev->info->num_data_base_bdevs, sizeof(uint8_t));
		afa_bdev->scis[sc_no].state = DUPLICATE;
		afa_bdev->scis[sc_no].used_chunks = 1;
		afa_bdev->scis[sc_no].bitmap[0] = 1;
		*slot = 0;
	}

	for(i = 0; i < k; ++i) {
		if(afa_bdev->scis[sc_no].bitmap[i] == 0) {
			*slot = i;
			afa_bdev->scis[sc_no].bitmap[i] = 1;
			afa_bdev->scis[sc_no].used_chunks++;
			break;
		}
	}

	assert(i < k);

	if(afa_bdev->scis[sc_no].used_chunks < k) {
		if(can_avoid_gc(afa_bdev, sc_no, &tmp)) {
			merge_list_insert(&ch->partial_sc, sc_no, sc_no + 1);
			ch->num_partial_sc++;
		}
		else {
			merge_list_insert(&ch->partial_sc_lp, sc_no, sc_no + 1);
			ch->num_partial_sc_lp++;
		}
	}
	else {
		sc = calloc(1, sizeof(struct sc_entry));
		sc->no = sc_no;
		TAILQ_INSERT_TAIL(&afa_bdev->sc_need_switch, sc, tailq);
	}
	
	return sc_no;
}

static uint32_t
get_one_empty_chunk_gc_avoid(struct afa_bdev *afa_bdev, struct afa_io_channel *ch, uint8_t *slot) {
	struct mlh res;
	struct sc_entry *sc;
	uint32_t sc_no;
	uint32_t get_num = 0;
	uint8_t k = afa_bdev->info->num_data_base_bdevs;
	uint8_t tmp;

	res.first = NULL;

	while(ch->num_partial_sc > 0) {
		merge_list_get(&ch->partial_sc, 1, &res);
		ch->num_partial_sc -= 1;
		sc_no = res.first->begin;
		free(res.first);
		
		if(can_avoid_gc(afa_bdev, sc_no, slot)) {
			afa_bdev->scis[sc_no].bitmap[*slot] = 1;
			afa_bdev->scis[sc_no].used_chunks++;
			get_num = 1;
			break;
		}
		else {
			merge_list_insert(&ch->partial_sc_lp, sc_no, sc_no + 1);
			ch->num_partial_sc_lp++;
		}
	}
	if(get_num == 0) {
		sc_no = get_one_free_stripe_chunk(afa_bdev, ch);
		choose_parity_idxs(afa_bdev, ch, sc_no);
		afa_bdev->scis[sc_no].bitmap = calloc(afa_bdev->info->num_data_base_bdevs, sizeof(uint8_t));
		afa_bdev->scis[sc_no].state = DUPLICATE;
		afa_bdev->scis[sc_no].used_chunks = 1;
		*slot = 0;
		afa_bdev->scis[sc_no].bitmap[0] = 1;
	}

	if(afa_bdev->scis[sc_no].used_chunks < k) {
		if(can_avoid_gc(afa_bdev, sc_no, &tmp)) {
			merge_list_insert(&ch->partial_sc, sc_no, sc_no + 1);
			ch->num_partial_sc++;
		}
		else {
			merge_list_insert(&ch->partial_sc_lp, sc_no, sc_no + 1);
			ch->num_partial_sc_lp++;
		}
	}
	else {
		sc = calloc(1, sizeof(struct sc_entry));
		sc->no = sc_no;
		TAILQ_INSERT_TAIL(&afa_bdev->sc_need_switch, sc, tailq);
	}

	return sc_no;
}

/* get a empty chunk */
static uint32_t
get_one_empty_chunk(struct afa_bdev *afa_bdev, struct afa_io_channel *ch, uint8_t *slot)
{
	uint32_t sc_no = UINT32_MAX;

	if(afa_bdev->num_gcing_base_bdevs < afa_bdev->info->num_data_base_bdevs) 
		sc_no =  get_one_empty_chunk_gc_avoid(afa_bdev, ch, slot);
	if(sc_no == UINT32_MAX) 
		sc_no = get_one_empty_chunk_wo_gc_avoid(afa_bdev, ch, slot);

	if(afa_bdev->num_gcing_base_bdevs < afa_bdev->info->num_data_base_bdevs) {
		uint8_t base_bdev_idx = k_num_without_array(*slot, afa_bdev->info->num_parity_base_bdevs, afa_bdev->scis[sc_no].parity_idxs);
		if(afa_bdev->base_bdevs[base_bdev_idx].info->state == GC) {
			global_redirect_cnt++;
		}
	}
	
	return sc_no;
}


/* return the kth smallest number without counting numbers in the sorted array */
static int
k_num_without_array(int k, int len, uint8_t *array)
{
	int i = 0, j = 0, p = 0;
	for(; i <= k + len && j < len; ++i) {
		if(i == array[j]) j++;
		else if(p++ == k) return i;
	}
	return i + (k - p);
}


/**
 * none zero - read transient area (value = parity idx + ), false - read normal area
 */
static bool
choose_read_base_bdev_gc_avoid(struct afa_bdev *afa_bdev, uint32_t sc_no, uint8_t slot, uint8_t *idx) 
{
	uint8_t	m = afa_bdev->info->num_parity_base_bdevs;
	uint8_t i, j, p, orig;

	orig = k_num_without_array(slot, m, afa_bdev->scis[sc_no].parity_idxs);
	if(afa_bdev->base_bdevs[orig].info->state == NORMAL) {
		*idx = orig;
		return 0;
	}

	for(i = rand() % m, j = 0; j < m; ++j, i = (i+1) % m) {
		p = afa_bdev->scis[sc_no].parity_idxs[i];
		if(afa_bdev->base_bdevs[p].info->state == NORMAL) {
			*idx = p;
			return i + 1;
		}
	}

	*idx = orig;
	return 0;
}


/** 
 * Handle read request no greater than one chunk. 
 * We translate user logical addr to afa logical addr here.
 */
static void
afa_bdev_handle_chunk_read_request(struct afa_bdev_io *afa_io, struct iovec *iovs, int iovcnt,
					uint32_t user_chunk_no, uint32_t offset_blocks, uint32_t num_blocks) 
{
	struct afa_bdev				*afa_bdev = afa_io->afa_bdev;
	uint32_t 					afa_chunk_no = afa_bdev->cmt[user_chunk_no];
	uint32_t 					afa_strip_chunk_no;
	uint8_t 					slot;
	uint8_t 					base_bdev_idx;
	uint8_t						k = afa_bdev->info->num_data_base_bdevs;
	uint8_t						m = afa_bdev->info->num_parity_base_bdevs;
	uint64_t					base_lba;
	struct afa_base_bdev		*base_bdev;
	struct spdk_io_channel		*base_ch;
	struct read_io_cb_arg		*arg = calloc(1, sizeof(struct read_io_cb_arg));;
	int 						ret = 0;

	if(afa_chunk_no == UINT32_MAX) {
		base_lba = rand() % TRANSIENT_AREA_OFFSET;
		base_bdev_idx = rand() % afa_bdev->info->num_base_bdevs;
		base_bdev = &afa_bdev->base_bdevs[base_bdev_idx];
		base_ch = afa_io->afa_ch->base_channel[base_bdev_idx];
		arg->afa_io = afa_io;
		arg->base_bdev_idx = base_bdev_idx;
		ret = spdk_bdev_readv_blocks(base_bdev->desc, base_ch, iovs, iovcnt, 
						 base_lba, num_blocks, afa_base_read_io_complete, arg);
		return;
	}

	afa_strip_chunk_no = afa_chunk_no / afa_bdev->info->num_data_base_bdevs;
	slot = afa_chunk_no % k;
	switch (afa_bdev->scis[afa_strip_chunk_no].state)
	{
	case DUPLICATE:
		ret = choose_read_base_bdev_gc_avoid(afa_bdev, afa_strip_chunk_no, slot, &base_bdev_idx);
		if(ret) {
			base_lba = (afa_bdev->scis[afa_strip_chunk_no].pair_sc_offsets[ret - 1] * k + slot)
						* afa_bdev->info->chunk_size_block + offset_blocks;
		}
		else {
			base_lba = TRANSIENT_AREA_OFFSET + afa_strip_chunk_no  * afa_bdev->info->chunk_size_block + offset_blocks;
		}
		base_bdev = &afa_bdev->base_bdevs[base_bdev_idx];
		base_ch = afa_io->afa_ch->base_channel[base_bdev_idx];
		arg->afa_io = afa_io;
		arg->base_bdev_idx = base_bdev_idx;
		ret = spdk_bdev_readv_blocks(base_bdev->desc, base_ch, iovs, iovcnt, 
						 base_lba, num_blocks, afa_base_read_io_complete, arg);
		break;
	case STRIPE:
		base_bdev_idx = k_num_without_array(slot, m, afa_bdev->scis[afa_strip_chunk_no].parity_idxs);
		base_lba = TRANSIENT_AREA_OFFSET + afa_strip_chunk_no  * afa_bdev->info->chunk_size_block + offset_blocks;
		base_bdev = &afa_bdev->base_bdevs[base_bdev_idx];
		base_ch = afa_io->afa_ch->base_channel[base_bdev_idx];
		arg->afa_io = afa_io;
		arg->base_bdev_idx = base_bdev_idx;
		ret = spdk_bdev_readv_blocks(base_bdev->desc, base_ch, iovs, iovcnt, 
						 base_lba, num_blocks, afa_base_read_io_complete, arg);
		break;
	default:
		assert(0);
	}

	if (ret == 0) {
		afa_io->base_bdev_io_submitted++;
	} else if (ret == -ENOMEM) {
		// TODO
		assert(0);
		return;
	} else {
		SPDK_ERRLOG("base io submit error not due to ENOMEM, it should not happen\n");
		assert(0);
	}
}


/** 
 * Handle read request.
 * \param io - the read io 
 */
static void 
afa_bdev_handle_read_request(struct afa_bdev_io *io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(io);
	struct afa_bdev			*afa_bdev = io->afa_bdev;
	uint32_t				start_chunk;
	uint32_t				end_chunk;
	uint32_t				start_offset_in_chunk;
	uint32_t				end_offset_in_chunk;
	struct iovec			*base_iovec;
	size_t i = 0;
	uint32_t j = 0;

	/* DO NOT SUPPORT MULTIPLE IOV NOW. */
	assert(bdev_io->u.bdev.iovcnt == 1);

	start_chunk = bdev_io->u.bdev.offset_blocks / afa_bdev->info->chunk_size_block;
	end_chunk = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) / afa_bdev->info->chunk_size_block;
	start_offset_in_chunk = bdev_io->u.bdev.offset_blocks % afa_bdev->info->chunk_size_block;
	end_offset_in_chunk = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) % afa_bdev->info->chunk_size_block;
	
	if(start_chunk == end_chunk)
	{
		io->base_bdev_io_remaining = 1;
		afa_bdev_handle_chunk_read_request(io, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, start_chunk, 
								start_offset_in_chunk, end_offset_in_chunk - start_offset_in_chunk + 1);
		return;
	}

	io->base_bdev_io_remaining += end_chunk - start_chunk + 1;

	/* The first chunk may not be complete. */
	base_iovec = calloc(1, sizeof(struct iovec));
	base_iovec[0].iov_base = bdev_io->u.bdev.iovs[0].iov_base;
	base_iovec[0].iov_len = (afa_bdev->info->chunk_size_block - start_offset_in_chunk) * afa_bdev->bdev.blocklen;
	i += base_iovec->iov_len;
	afa_bdev_handle_chunk_read_request(io, base_iovec, 1, start_chunk, start_offset_in_chunk, 
						afa_bdev->info->chunk_size_block - start_offset_in_chunk);

	/* Strips between them certainly have aligned offset and length to boundaries. */
	for(j = start_chunk + 1; j < end_chunk; ++j)
	{
		base_iovec = calloc(1, sizeof(struct iovec));
		base_iovec[0].iov_base = bdev_io->u.bdev.iovs[0].iov_base + i;
		base_iovec[0].iov_len = afa_bdev->info->chunk_size_block * afa_bdev->bdev.blocklen;
		i += base_iovec->iov_len;
		afa_bdev_handle_chunk_read_request(io, base_iovec, 1, j, 0, afa_bdev->info->chunk_size_block);
	}

	/* The end strip may not be complete. */
	base_iovec = calloc(1, sizeof(struct iovec));
	base_iovec[0].iov_base = bdev_io->u.bdev.iovs[0].iov_base + i;
	base_iovec[0].iov_len = (end_offset_in_chunk + 1) * afa_bdev->bdev.blocklen;
	i += base_iovec->iov_len;
	afa_bdev_handle_chunk_read_request(io, base_iovec, 1, end_chunk, 0, 
						end_offset_in_chunk + 1);

	assert(i == bdev_io->u.bdev.iovs[0].iov_len);
}


static void afa_split_write_io(struct afa_split_io **ios, int *num_ios, struct afa_bdev_io *afa_io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(afa_io);
	struct afa_bdev			*afa_bdev = afa_io->afa_bdev;
	uint32_t				start_chunk;
	uint32_t				end_chunk;
	uint32_t				start_offset_in_chunk;
	uint32_t				end_offset_in_chunk;
	struct afa_split_io		*res;
	int	i, num = 0;
	size_t p = 0;

	start_chunk = bdev_io->u.bdev.offset_blocks / afa_bdev->info->chunk_size_block;
	end_chunk = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) / afa_bdev->info->chunk_size_block;
	start_offset_in_chunk = bdev_io->u.bdev.offset_blocks % afa_bdev->info->chunk_size_block;
	end_offset_in_chunk = (bdev_io->u.bdev.offset_blocks + bdev_io->u.bdev.num_blocks - 1) % afa_bdev->info->chunk_size_block;

	num = end_chunk - start_chunk + 1;
	res = calloc(num, sizeof(struct afa_split_io));
	
	if(num == 1) {
		res[0].parent_io = afa_io;
		res[0].user_offset_block = bdev_io->u.bdev.offset_blocks;
		res[0].num_blocks = bdev_io->u.bdev.num_blocks;
		res[0].iovcnt = 1;
		res[0].iovs = calloc(1, sizeof(struct iovec));
		res[0].iovs[0].iov_base = bdev_io->u.bdev.iovs[0].iov_base;
		res[0].iovs[0].iov_len = res[0].num_blocks * afa_bdev->bdev.blocklen;
		p += res[0].iovs[0].iov_len;
	}
	else {
		res[0].parent_io = afa_io;
		res[0].user_offset_block = bdev_io->u.bdev.offset_blocks;
		res[0].num_blocks = afa_bdev->info->chunk_size_block - start_offset_in_chunk;
		res[0].iovcnt = 1;
		res[0].iovs = calloc(1, sizeof(struct iovec));
		res[0].iovs[0].iov_base = bdev_io->u.bdev.iovs[0].iov_base;
		res[0].iovs[0].iov_len = res[0].num_blocks * afa_bdev->bdev.blocklen;
		p += res[0].iovs[0].iov_len;
		for(i = 1; i < num - 1; i++) {
			res[i].parent_io = afa_io;
			res[i].user_offset_block = bdev_io->u.bdev.offset_blocks + p / afa_bdev->bdev.blocklen;
			res[i].num_blocks = afa_bdev->info->chunk_size_block;
			res[i].iovcnt = 1;
			res[i].iovs = calloc(1, sizeof(struct iovec));
			res[i].iovs[0].iov_base = bdev_io->u.bdev.iovs[0].iov_base + p;
			res[i].iovs[0].iov_len = res[i].num_blocks * afa_bdev->bdev.blocklen;
			p += res[i].iovs[0].iov_len;
		}
		res[i].parent_io = afa_io;
		res[i].user_offset_block = bdev_io->u.bdev.offset_blocks + p / afa_bdev->bdev.blocklen;
		res[i].num_blocks = end_offset_in_chunk + 1;
		res[i].iovcnt = 1;
		res[i].iovs = calloc(1, sizeof(struct iovec));
		res[i].iovs[0].iov_base = bdev_io->u.bdev.iovs[0].iov_base + p;
		res[i].iovs[0].iov_len = res[i].num_blocks * afa_bdev->bdev.blocklen;
		p += res[i].iovs[0].iov_len;
	}

	assert(p == bdev_io->u.bdev.iovs[0].iov_len);

	*num_ios = num;
	*ios = res;
}

static
uint16_t get_one_free_pair_sc(struct mlh *head, uint32_t *cnt) {
	struct mlh res;
	uint32_t num;

	res.first = NULL;
	num = merge_list_get(head, 1, &res);
	
	assert(num == 1);

	(*cnt)--;
	return res.first->begin;
}


static void 
choose_parity_idxs(struct afa_bdev *afa_bdev, struct afa_io_channel *ch, uint32_t sc_no)
{	
	uint8_t k = afa_bdev->info->num_data_base_bdevs;
	uint8_t m = afa_bdev->info->num_parity_base_bdevs;
	uint8_t num_candidates = 0;
	uint32_t *num_free;
	uint8_t  *idx;
	uint32_t max = 0, max_idx = 0;
	struct ask_pair_sc_arg *arg;
	int i, j, p;

	afa_bdev->scis[sc_no].parity_idxs = calloc(m, sizeof(uint8_t));
	afa_bdev->scis[sc_no].pair_sc_offsets = calloc(m, sizeof(uint16_t));

	if(afa_bdev->num_gcing_base_bdevs > k - 1) {	// Cannot avoid GC interference.
		num_candidates = k + m;
		num_free = calloc(num_candidates, sizeof(uint32_t));
		idx = calloc(num_candidates, sizeof(uint8_t));
		for(i = 0; i < num_candidates; ++i) {
			num_free[i] = ch->num_free_pair_sc[i];
			idx[i] = i;
		}
	}
	else {
		num_free = calloc(k+m, sizeof(uint32_t));
		idx = calloc(k+m, sizeof(uint8_t));
		for(i = 0; i < k+m; ++i) {
			if(afa_bdev->base_bdevs[i].info->state == NORMAL) {
				num_free[num_candidates] = ch->num_free_pair_sc[i];
				idx[num_candidates] = i;
				num_candidates++;
			}
		}
	}

	j = 0;
	while(j < m) {
		for(i = rand() % num_candidates, p = 0; p < num_candidates; ++p, i = (i+1) % num_candidates) {
			if(num_free[i] > max) {
				max = num_free[i];
				max_idx = idx[i];
			}
		}
		if(max != 0) {
			afa_bdev->scis[sc_no].parity_idxs[j] = max_idx;
			afa_bdev->scis[sc_no].pair_sc_offsets[j] = get_one_free_pair_sc(&ch->free_pair_sc[max_idx], &ch->num_free_pair_sc[max_idx]);
			if(ch->num_free_pair_sc[max_idx] < LOW_THRESHOLD_SUPP_PAIR_SC) {
				struct spdk_thread *t = spdk_get_thread();
				arg = calloc(1, sizeof(struct ask_pair_sc_arg));
				arg->base_bdev_idx = max_idx;
				arg->ch = ch;
				spdk_thread_send_msg(afa_bdev->daemon, afa_handle_ask_pair_sc, arg);
			}
			num_free[max_idx] = 0;
			j++;
		}
		else {
			struct spdk_thread *t = spdk_get_thread();
			arg = calloc(1, sizeof(struct ask_pair_sc_arg));
			arg->base_bdev_idx = max_idx;
			arg->ch = ch;
			spdk_set_thread(afa_bdev->daemon);
			afa_handle_ask_pair_sc(arg);
			spdk_set_thread(t);
			return choose_parity_idxs(afa_bdev, ch, sc_no);
		}
	}
	
}


static void
afa_bdev_handle_chunk_write_request(struct afa_bdev_io *afa_io, struct iovec *iovs, int iovcnt,uint8_t base_bdev_idx, 
				uint32_t base_lba, uint32_t num_block, uint32_t user_chunk_no, uint32_t afa_chunk_no, bool change_cmt, bool transient)
{
	global_wrt_cnt++;

	struct afa_bdev				*afa_bdev = afa_io->afa_bdev;
	struct afa_base_bdev		*base_bdev;
	struct spdk_io_channel		*base_ch;
	struct write_io_cb_arg		*arg = calloc(1, sizeof(struct write_io_cb_arg));
	uint8_t slot = afa_chunk_no % afa_bdev->info->num_data_base_bdevs;
	int ret;

	arg->afa_chunk_no = afa_chunk_no;
	arg->user_chunk_no = user_chunk_no;
	arg->change_cmt = change_cmt;
	arg->afa_io = afa_io;
	arg->base_bdev_idx = base_bdev_idx;
	base_bdev = &afa_bdev->base_bdevs[base_bdev_idx];
	base_ch = afa_io->afa_ch->base_channel[base_bdev_idx];
	
	ret = spdk_bdev_writev_blocks_afa_spec(base_bdev->desc, base_ch, iovs, iovcnt, 
						 base_lba, num_block, afa_base_write_io_complete, arg, transient, user_chunk_no, slot);
	if (ret == 0) {
		afa_io->base_bdev_io_submitted++;
	} else if (ret == -ENOMEM) {
		assert(0);
		return;
	} else {
		SPDK_ERRLOG("base io submit error not due to ENOMEM, it should not happen\n");
		assert(0);
	}

}


static void
modify_write(struct spdk_bdev_io *base_io, bool success, void *cb_arg)
{
	struct mw_arg 		*arg = cb_arg;
	struct afa_split_io *sio = arg->sio;
	struct afa_bdev		*afa_bdev = sio->parent_io->afa_bdev;
	struct afa_io_channel	*afa_ch = sio->parent_io->afa_ch;
	struct iovec		*iovec;
	char 				*buf = arg->buf;
	uint64_t			base_lba;
	uint32_t			sc_no;
	uint32_t 			offset_block = sio->user_offset_block % afa_bdev->info->chunk_size_block;
	uint32_t			user_chunk_no = sio->user_offset_block / afa_bdev->info->chunk_size_block;
	uint32_t			afa_chunk_no;
	uint8_t				k = afa_bdev->info->num_data_base_bdevs;
	uint8_t				m = afa_bdev->info->num_parity_base_bdevs;
	uint8_t 			base_bdev_idx;
	uint8_t				slot;
	uint32_t i;

	if(success) {
		// modify
		memcpy(arg->buf + offset_block * afa_bdev->bdev.blocklen, sio->iovs[0].iov_base, 
			sio->num_blocks * afa_bdev->bdev.blocklen);
		
		// write
		sc_no = get_one_empty_chunk(afa_bdev, afa_ch, &slot);

		assert(afa_bdev->scis[sc_no].used_chunks <= k);

		base_bdev_idx = k_num_without_array(slot, m, afa_bdev->scis[sc_no].parity_idxs);
		base_lba = TRANSIENT_AREA_OFFSET + sc_no * afa_bdev->info->chunk_size_block;
		afa_chunk_no = sc_no * k + slot;
		iovec = calloc(1, sizeof(struct iovec));
		iovec[0].iov_base = buf;
		iovec[0].iov_len = afa_bdev->info->chunk_size_block * afa_bdev->bdev.blocklen;
		afa_bdev_handle_chunk_write_request(sio->parent_io, iovec, 1, base_bdev_idx, base_lba, 
							sio->num_blocks, user_chunk_no, afa_chunk_no, true, false);
		sio->parent_io->base_bdev_io_remaining++;

		for(i = 0; i < m; ++i) {
			base_bdev_idx = afa_bdev->scis[sc_no].parity_idxs[i];
			base_lba = (afa_bdev->scis[sc_no].pair_sc_offsets[i] * k + slot) 
						* afa_bdev->info->chunk_size_block + sio->user_offset_block % afa_bdev->info->chunk_size_block;
			afa_bdev_handle_chunk_write_request(sio->parent_io, iovec, 1, base_bdev_idx, base_lba, sio->num_blocks, user_chunk_no, afa_chunk_no, false, true);
			sio->parent_io->base_bdev_io_remaining++;
		}

		
	}
	else assert(0);

	spdk_bdev_free_io(base_io);
	free(arg);
}

static void
write_after_read(struct spdk_bdev_io *base_io, bool success, void *cb_arg) {
	struct war_arg 		*arg = (struct war_arg *)cb_arg;
	struct afa_bdev		*afa_bdev = arg->afa_bdev;
	char 				*buf = arg->buf;
	uint32_t			user_chunk_no = arg->user_chunk_no;

	struct spdk_io_channel *afa_ch = spdk_get_io_channel(afa_bdev);
	struct spdk_bdev_desc *desc;
	spdk_bdev_open_ext(afa_bdev->info->name, true, afa_bdev_event_base_bdev, NULL, &desc);

	uint64_t			base_lba;
	struct iovec		*iovec;

	if(success) {
		iovec = calloc(1, sizeof(struct iovec));
		iovec[0].iov_base = buf;
		iovec[0].iov_len = afa_bdev->info->chunk_size_block * afa_bdev->bdev.blocklen;
		base_lba = user_chunk_no * afa_bdev->info->chunk_size_block;
		spdk_bdev_writev_blocks(desc, afa_ch, iovec, 1, 
						 base_lba, afa_bdev->info->chunk_size_block, NULL, NULL);
		
	}
	else assert(0);

	spdk_bdev_free_io(base_io);
	free(arg);
}


static void afa_bdev_handle_spilt_write_request(struct afa_split_io *sio)
{
	struct afa_bdev			*afa_bdev = sio->parent_io->afa_bdev;
	struct afa_io_channel	*afa_ch = sio->parent_io->afa_ch;
	struct iovec			*base_iovec;
	struct mw_arg			*rmw_arg;
	uint8_t					k = afa_bdev->info->num_data_base_bdevs;
	uint8_t					m = afa_bdev->info->num_parity_base_bdevs;
	uint8_t 				base_bdev_idx;
	uint8_t 				slot;
	uint32_t				sc_no;
	uint32_t				user_chunk_no = sio->user_offset_block / afa_bdev->info->chunk_size_block;
	uint32_t				afa_chunk_no;
	uint64_t				base_lba;
	uint32_t 				i;
	char 					*buf = NULL;

	if(afa_bdev->cmt[user_chunk_no] != UINT32_MAX) 
	{	
		afa_chunk_no = afa_bdev->cmt[user_chunk_no];
		sc_no = afa_chunk_no / k;
		base_bdev_idx = k_num_without_array(afa_chunk_no % k, m, afa_bdev->scis[sc_no].parity_idxs);
		if(afa_bdev->scis[afa_chunk_no / k].state == DUPLICATE) {
			base_lba = TRANSIENT_AREA_OFFSET + sc_no * afa_bdev->info->chunk_size_block + 
							sio->user_offset_block % afa_bdev->info->chunk_size_block;
			base_iovec = calloc(1, sizeof(struct iovec));
			base_iovec[0].iov_base = sio->iovs[0].iov_base;
			base_iovec[0].iov_len = sio->iovs[0].iov_len;
			afa_bdev_handle_chunk_write_request(sio->parent_io, base_iovec, 1, base_bdev_idx, base_lba, 
								sio->num_blocks, user_chunk_no, afa_chunk_no, false, false);
			sio->parent_io->base_bdev_io_remaining++;

			for(i = 0; i < m; ++i) {
				base_bdev_idx = afa_bdev->scis[sc_no].parity_idxs[i];
				base_lba = (afa_bdev->scis[sc_no].pair_sc_offsets[i] * k + afa_chunk_no % k) 
							* afa_bdev->info->chunk_size_block + sio->user_offset_block % afa_bdev->info->chunk_size_block;
				afa_bdev_handle_chunk_write_request(sio->parent_io, base_iovec, 1, base_bdev_idx, base_lba, sio->num_blocks, user_chunk_no, afa_chunk_no, false, true);
				sio->parent_io->base_bdev_io_remaining++;
			}
		}
		else if(afa_bdev->scis[afa_chunk_no / k].state == STRIPE) {
			buf = spdk_zmalloc(afa_bdev->info->chunk_size_block * afa_bdev->bdev.blocklen, 
						0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
			base_lba = TRANSIENT_AREA_OFFSET + sc_no * afa_bdev->info->chunk_size_block;
			rmw_arg = calloc(1, sizeof(struct mw_arg));
			rmw_arg->sio = sio;
			rmw_arg->buf = buf;
			spdk_bdev_read_blocks(afa_bdev->base_bdevs[base_bdev_idx].desc, afa_ch->base_channel[base_bdev_idx], buf,
						base_lba, afa_bdev->info->chunk_size_block, modify_write, rmw_arg);
		}
		else assert(0);
	}
	else {
		sc_no = get_one_empty_chunk(afa_bdev, afa_ch, &slot);

		assert(afa_bdev->scis[sc_no].used_chunks <= k);

		base_bdev_idx = k_num_without_array(slot, m, afa_bdev->scis[sc_no].parity_idxs);
		base_lba = TRANSIENT_AREA_OFFSET + sc_no * afa_bdev->info->chunk_size_block + 
						sio->user_offset_block % afa_bdev->info->chunk_size_block;
		afa_chunk_no = sc_no * k + slot;
		base_iovec = calloc(1, sizeof(struct iovec));
		base_iovec[0].iov_base = sio->iovs[0].iov_base;
		base_iovec[0].iov_len = sio->iovs[0].iov_len;
		afa_bdev_handle_chunk_write_request(sio->parent_io, base_iovec, 1, base_bdev_idx, base_lba, 
							sio->num_blocks, user_chunk_no, afa_chunk_no, true, false);
		sio->parent_io->base_bdev_io_remaining++;

		for(i = 0; i < m; ++i) {
			base_bdev_idx = afa_bdev->scis[sc_no].parity_idxs[i];
			base_lba = (afa_bdev->scis[sc_no].pair_sc_offsets[i] * k + slot) 
						* afa_bdev->info->chunk_size_block + sio->user_offset_block % afa_bdev->info->chunk_size_block;
			afa_bdev_handle_chunk_write_request(sio->parent_io, base_iovec, 1, base_bdev_idx, base_lba, sio->num_blocks, user_chunk_no, afa_chunk_no, false, true);
			sio->parent_io->base_bdev_io_remaining++;
		}
	}
}


static void 
afa_bdev_handle_write_request(struct afa_bdev_io *io)
{
	struct spdk_bdev_io		*bdev_io = spdk_bdev_io_from_ctx(io);
	struct afa_split_io		*split_ios;
	uint32_t				num_split_ios = 0;
	uint32_t i;

	/* DO NOT SUPPORT MULTIPLE IOV NOW. */
	assert(bdev_io->u.bdev.iovcnt == 1);

	afa_split_write_io(&split_ios, &num_split_ios, io);

	for(i = 0; i < num_split_ios; ++i) {
		afa_bdev_handle_spilt_write_request(&split_ios[i]);
	}
}



/**
 * afa_bdev_submit_request function is the submit_request function pointer of
 * afa bdev function table. This is used to submit the io on afa_bdev to below
 * layers.
 * \param ch - pointer to afa bdev io channel
 * \param bdev_io - pointer to parent bdev_io on afa bdev device
 */
static void
afa_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct afa_bdev_io *afa_io = (struct afa_bdev_io *)bdev_io->driver_ctx;

	afa_io->afa_bdev = bdev_io->bdev->ctxt;
	afa_io->afa_ch = spdk_io_channel_get_ctx(ch);
	afa_io->base_bdev_io_remaining = 0;
	afa_io->base_bdev_io_submitted = 0;
	afa_io->base_bdev_io_status  = SPDK_BDEV_IO_STATUS_SUCCESS;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, afa_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		afa_bdev_handle_write_request(afa_io);
		break;
	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		afa_bdev_io_complete(afa_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}


/**
 * afa_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * afa bdev module
 * \param ctx - pointer to afa bdev context
 * \param type - io type
 * \return true - io_type is supported, false - io_type is not supported
 */
static bool
afa_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_ABORT:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}


/**
 * afa_bdev_get_io_channel is the get_io_channel function table pointer for
 * afa bdev. This is used to return the io channel for this afa bdev
 * \param ctx - pointer to afa_bdev
 * \return pointer to io channel for afa bdev
 */
static struct spdk_io_channel *
afa_bdev_get_io_channel(void *ctx)
{
	struct afa_bdev *afa_bdev = ctx;

	return spdk_get_io_channel(afa_bdev);
}


static int
afa_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct afa_bdev *afa_bdev = ctx;
	struct afa_base_bdev *base_bdev;
	int domains_count = 0, rc;

	/* First loop to get the number of memory domains */
	AFA_FOR_EACH_BASE_BDEV(afa_bdev, base_bdev) {
		rc = spdk_bdev_get_memory_domains(base_bdev->bdev, NULL, 0);
		if (rc < 0) return rc;
		domains_count += rc;
	}

	if (!domains || array_size < domains_count) {
		return domains_count;
	}

	AFA_FOR_EACH_BASE_BDEV(afa_bdev, base_bdev){
		rc = spdk_bdev_get_memory_domains(base_bdev->bdev, domains, array_size);
		if (rc < 0) return rc;
		domains += rc;
		array_size -= rc;
	}

	return domains_count;
}


/**
 * afa_bdev_init is the initialization function for afa bdev module,
 * but nothing needs to be done now
 */
static int
afa_bdev_initialize(void)
{
	SPDK_DEBUGLOG(bdev_afa, "initialize afa bdev module.\n");
	return 0;
}


/**
 * afa_bdev_finish is called on afa bdev module exit time by bdev layer,
 * but nothing needs to be done now
 */
static void
afa_bdev_finish(void)
{
	SPDK_DEBUGLOG(bdev_afa, "finish afa bdev module.\n");
}


/**
 * afa_bdev_get_ctx_size is used to return the context size of bdev_io for afa module
 * \return size of spdk_bdev_io context for afa
 */
static int
afa_bdev_get_ctx_size(void)
{
	return sizeof(struct afa_bdev_io);
}


/**
 * afa_bdev_examine function is the examine function call by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this afa bdev or not.
 * \param bdev - pointer to base bdev
 */
static void
afa_bdev_examine(struct spdk_bdev *bdev)
{
	spdk_bdev_module_examine_done(&afa_if);
}


/**
 * afa_bdev_event_base_bdev function is called by below layers when base_bdev
 * triggers asynchronous event.
 * \param type - event details.
 * \param bdev - bdev that triggered event.
 * \param event_ctx - context for event.
 */
static void
afa_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		SPDK_DEBUGLOG(bdev_afa, "afa bdev doesn't support removing base bdev now.\n");
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}


/**
 * \param afa_bdev - the afa bdev to be added on
 * \param base_bdev_name - name of base bdev
 * \param base_bdev_slot - index of the base bdev
 * \param base_bdev_state - state of the base bdev
 * \return 0 - success, non zero - failure
 */
static int 
afa_add_base_bdev(struct afa_bdev *afa_bdev, const char *base_bdev_name, 
				  uint8_t base_bdev_slot, enum afa_base_bdev_state base_bdev_state) {
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *base_bdev;
	int ret;

	ret = spdk_bdev_open_ext(base_bdev_name, true, afa_bdev_event_base_bdev, NULL, &desc);
	if (ret) {
		SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", base_bdev_name);
		return ret;
	}

	base_bdev = spdk_bdev_desc_get_bdev(desc);
	ret = spdk_bdev_module_claim_bdev(base_bdev, NULL, &afa_if);
	if (ret) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return ret;
	}
	SPDK_DEBUGLOG(bdev_afa, "bdev %s is claimed\n", base_bdev_name);

	afa_bdev->base_bdevs[base_bdev_slot].bdev = base_bdev;
	afa_bdev->base_bdevs[base_bdev_slot].desc = desc;
	afa_bdev->base_bdevs[base_bdev_slot].thread = spdk_get_thread();
	afa_bdev->base_bdevs[base_bdev_slot].info = calloc(1, sizeof(struct afa_base_bdev_info));
	afa_bdev->base_bdevs[base_bdev_slot].info->name = strdup(base_bdev_name);
	afa_bdev->base_bdevs[base_bdev_slot].info->state = base_bdev_state;

	return 0;
}

/**
 * receive sc from daemon or other afa channels
 */
void
afa_receive_sc(void *ctx)
{
	struct send_sc_arg *arg = ctx;
	struct afa_io_channel *ch = arg->ch;
	struct afa_bdev	*afa_bdev = arg->afa_bdev;
	struct interval* i;
	struct mlh *p_partial_sc, *p_free_sc;

	assert(ch != NULL || afa_bdev != NULL);
	assert(ch == NULL || afa_bdev == NULL);

	if(ch != NULL) {
		p_partial_sc = &ch->partial_sc;
		p_free_sc = &ch->free_sc;
	}
	else {
		assert(spdk_get_thread() == afa_bdev->daemon);
		p_partial_sc = &afa_bdev->partial_sc;
		p_free_sc = &afa_bdev->free_sc;
	}
	
	i = arg->partial_sc.first;
	while(i != NULL) {
		merge_list_insert(p_partial_sc, i->begin, i->end);
		arg->partial_sc.first = i->next;
		free(i);
		i = arg->partial_sc.first;
	}
	if(ch != NULL) ch->num_partial_sc += arg->num_partial;
	else afa_bdev->num_partial_sc += arg->num_partial;

	i = arg->free_sc.first;
	while(i != NULL) {
		merge_list_insert(p_free_sc, i->begin, i->end);
		arg->free_sc.first = i->next;
		free(i);
		i = arg->free_sc.first;
	}
	if(ch != NULL) ch->num_free_sc += arg->num_free;
	else afa_bdev->num_free_sc += arg->num_free;

	free(arg);
}

/**
 * receive pair sc from daemon or other afa channels
 */
void
afa_receive_pair_sc(void *ctx)
{
	struct send_pair_sc_arg *arg = ctx;
	struct afa_io_channel *ch = arg->ch;
	struct afa_bdev	*afa_bdev = arg->afa_bdev;
	struct interval* i;
	struct mlh *p;
	uint8_t base_bdev_idx = arg->base_bdev_idx;

	assert(ch != NULL || afa_bdev != NULL);
	assert(ch == NULL || afa_bdev == NULL);

	if(ch != NULL) {
		p = &ch->free_pair_sc[base_bdev_idx];
	}
	else {
		assert(spdk_get_thread() == afa_bdev->daemon);
		p = &afa_bdev->base_bdevs[base_bdev_idx].free_pair_sc;
	}
	
	
	i = arg->free_pair_sc.first;
	while(i != NULL) {
		merge_list_insert(p, i->begin, i->end);
		arg->free_pair_sc.first = i->next;
		free(i);
		i = arg->free_pair_sc.first;
	}

	if(ch != NULL) ch->num_free_pair_sc[base_bdev_idx] += arg->num_free_pair_sc;
	else afa_bdev->base_bdevs[base_bdev_idx].num_free_pair_sc += arg->num_free_pair_sc;

	free(arg);
}


/**
 * allocate sc for the given afa io channel
 * \param ctx - pointer to the afa io channel
 */
void
afa_handle_ask_sc(void *ctx)
{
	struct afa_io_channel *ch = ctx;
	struct spdk_io_channel *sch = spdk_io_channel_from_ctx(ctx);
	struct afa_bdev *afa_bdev = spdk_io_channel_get_io_device(sch);
	struct spdk_thread *asker = spdk_io_channel_get_thread(sch);
	uint8_t ask_fac = ch->ask_fac;
	uint32_t num_want = BASE_NUM_SC_ALLOC_ONCE << spdk_min(MAX_NUM_SC_ALLOC_ONCE_SHIFT - BASE_NUM_SC_ALLOC_ONCE_SHIFT, ask_fac);
	struct send_sc_arg *arg = calloc(1, sizeof(struct send_sc_arg));
	
	assert(spdk_get_thread() == afa_bdev->daemon);

	// allocate partial
	arg->partial_sc.first = NULL;
	arg->num_partial = merge_list_get(&afa_bdev->partial_sc, num_want, &arg->partial_sc);
	afa_bdev->num_partial_sc -= arg->num_partial;

	// allocate free sc
	if(arg->num_partial < num_want) {
		arg->free_sc.first = NULL;
		arg->num_free = merge_list_get(&afa_bdev->free_sc, num_want - arg->num_partial, &arg->free_sc);
		afa_bdev->num_free_sc -= arg->num_free;
	}

	ch->ask_fac++;

	arg->ch = ch;
	arg->afa_bdev = NULL;

	if(ch->num_free_sc == 0 && ch->num_partial_sc == 0) {
		afa_receive_sc(arg);
	}
	else spdk_thread_send_msg(asker, afa_receive_sc, arg);
}


/**
 * allocate pair sc for the given afa io channel on base bdev
 * \param ctx - pointer to the afa io channel and base bdev
 */
void
afa_handle_ask_pair_sc(void *ctx)
{
	struct ask_pair_sc_arg *arg = (struct ask_pair_sc_arg*)ctx;
	struct afa_io_channel *ch = arg->ch;
	struct spdk_io_channel *sch = spdk_io_channel_from_ctx(ch);
	struct afa_bdev *afa_bdev = spdk_io_channel_get_io_device(sch);
	struct spdk_thread *asker = spdk_io_channel_get_thread(sch);
	uint8_t base_bdev_idx = arg->base_bdev_idx;
	uint32_t num_want = NUM_PAIR_SC_ALLOC_ONCE;
	struct send_pair_sc_arg *send_arg = calloc(1, sizeof(struct send_pair_sc_arg));
	struct spdk_thread *t = spdk_get_thread();

	if(afa_bdev->base_bdevs[base_bdev_idx].num_free_pair_sc == 0) assert(0);

	send_arg->free_pair_sc.first = NULL;
	send_arg->num_free_pair_sc = merge_list_get(&afa_bdev->base_bdevs[base_bdev_idx].free_pair_sc, num_want, &send_arg->free_pair_sc);
	afa_bdev->base_bdevs[base_bdev_idx].num_free_pair_sc -= send_arg->num_free_pair_sc;

	send_arg->base_bdev_idx = base_bdev_idx;
	send_arg->ch = ch;
	send_arg->afa_bdev = NULL;

	if(ch->num_free_pair_sc[base_bdev_idx] == 0) {
		afa_receive_pair_sc(send_arg);
	}
	else spdk_thread_send_msg(asker, afa_receive_pair_sc, send_arg);
	
	free(arg);
}

/**
 * afa_bdev_create_cb function is a cb function for afa bdev which creates the
 * map from afa bdev io channel to base bdev io channels. It will be called per core.
 * \param io_device - pointer to afa bdev io device
 * \param ctx_buf - pointer to afa bdev io channel
 * \return 0 - success, non zero - failure
 */
static int
afa_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct afa_bdev            *afa_bdev = io_device;
	struct afa_io_channel 	   *afa_ch = ctx_buf;
	struct ask_pair_sc_arg 	   *arg;
	struct spdk_thread *t;
	uint8_t i;

	assert(afa_bdev != NULL);

	afa_ch->num_channels = afa_bdev->info->num_base_bdevs;

	afa_ch->base_channel = calloc(afa_ch->num_channels, sizeof(struct spdk_io_channel *));
	afa_ch->free_pair_sc = calloc(afa_ch->num_channels, sizeof(struct mlh));
	afa_ch->num_free_pair_sc = calloc(afa_ch->num_channels, sizeof(uint32_t));

	if (!afa_ch->base_channel || !afa_ch->num_free_pair_sc || !afa_ch->num_free_pair_sc) {
		SPDK_ERRLOG("Unable to allocate afa base bdevs io channel\n");
		return -ENOMEM;
	}
	for (i = 0; i < afa_ch->num_channels; i++) {
		afa_ch->free_pair_sc[i].first = NULL;
		afa_ch->num_free_pair_sc[i] = 0;
		arg = calloc(1, sizeof(struct ask_pair_sc_arg));
		arg->base_bdev_idx = i;
		arg->ch = afa_ch;

		while(afa_bdev->daemon == NULL) {}
		spdk_thread_send_msg(afa_bdev->daemon, afa_handle_ask_pair_sc, arg);

		afa_ch->base_channel[i] = spdk_bdev_get_io_channel(
						   afa_bdev->base_bdevs[i].desc);
		if (!afa_ch->base_channel[i]) {
			uint8_t j;
			for (j = 0; j < i; j++) {
				spdk_put_io_channel(afa_ch->base_channel[j]);
			}
			free(afa_ch->base_channel);
			afa_ch->base_channel = NULL;
			SPDK_ERRLOG("Unable to create io channel for afa base bdev\n");
			return -ENOMEM;
		}
	}

	afa_ch->free_sc.first = NULL;
	afa_ch->partial_sc.first = NULL;
	afa_ch->partial_sc_lp.first = NULL;
	afa_ch->num_free_sc = 0;
	afa_ch->num_partial_sc = 0;
	afa_ch->num_partial_sc_lp = 0;
	afa_ch->ask_fac = 0;

	spdk_thread_send_msg(afa_bdev->daemon, afa_handle_ask_sc, afa_ch);

	return 0;
}


/**
 * afa_bdev_destroy_cb function is a cb function for afa bdev which deletes the
 * map from afa bdev io channel to base bdev io channels. It will be called per core
 * \param io_device - pointer to afa bdev io device
 * \param ctx_buf - pointer to afa bdev io channel
 */
static void
afa_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct afa_io_channel	*afa_ch = ctx_buf;
	struct afa_bdev			*afa_bdev = io_device;
	struct send_sc_arg		*arg = calloc(1, sizeof(struct send_sc_arg));
	struct send_pair_sc_arg *pair_arg;
	struct interval* p;
	uint8_t i;

	assert(afa_ch != NULL);
	assert(afa_ch->base_channel != NULL);
	for (i = 0; i < afa_ch->num_channels; i++) {
		/* Free base bdev channels */
		assert(afa_ch->base_channel[i] != NULL);
		
		pair_arg = calloc(1, sizeof(struct send_pair_sc_arg));
		pair_arg->afa_bdev = afa_bdev;
		pair_arg->ch = NULL;
		pair_arg->free_pair_sc = afa_ch->free_pair_sc[i];
		pair_arg->num_free_pair_sc = afa_ch->num_free_pair_sc[i];
		pair_arg->base_bdev_idx = i;
		spdk_thread_send_msg(afa_bdev->daemon, afa_receive_pair_sc, pair_arg);

		spdk_put_io_channel(afa_ch->base_channel[i]);
	}
	free(afa_ch->base_channel);
	afa_ch->base_channel = NULL;

	p = afa_ch->partial_sc_lp.first;
	while(p != NULL) {
		merge_list_insert(&afa_ch->partial_sc, p->begin, p->end);
		afa_ch->partial_sc_lp.first = p->next;
		free(p);
		p = afa_ch->partial_sc_lp.first;
	}
	afa_ch->num_partial_sc += afa_ch->num_partial_sc_lp;
	afa_ch->num_partial_sc_lp = 0;

	arg->free_sc = afa_ch->free_sc;
	arg->partial_sc = afa_ch->partial_sc;
	arg->num_free = afa_ch->num_free_sc;
	arg->num_partial = afa_ch->num_partial_sc;
	arg->afa_bdev = afa_bdev;
	arg->ch = NULL;

	spdk_thread_send_msg(afa_bdev->daemon, afa_receive_sc, arg);
	printf("\n\n\n\nwrt_cnt = %ld, redirect_cnt = %ld\n\n\n\n", global_wrt_cnt, global_redirect_cnt);
}


static void
daemon_init(void *ctx)
{
	struct afa_bdev *afa_bdev = (struct afa_bdev *)ctx;
	int i;

	afa_bdev->internal_channel = calloc(afa_bdev->info->num_base_bdevs, sizeof(struct spdk_io_channel *));
	i = spdk_env_get_current_core();

	for (i = 0; i < afa_bdev->info->num_base_bdevs; i++) {
		afa_bdev->internal_channel[i] = spdk_bdev_get_io_channel(
						   afa_bdev->base_bdevs[i].desc);
		if (!afa_bdev->internal_channel [i]) {
			uint8_t j;
			for (j = 0; j < i; j++) {
				spdk_put_io_channel(afa_bdev->internal_channel [j]);
			}
			free(afa_bdev->internal_channel);
			afa_bdev->internal_channel  = NULL;
			SPDK_ERRLOG("Unable to create io channel for afa internal\n");
			assert(0);
		}
	}

	SPDK_POLLER_REGISTER(afa_base_bdev_gc_probe, ctx, GC_PROBE_INTERVAL);
}


static int add_daemon(void *arg) 
{
	struct afa_bdev *afa_bdev = arg;
	struct spdk_cpuset cpu_mask = {};
	char *daemon_name;
	uint32_t i;
	
	spdk_thread_send_msg(afa_bdev->daemon, daemon_init, afa_bdev);

	return 0;
}


void bdev_afa_hello()
{	
	// printf("\t\t███████╗ ██████╗ █████╗ ██╗      █████╗  █████╗ ███████╗ █████╗ \n");
	// printf("\t\t██╔════╝██╔════╝██╔══██╗██║     ██╔══██╗██╔══██╗██╔════╝██╔══██╗\n");
	// printf("\t\t███████╗██║     ███████║██║     ███████║███████║█████╗  ███████║\n");
	// printf("\t\t╚════██║██║     ██╔══██║██║     ██╔══██║██╔══██║██╔══╝  ██╔══██║\n");                                                            
	// printf("\t\t███████║╚██████╗██║  ██║███████╗██║  ██║██║  ██║██║     ██║  ██║\n"); 
	// printf("\t\t╚══════╝ ╚═════╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝     ╚═╝  ╚═╝\n");
	printf("\n\n\n");
	printf("\t\t███████╗ ██████╗ █████╗ ██╗      █████╗ ");
	printf("\033[0m\033[1;31m%s\033[0m", " █████╗ ███████╗ █████╗ \n");
	printf("\t\t██╔════╝██╔════╝██╔══██╗██║     ██╔══██╗");
	printf("\033[0m\033[1;31m%s\033[0m", "██╔══██╗██╔════╝██╔══██╗\n");
	printf("\t\t███████╗██║     ███████║██║     ███████║");
	printf("\033[0m\033[1;31m%s\033[0m", "███████║█████╗  ███████║\n");
	printf("\t\t╚════██║██║     ██╔══██║██║     ██╔══██║");
	printf("\033[0m\033[1;31m%s\033[0m", "██╔══██║██╔══╝  ██╔══██║\n");                                                            
	printf("\t\t███████║╚██████╗██║  ██║███████╗██║  ██║");
	printf("\033[0m\033[1;31m%s\033[0m", "██║  ██║██║     ██║  ██║\n"); 
	printf("\t\t╚══════╝ ╚═════╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝");
	printf("\033[0m\033[1;31m%s\033[0m", "╚═╝  ╚═╝╚═╝     ╚═╝  ╚═╝\n");
	printf("\n\n\n");
}


/**
 * afa_bdev_create allocates afa bdev based on passed options
 * \param bdev - spdk bdev structure to store afa bdev
 * \param opts - options of afa bdev
 * \return 0 - success, non zero - failure
 */
int bdev_afa_create(struct spdk_bdev **bdev, struct afa_bdev_opts *opts)
{
	struct afa_bdev	*afa_bdev;
	struct afa_base_bdev *base_bdev;
	uint32_t blocklen = 0;
	uint64_t min_blockcnt = UINT64_MAX;	// min block len of base bdev
	uint32_t stripe_chunk_shift;
	int ret = 0;
	
	if (opts == NULL) {
		SPDK_ERRLOG("No options provided for Null bdev.\n");
		return -EINVAL;
	}

	if(opts->num_data_base_bdevs == 0 && opts->num_parity_base_bdevs == 0) {
		SPDK_ERRLOG("At least 1 base device are required\n");
		return -EINVAL;
	}

	if(opts->chunk_size_kb & (opts->chunk_size_kb - 1)) {
		SPDK_ERRLOG("Stripe size in KB must be a power of 2.\n");
		return -EINVAL;
	}

	afa_bdev = calloc(1, sizeof(struct afa_bdev));
	if (!afa_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for afa bdev.\n");
		return -ENOMEM;
	}

	afa_bdev->info = calloc(1, sizeof(struct afa_bdev_info));
	if (!afa_bdev->info) {
		SPDK_ERRLOG("Unable able to allocate afa bdev info.\n");
		free(afa_bdev);
		return -ENOMEM;
	}

	afa_bdev->info->afa_bdev = afa_bdev;
	afa_bdev->info->name = strdup(opts->name);
	afa_bdev->info->num_base_bdevs = opts->num_data_base_bdevs + opts->num_parity_base_bdevs + opts->num_spare_base_bdevs;
	afa_bdev->info->num_data_base_bdevs = opts->num_data_base_bdevs;
	afa_bdev->info->num_parity_base_bdevs = opts->num_parity_base_bdevs;
	afa_bdev->info->num_spare_base_bdevs = opts->num_spare_base_bdevs;
	afa_bdev->info->num_failed_base_bdevs = 0;
	afa_bdev->info->chunk_size_kb = opts->chunk_size_kb;

	afa_bdev->base_bdevs = calloc(opts->num_data_base_bdevs + opts->num_parity_base_bdevs + opts->num_spare_base_bdevs,
					   sizeof(struct afa_base_bdev));
	if (!afa_bdev->base_bdevs) {
		SPDK_ERRLOG("Unable able to allocate afa base bdev.\n");
		free(afa_bdev->info);
		free(afa_bdev);
		return -ENOMEM;
	}

	for(uint8_t i = 0; i < afa_bdev->info->num_base_bdevs; ++i) {
		if(i < afa_bdev->info->num_data_base_bdevs + afa_bdev->info->num_parity_base_bdevs){
			ret = afa_add_base_bdev(afa_bdev, opts->base_bdevs[i].name, i, NORMAL);
		}
		else ret = afa_add_base_bdev(afa_bdev, opts->base_bdevs[i].name, i, SPARE);	
		
		if(ret) {
			SPDK_ERRLOG("Unable able to add afa base bdev.\n");
			free(afa_bdev->base_bdevs);
			free(afa_bdev->info);
			free(afa_bdev);
			return ret;
		}
	}

	AFA_FOR_EACH_BASE_BDEV(afa_bdev, base_bdev) {
		/* Check blocklen for all base bdevs that it should be same */
		if (blocklen == 0) {
			blocklen = base_bdev->bdev->blocklen;
		} else if (blocklen != base_bdev->bdev->blocklen) {
			SPDK_ERRLOG("Blocklen of various bdevs not matching\n");
			return -EINVAL;
		}
		min_blockcnt = spdk_min(min_blockcnt, base_bdev->bdev->blockcnt);
	}
	assert(blocklen > 0);
	afa_bdev->info->chunk_size_block = (opts->chunk_size_kb << 10) / blocklen;
	stripe_chunk_shift = spdk_u64log2(afa_bdev->info->chunk_size_block * opts->num_data_base_bdevs);
	afa_bdev->bdev.blocklen = blocklen;
	afa_bdev->bdev.blockcnt = (min_blockcnt - TRANSIENT_AREA_OFFSET) * opts->num_data_base_bdevs;
	afa_bdev->bdev.blockcnt = (afa_bdev->bdev.blockcnt >> stripe_chunk_shift) << stripe_chunk_shift;
	afa_bdev->bdev.name = strdup(opts->name);
	afa_bdev->bdev.product_name = "All Flash Array";
	afa_bdev->bdev.ctxt = afa_bdev;
	afa_bdev->bdev.fn_table = &g_afa_bdev_fn_table;
	afa_bdev->bdev.module = &afa_if;
	afa_bdev->bdev.write_cache = 0;

	afa_bdev->info->num_chunks = afa_bdev->bdev.blockcnt / afa_bdev->info->chunk_size_block;
	afa_bdev->info->num_stripe_chunks = afa_bdev->info->num_chunks / afa_bdev->info->num_data_base_bdevs;

	AFA_FOR_EACH_BASE_BDEV(afa_bdev, base_bdev) {
		base_bdev->num_free_pair_sc = TRANSIENT_AREA_OFFSET / afa_bdev->info->chunk_size_block / afa_bdev->info->num_data_base_bdevs;
		base_bdev->free_pair_sc.first = calloc(1, sizeof(struct interval));
		base_bdev->free_pair_sc.first->begin = 0;
		base_bdev->free_pair_sc.first->end = base_bdev->num_free_pair_sc;
		base_bdev->free_pair_sc.first->next = NULL;
	}

	afa_bdev->cmt = calloc(afa_bdev->info->num_chunks, sizeof(uint32_t));
	memset(afa_bdev->cmt, 0xff, afa_bdev->info->num_chunks * sizeof(uint32_t));
	if(!afa_bdev->cmt) {
		SPDK_ERRLOG("Unable able to allocate chunk mapping table.\n");
		free(afa_bdev->base_bdevs);
		free(afa_bdev->info);
		free(afa_bdev);
		return -ENOMEM;
	}
	afa_bdev->rcmt = calloc(afa_bdev->info->num_chunks, sizeof(uint32_t));
	memset(afa_bdev->rcmt, 0xff, afa_bdev->info->num_chunks * sizeof(uint32_t));
	if(!afa_bdev->rcmt) {
		SPDK_ERRLOG("Unable able to allocate reversed chunk mapping table.\n");
		free(afa_bdev->base_bdevs);
		free(afa_bdev->info);
		free(afa_bdev->cmt);
		free(afa_bdev);
		return -ENOMEM;
	}
	afa_bdev->rcmt_bm = calloc(afa_bdev->info->num_chunks, sizeof(uint8_t));
	if(!afa_bdev->rcmt_bm) {
		SPDK_ERRLOG("Unable able to allocate chunk mapping table bitmap.\n");
		free(afa_bdev->base_bdevs);
		free(afa_bdev->info);
		free(afa_bdev->cmt);
		free(afa_bdev->rcmt);
		free(afa_bdev);
		return -ENOMEM;
	}

	afa_bdev->scis = calloc(afa_bdev->info->num_stripe_chunks, sizeof(struct stripe_chunk_info));
	if(!afa_bdev->scis) {
		SPDK_ERRLOG("Unable able to allocate chunk pairing directory.\n");
		free(afa_bdev->base_bdevs);
		free(afa_bdev->info);
		free(afa_bdev->cmt);
		free(afa_bdev->rcmt);
		free(afa_bdev->rcmt_bm);
		free(afa_bdev);
		return -ENOMEM;
	}
	for(uint32_t i = 0; i < afa_bdev->info->num_stripe_chunks; ++i) afa_bdev->scis[i].no = i;

	afa_bdev->num_free_sc = afa_bdev->info->num_stripe_chunks;
	afa_bdev->free_sc.first = calloc(1, (sizeof(struct interval)));
	afa_bdev->free_sc.first->begin = 0;
	afa_bdev->free_sc.first->end = afa_bdev->num_free_sc - 1;
	afa_bdev->free_sc.first->next = NULL;
	afa_bdev->partial_sc.first = NULL;
	afa_bdev->num_partial_sc = 0;
	TAILQ_INIT(&(afa_bdev->sc_need_switch));

	afa_bdev->victim_sc_pq = pqueue_init(afa_bdev->info->num_stripe_chunks, victim_sc_cmp_pri,
										 victim_sc_get_pri, victim_sc_set_pri,
										 victim_sc_get_pos, victim_sc_set_pos);

	struct spdk_cpuset cpu_mask = {};
	char *daemon_name;
	spdk_cpuset_set_cpu(&cpu_mask, spdk_env_get_last_core(), true);
	daemon_name = malloc(strlen(afa_bdev->info->name) + strlen(" daemon"));
	strcpy(daemon_name, afa_bdev->info->name);
    strcat(daemon_name, " daemon");
	afa_bdev->daemon = spdk_thread_create(daemon_name, &cpu_mask);
	add_daemon(afa_bdev);

	spdk_io_device_register(afa_bdev, afa_bdev_create_cb, afa_bdev_destroy_cb,
				sizeof(struct afa_io_channel), afa_bdev->bdev.name);

	ret = spdk_bdev_register(&afa_bdev->bdev);

	if (ret) {
		SPDK_ERRLOG("Unable able to register afa bdev.\n");
		free(afa_bdev->base_bdevs);
		free(afa_bdev->info);
		free(afa_bdev->cmt);
		free(afa_bdev->rcmt);
		free(afa_bdev->rcmt_bm);
		free(afa_bdev->scis);
		free(afa_bdev);
		return ret;
	}

	*bdev = &afa_bdev->bdev;

	TAILQ_INSERT_TAIL(&g_afa_bdev_head, afa_bdev, tailq);

	bdev_afa_hello();

	return 0;
}


void
bdev_afa_delete(const char *bdev_name, spdk_delete_afa_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(bdev_name, &afa_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}


static void
free_sc_base_unmap_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	assert(success);

	struct sc_io *sc_io = cb_arg;
	struct afa_bdev *afa_bdev = sc_io->afa_bdev;
	uint32_t sc_no = sc_io->sc_no;

	assert(sc_io->base_bdev_io_remaining >= 1);
	sc_io->base_bdev_io_remaining -= 1;
	
	if (sc_io->base_bdev_io_remaining == 0) {
		if(afa_bdev->scis[sc_no].state != CLEAN) {
			afa_bdev->scis[sc_no].state = CLEAN;
			free(afa_bdev->scis[sc_no].bitmap);
			free(afa_bdev->scis[sc_no].pair_sc_offsets);
			free(afa_bdev->scis[sc_no].parity_idxs);
			afa_bdev->num_free_sc++;
			merge_list_insert(&afa_bdev->free_sc, sc_no, sc_no + 1);
			free(sc_io);
		}
	}

	spdk_bdev_free_io(bdev_io);
}



static void 
gc_free_stripe_chunk(struct afa_bdev *afa_bdev, uint32_t sc_no)
{	
	struct afa_base_bdev   *base_bdev;
	struct spdk_io_channel *base_ch;
	struct sc_io *sc_io;
	struct war_arg *arg = calloc(1, sizeof(struct war_arg));
	uint8_t k = afa_bdev->info->num_data_base_bdevs;
	uint8_t m = afa_bdev->info->num_parity_base_bdevs;
	uint8_t base_bdev_idx;
	uint32_t afa_chunk_no;
	int i, ret;
	char *buf = NULL;
	uint64_t base_lba;

	assert(afa_bdev->scis[sc_no].state == STRIPE);

	for(i = 0; i < k; ++i) {
		afa_chunk_no = sc_no * k + i;
		if(afa_bdev->rcmt_bm[afa_chunk_no] == 1) {	// valid
			// TODO Match Remap 优化
			buf = spdk_zmalloc(afa_bdev->info->chunk_size_block * afa_bdev->bdev.blocklen, 
						0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
			base_lba = TRANSIENT_AREA_OFFSET + sc_no * afa_bdev->info->chunk_size_block;
			base_bdev_idx = k_num_without_array(i, m, afa_bdev->scis[sc_no].parity_idxs);
			base_bdev = &afa_bdev->base_bdevs[base_bdev_idx];
			base_ch = afa_bdev->internal_channel[base_bdev_idx];
			arg->afa_bdev = afa_bdev;
			arg->buf = buf;
			arg->user_chunk_no = afa_bdev->rcmt[afa_chunk_no];
			spdk_bdev_read_blocks(base_bdev->desc, base_ch, buf, base_lba, afa_bdev->info->chunk_size_block, write_after_read, arg);
		}
	}

	sc_io = calloc(1, sizeof(struct sc_io));
	sc_io->afa_bdev = afa_bdev;
	sc_io->sc_no = sc_no;
	sc_io->base_bdev_io_remaining = k+m;
	for(i = 0; i < k+m; ++i) {
		base_bdev = &afa_bdev->base_bdevs[i];
		base_ch = afa_bdev->internal_channel[i];
		ret = spdk_bdev_unmap_blocks(base_bdev->desc, base_ch,  TRANSIENT_AREA_OFFSET + sc_no * afa_bdev->info->chunk_size_block, 
									afa_bdev->info->chunk_size_block, free_sc_base_unmap_complete, sc_io);
		
		if (ret == 0) {
			sc_io->base_bdev_io_submitted++;
		} 
		else{ 
			SPDK_ERRLOG("base io submit error not due to ENOMEM, it should not happen\n");
			assert(0);
		}
	}
	
	
}


/**
 * garbage collection of afa stripe chunk
 * \param ctx the afa bdev
 */
static int
afa_sc_garbage_collection(void *ctx)
{
	struct afa_bdev *afa_bdev = (struct afa_bdev*)ctx;
	struct stripe_chunk_info *victim_sc;
	int level = SC_GC_LEVEL ? SC_GC_LEVEL : afa_bdev->info->num_data_base_bdevs;

	struct spdk_thread *t = spdk_get_thread();
	
	assert(t == afa_bdev->daemon);

	victim_sc = pqueue_peek(afa_bdev->victim_sc_pq);

	if(victim_sc == NULL || victim_sc->invalid_chunks < level) return SPDK_POLLER_IDLE;

	while(victim_sc->invalid_chunks >= level) {
		gc_free_stripe_chunk(afa_bdev, victim_sc->no);
		pqueue_pop(afa_bdev->victim_sc_pq);
		victim_sc = pqueue_peek(afa_bdev->victim_sc_pq);
	}
	
	return SPDK_POLLER_BUSY;
}


static void
switch_sc_complete_part(struct spdk_bdev_io *io, bool success, void *cb_arg)
{
    struct compute_parity_cb_arg *arg = cb_arg;
    struct afa_bdev *afa_bdev = arg->sc_io->afa_bdev;
    uint32_t sc_no = arg->sc_io->sc_no;
    uint8_t base_bdev_idx = arg->base_bdev_idx;

    handle_gc_flag(afa_bdev, io, base_bdev_idx);

    if(success) {
        merge_list_insert(&afa_bdev->base_bdevs[base_bdev_idx].free_pair_sc, arg->pair_sc_idx, arg->pair_sc_idx + 1);
        arg->sc_io->base_bdev_io_remaining--;
        if(arg->sc_io->base_bdev_io_remaining == 0)
        {
            afa_bdev->scis[sc_no].state = STRIPE;
            free(arg->sc_io);
        }
    }
    else assert(0);
    
    free(arg);
    spdk_bdev_free_io(io);
}


/**
 * switch one stripe chunk
 * \param sc the stripe chunk need swithed
 */
static void
sc_switch(struct afa_bdev *afa_bdev, struct sc_entry *sc) 
{
    struct afa_base_bdev    *base_bdev;
    struct spdk_io_channel  *base_ch;
    struct sc_io *sc_io =   calloc(1, sizeof(struct sc_io));
    struct compute_parity_cb_arg *cb_arg;
    uint64_t                src_lba, dst_lba;
    uint8_t                 m = afa_bdev->info->num_parity_base_bdevs;
    uint8_t                 base_bdev_idx;
    int i, ret;

    sc_io->afa_bdev = afa_bdev;
    sc_io->sc_no = sc->no;
    sc_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
    sc_io->base_bdev_io_remaining = m;

    for(i = 0; i < m; ++i) {
        base_bdev_idx = afa_bdev->scis[sc->no].parity_idxs[i];
        base_bdev = &afa_bdev->base_bdevs[base_bdev_idx];
        base_ch = afa_bdev->internal_channel[base_bdev_idx];
        src_lba = afa_bdev->scis[sc->no].pair_sc_offsets[i] * afa_bdev->info->num_data_base_bdevs * afa_bdev->info->chunk_size_block;
        dst_lba = sc->no * afa_bdev->info->chunk_size_block + TRANSIENT_AREA_OFFSET;

        cb_arg = calloc(1, sizeof(struct compute_parity_cb_arg));
        cb_arg->base_bdev_idx = base_bdev_idx;
        cb_arg->sc_io = sc_io;
        cb_arg->pair_sc_idx = src_lba / afa_bdev->info->chunk_size_block / afa_bdev->info->num_data_base_bdevs;

        ret = spdk_bdev_compute_parity(base_bdev->desc, base_ch, src_lba, afa_bdev->info->num_data_base_bdevs, dst_lba, 
                                afa_bdev->info->chunk_size_block, switch_sc_complete_part, cb_arg);
        if (ret == 0) sc_io->base_bdev_io_submitted++;
        else assert(0);
    }
}


/**
 * if a stripe chunks really can switch
 * \param afa_bdev - the afa bdev
 * \param sc_no - number of stripe chunk
 * \return true - can switch, false - cannot switch
 */
static inline bool 
can_switch(struct afa_bdev *afa_bdev, uint32_t sc_no) 
{
	uint8_t k = afa_bdev->info->num_data_base_bdevs;
	uint32_t i;

	if(afa_bdev->scis[sc_no].used_chunks != afa_bdev->info->num_data_base_bdevs) return false;
	for(i = 0; i < k; ++i) {
		if(afa_bdev->rcmt_bm[sc_no * k + i] != 1) return false;
	}

	return true;
}


/**
 * swith stripe chunks from DUPLICATE to STRIPE
 * \param ctx the afa bdev 
 */
static int
afa_phase_switching(void *ctx)
{
	struct afa_bdev *afa_bdev = (struct afa_bdev *)ctx;
	struct sc_entry *sc;
	struct spdk_thread *t = spdk_get_thread();
	
	assert(t == afa_bdev->daemon);

	if(TAILQ_EMPTY(&(afa_bdev->sc_need_switch)) || afa_bdev->num_free_sc > afa_bdev->info->num_stripe_chunks * SWITCH_BOUND) return SPDK_POLLER_IDLE;

	TAILQ_FOREACH(sc, &(afa_bdev->sc_need_switch), tailq) {
		if(can_switch(afa_bdev, sc->no)) { 
			sc_switch(afa_bdev, sc); 
			TAILQ_REMOVE(&(afa_bdev->sc_need_switch), sc, tailq);
			free(sc);
		}
	}
	
	return SPDK_POLLER_BUSY;
}


static void
base_bdev_gc_probe_complete(struct spdk_bdev_io *io, bool success, void *cb_arg)
{	
	struct gc_probe_io_cb_arg *arg = cb_arg;

	if(success) {
		handle_gc_flag(arg->afa_bdev, io, arg->base_bdev_idx);
	}
	else assert(0);
	
	free(arg);
	spdk_bdev_free_io(io);
}

/**
 * proactively detect if an afa base bdev is still gc. 
 */
static int
afa_base_bdev_gc_probe(void *ctx)
{
	struct afa_bdev *afa_bdev = (struct afa_bdev *)ctx;
	struct afa_base_bdev *base_bdev;
	struct spdk_io_channel 	*base_ch;
	struct spdk_nvme_cmd *cmd;
	struct gc_probe_io_cb_arg *arg;
	uint8_t i;

	struct spdk_thread *t = spdk_get_thread();
	
	assert(t == afa_bdev->daemon);

	if(afa_bdev->num_gcing_base_bdevs == 0) return SPDK_POLLER_IDLE;
	
	for(i = 0; i < afa_bdev->info->num_base_bdevs; ++i) {
		if(afa_bdev->base_bdevs[i].info->state == GC) {
			base_bdev = &afa_bdev->base_bdevs[i];
			base_ch = afa_bdev->internal_channel[i];
			cmd = calloc(1, sizeof(struct spdk_nvme_cmd));
			cmd->opc = SPDK_NVME_OPC_GC_PROBE;
			cmd->nsid = 0;
			arg = calloc(1, sizeof(struct gc_probe_io_cb_arg));
			arg->afa_bdev = afa_bdev;
			arg->base_bdev_idx = i;
			spdk_bdev_nvme_admin_passthru(base_bdev->desc, base_ch, cmd, NULL, 0, base_bdev_gc_probe_complete, arg);
		}
	}
 		

	return SPDK_POLLER_BUSY;
}

/* Log component for bdev afa bdev module */
SPDK_LOG_REGISTER_COMPONENT(bdev_afa)


