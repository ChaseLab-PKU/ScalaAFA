#ifndef SPDK_BDEV_AFA_H
#define SPDK_BDEV_AFA_H

#include "spdk/bdev_module.h"
#include "pqueue.h"

// in blocks (512 B), 8 G
#define TRANSIENT_AREA_OFFSET (1 << 24) 

#define MAX_FREE_STRIPE_CHUNK	100000

#define SWITCH_BOUND				0.2

#define MAX_NUM_BASE_BDEV		255

#define MAX_NUM_SC_ALLOC_ONCE_SHIFT 10
#define BASE_NUM_SC_ALLOC_ONCE_SHIFT 10
#define LOW_THRESHOLD_SUPP_SC (1 << 8)

#define MAX_NUM_SC_ALLOC_ONCE (1 << BASE_NUM_SC_ALLOC_ONCE_SHIFT)
#define BASE_NUM_SC_ALLOC_ONCE (1 << BASE_NUM_SC_ALLOC_ONCE_SHIFT)

#define NUM_PAIR_SC_ALLOC_ONCE (1 << 10)
#define LOW_THRESHOLD_SUPP_PAIR_SC (1 << 8)

#define SC_SWITCH_INTERVAL 1E+3
#define GC_PROBE_INTERVAL 1E+3
#define SC_GC_INTERVAL 1E+3
#define SC_GC_LEVEL 0

#define NUM_POWER_OF_TWO_CHOICE 4

TAILQ_HEAD(afa_tailq, afa_bdev);
extern struct afa_tailq		g_afa_bdev_head;

// [begin,end)
struct interval {
	uint32_t begin;
	uint32_t end;
	struct interval *next;
};

struct mlh {
	struct interval *first;
};

/* description of the state of base bdev. */
enum afa_base_bdev_state {
    NORMAL = 0, 
	GC,   
    FAILED,
    SPARE,  
};

/**
 * afa_base_bdev_config is the per base bdev data structure which contains config
 */
struct afa_base_bdev_info {
	/* base bdev name */
	char				*name;

    /* state of the base bdev */
    enum afa_base_bdev_state    state;
};

/**
 * afa_base_bdev contains information for the base bdevs which are part of some
 * afa. This structure contains the per base bdev information. Whatever is
 * required per base device for afa bdev will be kept here
 */
struct afa_base_bdev {
	/* pointer to the real base bdev */
	struct spdk_bdev	*bdev;

	/* pointer to the base bdev descriptor opened by afa bdev */
	struct spdk_bdev_desc   *desc;

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool    remove_scheduled;

	/* free pairing sc*/
	struct mlh					free_pair_sc;
	uint32_t					num_free_pair_sc;

	/* thread where the base device is opened */
	struct spdk_thread  *thread;

    /* pointer to info */
    struct afa_base_bdev_info *info;
};



/**
 * afa_bdev_config contains the afa bdev config
 */
struct afa_bdev_info {
	/* Points to already created afa bdev  */
	struct afa_bdev		*afa_bdev;

	char				*name;

	/* chunk size of this afa bdev in block */
	uint32_t			chunk_size_block;

    /* chunk size of afa bdev in KB */
	uint32_t			chunk_size_kb;

	/* number of chunks */
	uint32_t			num_chunks;

	/* number of stripe_chunks */
	uint32_t			num_stripe_chunks;

    /* number of base bdevs in used */
	uint8_t				num_base_bdevs;

	/* number of data base bdevs (i.e., k for k + m afa) */
	uint8_t				num_data_base_bdevs;

	/* number of parity base bdevs discovered (i.e., m for k + m afa) */
	uint8_t				num_parity_base_bdevs;

    /* number of spare base bdevs */
	uint8_t				num_spare_base_bdevs;

    /* number of failed base bdevs */
    uint8_t				num_failed_base_bdevs;
};

/* description of  the state of stripe chunk */
enum stripe_chunk_state {
	CLEAN = 0,
	DUPLICATE,    
    STRIPE, 
};

/* info of per stripe chunk */
struct stripe_chunk_info {
	enum stripe_chunk_state state;
	uint8_t					*bitmap;			// bitmap of used chunks
	uint8_t 				used_chunks;		// number of used chunks in this sc
	uint8_t 				invalid_chunks;		// number of invalid chunks in this sc
	uint8_t 				*parity_idxs;
	uint16_t				*pair_sc_offsets;
	uint32_t				no;
	size_t 					pos;				// used for gc pqueue
};

/* entry of stripe chunk number, used for sc switch tailq */
struct sc_entry {
	uint32_t	no;
	TAILQ_ENTRY(sc_entry) tailq;		
};


/**
 * afa_bdev is the single entity structure which contains SPDK block device
 * and the information related to any afa bdev.
 */
struct afa_bdev {
	/* afa bdev device, this will get registered in bdev layer */
	struct spdk_bdev        bdev;

	/* pointer to info */
	struct afa_bdev_info 	*info;

	/* array of base bdev */
	struct afa_base_bdev	*base_bdevs;

	/* chunk mapping table */
	uint32_t 				*cmt;

	/* reversed chunk mapping table */
	uint32_t				*rcmt;

	/* afa address bitmap */	
	uint8_t					*rcmt_bm;		// 0 -> clean, 1-> valid, 2 -> invalid

	/* stripe chunk infos */
	struct stripe_chunk_info	*scis;

	/* free stripe chunks (unallocated) */
	struct mlh					free_sc;
	uint32_t					num_free_sc;

	/* stripe chunks with some free chunks but not all (unallocated) */
	struct mlh					partial_sc;
	uint32_t					num_partial_sc;

	/* stripe chunks need swithed (有可能该sc中的请求未完成) */
	TAILQ_HEAD(, sc_entry)		sc_need_switch;

	/* victime stripe chunk to be GC */
	pqueue_t *victim_sc_pq;

	/* daemon thread for this afa */
	struct spdk_thread				*daemon;

	/* used by daemon */
	struct spdk_io_channel			**internal_channel;

	/* number of base bdevs that is in GC */
	uint8_t num_gcing_base_bdevs;

	/* Set to true if destruct is called for this afa bdev */
	bool					destruct_called;

    /* link to the link of all afa bdev */
	TAILQ_ENTRY(afa_bdev)	tailq;
};


#define AFA_FOR_EACH_BASE_BDEV(a, i) \
	for (i = a->base_bdevs; i < a->base_bdevs + a->info->num_base_bdevs; i++)


/* options of afa base bdev */
struct afa_base_bdev_opts{
	char	*name;
};


/* options to create afa bdev */
struct afa_bdev_opts {
	/* base bdev opts per underlying bdev */
	struct afa_base_bdev_opts	base_bdevs[MAX_NUM_BASE_BDEV];

	char				*name;

	/* strip unit size of this afa bdev in KB */
	uint32_t			chunk_size_kb;

	/* number of data base bdevs (i.e., k for k + m afa) */
	uint8_t				num_data_base_bdevs;

	/* number of parity base bdevs discovered (i.e., m for k + m afa) */
	uint8_t				num_parity_base_bdevs;

    /* number of spare base bdevs */
	uint8_t				num_spare_base_bdevs;
};


/*
 * afa_io_channel is the context of spdk_io_channel for afa bdev device. It
 * contains the relationship of afa bdev io channel with base bdev io channels.
 */
struct afa_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;

	/* Free stripe chunks can be used by this channel */
	struct mlh		free_sc;

	/* Partial stripe chunks can be used by this channel */
	struct mlh		partial_sc;
	
	/* Partial stripe chunks with low priority for base bdevs gc */
	struct mlh		partial_sc_lp;

	/* Number of free stripe chunks can be used by this channel */
	uint32_t		num_free_sc;

	/* Number of partial stripe chunks can be used by this channel */
	uint32_t		num_partial_sc;	

	/* Number of partial stripe chunks with low priority */
	uint32_t		num_partial_sc_lp;	

	struct mlh 		*free_pair_sc;
	uint32_t 		*num_free_pair_sc;

	/* Number of IO channels */
	uint8_t			num_channels;

	/* the bigger request factor the more sc will be allocated at once */
	uint8_t			ask_fac;
};


/**
 * afa_bdev_io is the context part of bdev_io. It contains the information
 * related to bdev_io for a afa bdev
 */
struct afa_bdev_io {
	/* The afa bdev associated with this IO */
	struct afa_bdev *afa_bdev;

	/* Context of the original channel for this IO */
	struct afa_io_channel	*afa_ch;

	/* Used for tracking progress on io requests sent to member disks. */
	uint8_t				base_bdev_io_status;
	uint32_t			base_bdev_io_remaining;
	uint32_t			base_bdev_io_submitted;
};


struct sc_io {
	struct afa_bdev 	*afa_bdev;
	
	uint8_t				base_bdev_io_status;
	uint32_t			base_bdev_io_remaining;
	uint32_t			base_bdev_io_submitted;

	uint32_t			sc_no;
};


struct afa_split_io {
	struct afa_bdev_io	*parent_io;
	struct iovec		*iovs;
	int					iovcnt;
	uint64_t			user_offset_block;
	uint64_t			num_blocks;
};

struct write_io_cb_arg {
	struct afa_bdev_io 	*afa_io;
	uint32_t 			user_chunk_no;
	uint32_t 			afa_chunk_no;
	uint8_t 			base_bdev_idx;
	bool				change_cmt;
};

struct read_io_cb_arg {
	struct afa_bdev_io 	*afa_io;
	uint8_t 			base_bdev_idx;
};

struct send_sc_arg {
	struct afa_bdev *afa_bdev;
	struct afa_io_channel *ch;
	struct mlh partial_sc;
	struct mlh free_sc;
	uint32_t num_partial;
	uint32_t num_free;
};

struct ask_pair_sc_arg {
	struct afa_io_channel *ch;
	uint8_t	base_bdev_idx;
};

struct send_pair_sc_arg {
	struct afa_bdev* afa_bdev;
	struct afa_io_channel *ch;
	struct mlh free_pair_sc;
	uint32_t num_free_pair_sc;
	uint8_t base_bdev_idx;
};

struct compute_parity_cb_arg {
	struct sc_io *sc_io;
	uint8_t  base_bdev_idx;
	uint32_t pair_sc_idx;
};

struct mw_arg {
	struct afa_split_io	*sio;
	char				*buf;
};

struct war_arg {
	struct afa_bdev		*afa_bdev;
	char				*buf;
	uint32_t			user_chunk_no;
};

struct gc_probe_io_cb_arg {
	uint8_t base_bdev_idx;
	struct afa_bdev *afa_bdev;
};

void afa_bdev_io_complete(struct afa_bdev_io *afa_io, enum spdk_bdev_io_status status);

int bdev_afa_create(struct spdk_bdev **bdev, struct afa_bdev_opts *opts);

typedef void (*spdk_delete_afa_complete)(void *cb_arg, int rc);
void bdev_afa_delete(const char *bdev_name, spdk_delete_afa_complete cb_fn, void *cb_arg);


// For merge list
void merge_list_insert(struct mlh *phead, uint32_t begin, uint32_t end);
uint32_t merge_list_get(struct mlh *phead, uint32_t num_want, struct mlh *res);

// For stripe chunk management
void afa_handle_ask_sc(void *ctx);
void afa_receive_sc(void *ctx);
void afa_handle_ask_pair_sc(void *ctx);
void afa_receive_pair_sc(void *ctx);

#endif /* SPDK_BDEV_AFA_H */