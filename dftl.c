// hjkim
// Input: Request’sLogicalPageNumber(requestlpn),Request’sSize (requestsize )
// Output: NULL
// while requestsize ̸= 0 do
// if requestlpn miss in Cached Mapping Table then if Cached Mapping Table is full then
// /* Select entry for eviction using segmented LRU replacement algorithm */
// victimlpn ← select victim entry()
// if victimlast mod time ̸= victimload time then
// /*victimtype : Translation or Data Block
// T ranslation P agevictim : Physical Translation-Page Number containing victim entry */ T ranslation P agevictim ← consult GTD (victimlpn )
// victimtype ← Translation Block
// DFTL Service Request(victim)
// end
// erase entry(victimlpn)
// end
// Translation Pagerequest ←
// consult GTD(requestlpn)
// /* Load map entry of the request from flash into Cached Mapping Table */
// load entry(Translation Pagerequest)
// end
// requesttype ← Data Block
// requestppn ← CMT lookup(requestlpn ) DFTL Service Request(request) requestsize- -
//end

//

// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "dftl.h"

struct pool_line *g_max_ec_in_hot_pool; //Hot Pool 에서 가장 삭제 많이 된 블록
struct pool_line *g_min_ec_in_hot_pool; //Hot Pool 에서 가장 삭제 덜 된 블록
struct pool_line *g_max_rec_in_hot_pool; //Hot Pool 에 이동한 후 가장 삭제 많이 된 블록
struct pool_line *g_min_rec_in_hot_pool; //Hot Pool 에 이동한 후 가장 삭제 덜 된 블록
struct pool_line *g_max_ec_in_cold_pool; //Cold pool 에서 가장 삭제 많이 된 블록
struct pool_line *g_min_ec_in_cold_pool; //Cold pool 에서 가장 삭제 덜 된 블록
struct pool_line *g_max_rec_in_cold_pool; //Cold Pool 에 이동한 후 가장 삭제 많이 된 블록
struct pool_line *g_min_rec_in_cold_pool; //Cold Pool 에 이동한 후 가장 삭제 덜 된 블록


//struct pool_mgmt pm;

struct pool_line {
	uint32_t id;
	uint32_t hot_cold_pool;
	uint32_t total_erase_cnt;
	uint32_t nr_recent_erase_cnt;
};

void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
				 struct buffer *write_buffer, unsigned int buffs_to_release);

static inline bool last_pg_in_wordline(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct dftl *dftl)
{
	return (dftl->lm.free_line_cnt <= dftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct dftl *dftl)
{
	return dftl->lm.free_line_cnt <= dftl->cp.gc_thres_lines_high;
}

static inline struct ppa get_maptbl_ent(struct dftl *dftl, uint64_t lpn)
{
	return dftl->ppa[lpn];
}

static inline void set_maptbl_ent(struct dftl *dftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < dftl->ssd->sp.tt_pgs);
	dftl->cmt[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__, ppa->g.ch,
			    ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct dftl *dftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(dftl, ppa);

	return dftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct dftl *dftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(dftl, ppa);

	dftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

static inline void consume_write_credit(struct dftl *dftl)
{
	dftl->wfc.write_credits--;
}

static void foreground_gc(struct dftl *dftl,uint64_t local_lpn);

static inline void check_and_refill_write_credit(struct dftl *dftl, uint64_t local_lpn)
{
	struct write_flow_control *wfc = &(dftl->wfc);
	if (wfc->write_credits <= 0) {
		foreground_gc(dftl, local_lpn);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct dftl *dftl)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct line_mgmt *lm = &dftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
	//init_global_wearleveling(dftl); // hjkim

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void remove_lines(struct dftl *dftl)
{
	pqueue_free(dftl->lm.victim_line_pq);
	vfree(dftl->lm.lines);
}

static void remove_plins(struct dftl *dftl)
{
	vfree(dftl->pm.lines);
}

static void init_write_flow_control(struct dftl *dftl)
{
	struct write_flow_control *wfc = &(dftl->wfc);
	struct ssdparams *spp = &dftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct dftl *dftl)
{
	struct line_mgmt *lm = &dftl->lm;
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}

static struct write_pointer *__get_wp(struct dftl *ftl, uint32_t io_type)
{
	if (io_type == USER_IO) {
		return &ftl->wp;
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}

static void prepare_write_pointer(struct dftl *dftl, uint32_t io_type)
{
	struct write_pointer *wp = __get_wp(dftl, io_type);
	struct line *curline = get_next_free_line(dftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
}

static void advance_write_pointer(struct dftl *dftl, uint32_t io_type, int dual_pool)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct line_mgmt *lm = &dftl->lm;
	struct write_pointer *wpp = __get_wp(dftl, io_type);

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", wpp->ch, wpp->lun,
			    wpp->pl, wpp->blk, wpp->pg);
	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		if (dual_pool == 0) {
			NVMEV_ASSERT(wpp->curline->ipc > 0);
		}
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(dftl);
	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			    wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct dftl *dftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp(dftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static void init_maptbl(struct dftl *dftl)
{
	int i;
	struct ssdparams *spp = &dftl->ssd->sp;

	int table_size = 512 * 1024 / sizeof(struct Node);
    struct CMT* cmt = createHashTable(table_size, 100);

	// dftl->cmt = vmalloc(sizeof(struct ppa) * spp->tt_cmt_entry);
	// //dftl->cmt = vmalloc(spp->tt_cmt_entry * 64);
	// //for (i = 0; i < spp->tt_cmt_entry; i++) {
	// for (i = 0; i < spp->tt_cmt_entry; i++) {	
	// 	dftl->cmt[i].ppa = UNMAPPED_PPA;
	// 	dftl->cmt[i].cached = -1;
	// 	dftl->cmt[i].dirty = -1;
	// 	dftl->gtd[i].Mppn = -1;
	// 	dftl->gtd[i].Mvpn = -1;
	// }
}

static void remove_maptbl(struct dftl *dftl)
{
	vfree(dftl->cmt);
}

static void init_rmap(struct dftl *dftl)
{
	int i;
	struct ssdparams *spp = &dftl->ssd->sp;

	dftl->r_cmt = vmalloc(sizeof(uint64_t) * spp->tt_cmt_entry);
	dftl->r_gtd = vmalloc(sizeof(uint32_t) * spp->tt_gtd_entry);
	//dftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_cmt_entry; i++) {
		dftl->r_cmt[i] = INVALID_LPN;
		//dftl->r_gtd[i] = INVALID_LPN;
	}

	for (i = 0; i < spp->tt_gtd_entry; i++) {
		dftl->r_gtd[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct dftl *dftl)
{
	vfree(dftl->rmap);
}

static void init_global_wearleveling(struct dftl *dftl)
{
	struct pool_mgmt *pm = &dftl->pm;
	struct ssdparams *spp = &dftl->ssd->sp;
	int i;
	int j;
	pm->tt_lines = spp->tt_lines;
	pm->lines = vmalloc(sizeof(struct pool_line) * (spp->tt_lines));
	//memset(pm.lines, 0, sizeof(struct pool_line) * (spp->tt_lines));
	//pm_tmp = (struct pool_mgmt *)vmalloc(sizeof(struct pool_mgmt));

	pm->GC_TH = 2;


	for (i = 0; i < (pm->tt_lines/2); i++) {
		pm->lines[i] = (struct pool_line){
			.id = i, .hot_cold_pool = 0, .total_erase_cnt=0, .nr_recent_erase_cnt = 0,
		};
	}

	g_max_ec_in_hot_pool = &pm->lines[0];
	g_max_rec_in_hot_pool = &pm->lines[1];
	g_min_ec_in_hot_pool = &pm->lines[2];
	g_min_rec_in_hot_pool = &pm->lines[3];

	for (j = (pm->tt_lines / 2); j < (pm->tt_lines); j++) {
		pm->lines[j] = (struct pool_line){
			.id = j, .hot_cold_pool = 1, .total_erase_cnt=0, .nr_recent_erase_cnt = 0,
		};
	}

	g_max_ec_in_cold_pool = &pm->lines[j-1];
	g_max_rec_in_cold_pool = &pm->lines[j-2];
	g_min_ec_in_cold_pool = &pm->lines[j-3];
	g_min_rec_in_cold_pool = &pm->lines[j-4];

}

static void dftl_init_ftl(struct dftl *dftl, struct dftlparams *cpp, struct ssd *ssd)
{
	/*copy dftlparams*/
	dftl->cp = *cpp;
	dftl->ssd = ssd;
	dftl->c_cmt_tt = 0;
	dftl->c_gtd_tt = 0;

	/* initialize maptbl */
	init_maptbl(dftl); // mapping table

	/* initialize rmap */
	//init_rmap(dftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_lines(dftl);

	/*init global wearleveling*/
	/* no need for dftl */
	//init_global_wearleveling(dftl);
 
	/* initialize write pointer, this is how we allocate new pages for writes */
	prepare_write_pointer(dftl, USER_IO);
	prepare_write_pointer(dftl, GC_IO);

	init_write_flow_control(dftl);

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", dftl->ssd->sp.nchs,
		   dftl->ssd->sp.tt_pgs);

	return;
}

static void dftl_remove_ftl(struct dftl *dftl)
{	
	int i;
	struct pool_mgmt *pm = &dftl->pm;
	for (i=0;i<pm->tt_lines;i++) {
		if (pm->lines[i].total_erase_cnt !=0) {
			printk(KERN_INFO "block %d %d\n", i , pm->lines[i].total_erase_cnt);
		}
	}
	remove_lines(dftl);
	remove_rmap(dftl);
	remove_maptbl(dftl);
	remove_plins(dftl);
}

static void dftl_init_params(struct dftlparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
}



static bool check_cold_data_migration(struct dftl *dftl)
{
	struct pool_mgmt *pm = &dftl->pm;
//	printk(KERN_INFO "1st %d %d\n", g_max_ec_in_hot_pool->total_erase_cnt, g_min_ec_in_cold_pool->total_erase_cnt);
	if ((g_max_ec_in_hot_pool->total_erase_cnt - g_min_ec_in_cold_pool->total_erase_cnt) > pm->GC_TH) {
		return true;
	} else {
		return false;
	}
}

static bool check_hot_pool_adjustment(struct dftl *dftl)
{
	struct pool_mgmt *pm = &dftl->pm;
//	printk(KERN_INFO "2nd %d %d\n", g_max_ec_in_hot_pool->total_erase_cnt,g_min_ec_in_hot_pool->total_erase_cnt);
	if ((g_max_ec_in_hot_pool->total_erase_cnt - g_min_ec_in_hot_pool->total_erase_cnt) > (2 * (pm->GC_TH))) {
		return true;
	} else {
		return false;
	}
}

static bool check_cold_pool_adjustment(struct dftl *dftl)
{
	struct pool_mgmt *pm = &dftl->pm;
//	printk(KERN_INFO "3rd %d %d\n", g_max_rec_in_cold_pool->nr_recent_erase_cnt,g_min_rec_in_hot_pool->nr_recent_erase_cnt);
	if ((g_max_rec_in_cold_pool->nr_recent_erase_cnt - g_min_rec_in_hot_pool->nr_recent_erase_cnt) > pm->GC_TH) {
		return true;
	} else {
		return false;
	}
}

static void dual_pool_copyback(struct dftl *dftl, struct ppa ppa , struct ppa *ppa_old , int dual_pool_copy);
static void dual_pool_clean_one_flashpg(struct dftl *dftl, struct ppa *ppa, struct ppa *ppa_old ,int dual_pool_copy);
static uint64_t dual_pool_gc_write_page(struct dftl *dftl, struct ppa *old_ppa, struct ppa *ppa_dual, int dual_pool_copy);
static void inc_ers_cnt(struct ppa *ppa, struct dftl *dftl);
static void mark_line_free(struct dftl *dftl, struct ppa *ppa);
static void mark_block_free(struct dftl *dftl, struct ppa *ppa);
static inline bool valid_lpn(struct dftl *dftl, uint64_t lpn);
static void mark_page_valid(struct dftl *dftl, struct ppa *ppa, int dual_pool);
static void check_min_max_pool(struct dftl *dftl);
static void clean_one_flashpg(struct dftl *dftl, struct ppa *ppa);
static inline bool mapped_ppa(struct ppa *ppa);
static void mark_page_invalid(struct dftl *dftl, struct ppa *ppa, int dual_pool);
static void advance_write_pointer(struct dftl *dftl, uint32_t io_type, int dual_pool);

/* 
1. HOT POOL 에서 가장 삭제가 많이 된 블록(oldest block)의 valid data migration
2. oldest block erase
3. Cold pool 에서 가장 삭제가 덜 된 블록에서(youngest block) valid한 data 를 oldest block으로 복사
4. youngest block 삭제
5. oldest block & youngest block pool 변경 & oldest block EEC reset
6. pool status update
*/
static void cold_pool_migration(struct dftl *dftl, struct ppa *ppa_mig ,uint64_t local_lpn)
{
	struct line_mgmt *lm = &dftl->lm;
	struct pool_mgmt *pm = &dftl->pm;
	struct ssdparams *spp = &dftl->ssd->sp;
	struct line *victim_line = NULL;
	struct ppa ppa;
	int i;

	/* need to advance the write pointer here */
	advance_write_pointer(dftl, GC_IO, 1);

	// ppa = get_maptbl_ent(dftl, local_lpn);	
	// if (mapped_ppa(&ppa)) {
	// 		/* update old page information first */
	// 		mark_page_invalid(dftl, &ppa,1);
	// 		set_rmap_ent(dftl, INVALID_LPN, &ppa);
	// 		NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(dftl, &ppa));
	// }

	//victim_line = &lm->lines[g_max_ec_in_hot_pool->id];
	//ppa.g.blk = victim_line->id;
	//dftl->wfc.credits_to_refill = victim_line->ipc;

	//printk(KERN_INFO "victim_line : %d\n",victim_line->id);

	// /* 1. hot pool valid data migration */
	//dual_pool_copyback(dftl,ppa,ppa,0);

	// /* 2. oldest block erase */
	//mark_line_free(dftl, &ppa);
	//inc_ers_cnt(&ppa, dftl);
	victim_line = &lm->lines[g_min_ec_in_cold_pool->id];
	//pqueue_change_priority(lm->victim_line_pq, spp->pgs_per_line , victim_line);
	pqueue_remove(lm->victim_line_pq,victim_line);
	//victim_line = select_victim_line(dftl, 0);
	//pqueue_pop(lm->victim_line_pq);
	//victim_line->pos = 0;
	lm->victim_line_cnt--;

	ppa.g.blk = victim_line->id;
	dftl->wfc.credits_to_refill = victim_line->ipc;

	/* 3. cold pool youngest block valid data -> hot pool oldest block (erased) */
	if (g_min_ec_in_cold_pool->total_erase_cnt > 0 ) {
		dual_pool_copyback(dftl,ppa,ppa_mig,1);
	/* 4. youngest block erase */
		pqueue_insert(lm->victim_line_pq, victim_line);
		mark_line_free(dftl, &ppa);
		inc_ers_cnt(&ppa, dftl);
	}
	
	// /* 5. oldest block & yougesst block pool switch */
	g_max_ec_in_hot_pool->hot_cold_pool = 1;
	g_min_ec_in_cold_pool->hot_cold_pool = 0;
	g_max_ec_in_hot_pool->nr_recent_erase_cnt = 0;
	
	/* 6. pool state update */
	check_min_max_pool(dftl);

}

static void dual_pool_copyback(struct dftl *dftl, struct ppa ppa , struct ppa *ppa_mig , int dual_pool_copy) {
	/* copy back valid data */
	int flashpg;
	struct ssdparams *spp = &dftl->ssd->sp;

	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;
				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(dftl->ssd, &ppa);
				dual_pool_clean_one_flashpg(dftl, &ppa, ppa_mig, dual_pool_copy);
				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct dftlparams *cpp = &dftl->cp;
					
					mark_block_free(dftl, &ppa);
					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(dftl->ssd, &gce);
					}
					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}
}

/* here ppa identifies the block we want to clean */
static void dual_pool_clean_one_flashpg(struct dftl *dftl, struct ppa *ppa, struct ppa *ppa_mig ,int dual_pool_copy)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *cpp = &dftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(dftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		//if (dual_pool_copy == 0) NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;
		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(dftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(dftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			dual_pool_gc_write_page(dftl, &ppa_copy, ppa_mig ,dual_pool_copy);
		}

		ppa_copy.g.pg++;
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t dual_pool_gc_write_page(struct dftl *dftl, struct ppa *old_ppa, struct ppa *ppa_mig, int dual_pool_copy)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *cpp = &dftl->cp;
	struct ppa new_ppa;
	struct line_mgmt *lm = &dftl->lm;
	struct pool_mgmt *pm = &dftl->pm;
	struct line *mig_line = NULL;
	uint64_t lpn = get_rmap_ent(dftl, old_ppa);
	struct write_pointer *wp = __get_wp(dftl, 1);

	mig_line =  &lm->lines[g_max_ec_in_hot_pool->id];

	NVMEV_ASSERT(valid_lpn(dftl, lpn));
	if (dual_pool_copy == 0) { 
		new_ppa = get_new_page(dftl, GC_IO);
	} else {
		printk(KERN_INFO "**hj* else?\n");
		new_ppa.g.blk = mig_line->id;
		new_ppa.ppa = 0;
		new_ppa.g.ch = 0;
		new_ppa.g.lun = 0;
		new_ppa.g.pg = 0;
		new_ppa.g.pl = 0;	
	}

	/* update maptbl */
	set_maptbl_ent(dftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(dftl, lpn, &new_ppa);

	mark_page_valid(dftl, &new_ppa, 1);

	/* need to advance the write pointer here */
	advance_write_pointer(dftl, GC_IO, 1);

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(dftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(dftl->ssd, &gcw);
	}

	return 0;
}


static void inc_ers_cnt(struct ppa *ppa,struct dftl *dftl) {
	struct pool_mgmt *pm = &dftl->pm;
	pm->lines[ppa->g.blk].total_erase_cnt++;
	pm->lines[ppa->g.blk].nr_recent_erase_cnt++;
}

/* 
1. Cold pool 내에서 가장 큰 EEC를 갖는 블록을 Hot Pool로 이동
2. Hot pool로 이동후 Cold pool과 Hot pool의 상태를 갱신 (Cold pool에서 oldest block 이었다던지..)
 */

static void cold_pool_adjustment(struct dftl *dftl)
{
	struct pool_mgmt *pm = &dftl->pm;
	g_max_rec_in_cold_pool->hot_cold_pool = 0;
	//printk(KERN_INFO "*hj_max* %d",g_max_rec_in_cold_pool->nr_recent_erase_cnt);
	check_min_max_pool(dftl);
}

/* 
1. Hot pool 에서 가장 삭제 수가 작은 블록을 Cold pool 로 이동
2. 이동후 Cold pool과 Hot pool의 상태를 갱신
 */
static void hot_pool_adjustment(struct dftl *dftl)
{
	struct pool_mgmt *pm = &dftl->pm;
	g_min_ec_in_hot_pool->hot_cold_pool = 1;
	check_min_max_pool(dftl);
}

static void check_min_max_pool(struct dftl *dftl)
{
	struct pool_mgmt *pm = &dftl->pm;
	int i;
	int hot_min_ec = INT_MAX;
	int hot_max_ec = 0;
	int hot_min_rec = INT_MAX;
	int hot_max_rec = 0;

	int cold_min_ec = INT_MAX;
	int cold_max_ec = 0;
	int cold_min_rec = INT_MAX;
	int cold_max_rec = 0;

	//printk(KERN_INFO "*hj_max* %d %d %d %d\n",g_max_ec_in_cold_pool->total_erase_cnt,cold_pool->total_erase_cnt,g_max_rec_in_cold_pool->nr_recent_erase_cnt,g_max_rec_in_hot_pool->nr_recent_erase_cnt);
	//printk(KERN_INFO "*hj_min* %d %d %d %d\n",g_min_ec_in_cold_pool->total_erase_cnt,g_min_ec_in_hot_pool->total_erase_cnt,g_min_rec_in_cold_pool->nr_recent_erase_cnt,g_min_rec_in_hot_pool->nr_recent_erase_cnt);
	

	for (i = 0; i < pm->tt_lines; i++) {
		if(pm->lines[i].hot_cold_pool == 0) {
			//if (pm->lines[i].total_erase_cnt > 0) {
			//	printk(KERN_INFO "hot?? %d %d", i , pm->lines[i].total_erase_cnt);
			//}
			// min ec in hot pool
			if(hot_min_ec > pm->lines[i].total_erase_cnt) {
				g_min_ec_in_hot_pool = &pm->lines[i];
				hot_min_ec = pm->lines[i].total_erase_cnt;
			}
			// max ec in hot pool
			if(pm->lines[i].total_erase_cnt > hot_max_ec) {
				g_max_ec_in_hot_pool = &pm->lines[i];
				hot_max_ec = pm->lines[i].total_erase_cnt;
				//printk(KERN_INFO "hot_ec?? %d\n", i);
			}
			// min rec in hot pool
			if(hot_min_rec > pm->lines[i].nr_recent_erase_cnt) {
				g_min_rec_in_hot_pool = &pm->lines[i];
				hot_min_rec = pm->lines[i].nr_recent_erase_cnt;
			}
			// max rec in hot pool
			if(pm->lines[i].nr_recent_erase_cnt > hot_max_rec) {
				g_max_rec_in_hot_pool = &pm->lines[i];
				hot_max_rec = pm->lines[i].nr_recent_erase_cnt;
				//printk(KERN_INFO "hot_rec?? %d\n", g_max_rec_in_hot_pool->nr_recent_erase_cnt);
			}
		} else {
			// min ec in cold pool
			if(cold_min_ec > pm->lines[i].total_erase_cnt) {
				g_min_ec_in_cold_pool = &pm->lines[i];
				cold_min_ec = pm->lines[i].total_erase_cnt;
			}
			// max ec in cold pool
			if(pm->lines[i].total_erase_cnt > cold_max_ec) {
				g_max_ec_in_cold_pool = &pm->lines[i];
				cold_max_ec = pm->lines[i].total_erase_cnt;
			}
			// min rec in cold pool
			if(cold_min_rec > pm->lines[i].nr_recent_erase_cnt) {
				g_min_rec_in_cold_pool = &pm->lines[i];
				cold_min_rec = pm->lines[i].nr_recent_erase_cnt;
			}
			// max rec in cold pool
			if(pm->lines[i].nr_recent_erase_cnt > cold_max_rec) {
				g_max_rec_in_cold_pool = &pm->lines[i];
				cold_max_rec = pm->lines[i].nr_recent_erase_cnt;
			}
		}
	}
}

void dftl_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct dftlparams cpp;
	struct dftl *dftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	dftl_init_params(&cpp);

	dftls = kmalloc(sizeof(struct dftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		dftl_init_ftl(&dftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(dftls[i].ssd->pcie->perf_model);
		kfree(dftls[i].ssd->pcie);
		kfree(dftls[i].ssd->write_buffer);

		dftls[i].ssd->pcie = dftls[0].ssd->pcie;
		dftls[i].ssd->write_buffer = dftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)dftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = dftl_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}

void dftl_remove_namespace(struct nvmev_ns *ns)
{
	struct dftl *dftls = (struct dftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from dftl_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		dftls[i].ssd->pcie = NULL;
		dftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		dftl_remove_ftl(&dftls[i]);
		ssd_remove(dftls[i].ssd);
		kfree(dftls[i].ssd);
	}

	kfree(dftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool valid_lpn(struct dftl *dftl, uint64_t lpn)
{
	return (lpn < dftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct dftl *dftl, struct ppa *ppa)
{
	return &(dftl->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct dftl *dftl, struct ppa *ppa, int dual_pool)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct line_mgmt *lm = &dftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(dftl->ssd, ppa);
	if (dual_pool == 0) {
		NVMEV_ASSERT(pg->status == PG_VALID);
	}
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(dftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(dftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
}

static void mark_page_valid(struct dftl *dftl, struct ppa *ppa, int dual_pool)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(dftl->ssd, ppa);
	if (dual_pool == 0 ) NVMEV_ASSERT(pg->status == PG_FREE);	//ERROR POINT..//
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(dftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(dftl, ppa);
	if (dual_pool == 0 ) NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct nand_block *blk = get_blk(dftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *cpp = &dftl->cp;
	/* advance dftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(dftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct dftl *dftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *cpp = &dftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(dftl, old_ppa);

	NVMEV_ASSERT(valid_lpn(dftl, lpn));
	new_ppa = get_new_page(dftl, GC_IO);

	/* update maptbl */
	set_maptbl_ent(dftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(dftl, lpn, &new_ppa);

	mark_page_valid(dftl, &new_ppa, 0);

	/* need to advance the write pointer here */
	advance_write_pointer(dftl, GC_IO, 0);

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(dftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(dftl->ssd, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(dftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(dftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}

static struct line *select_victim_line(struct dftl *dftl, bool force)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct line_mgmt *lm = &dftl->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(dftl->ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(dftl, ppa);
			/* delay the maptbl update until "write" happens */
			gc_write_page(dftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(dftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct dftl *dftl, struct ppa *ppa)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	struct dftlparams *cpp = &dftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(dftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;

		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(dftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(dftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			gc_write_page(dftl, &ppa_copy);
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct dftl *dftl, struct ppa *ppa)
{
	struct line_mgmt *lm = &dftl->lm;
	struct line *line = get_line(dftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct dftl *dftl, bool force, uint64_t local_lpn)
{
	struct line *victim_line = NULL;
	struct pool_mgmt *pm = &dftl->pm;
	struct ssdparams *spp = &dftl->ssd->sp;
	struct ppa ppa;
	int flashpg;

	victim_line = select_victim_line(dftl, force);
	if (!victim_line) {
		return -1;
	}

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
			    victim_line->ipc, victim_line->vpc, dftl->lm.victim_line_cnt,
			    dftl->lm.full_line_cnt, dftl->lm.free_line_cnt);

	dftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(dftl->ssd, &ppa);
				clean_one_flashpg(dftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct dftlparams *cpp = &dftl->cp;

					mark_block_free(dftl, &ppa);

					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(dftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	mark_line_free(dftl, &ppa);
	
	inc_ers_cnt(&ppa,dftl);

	// /*여기다가 dual pool wear leveling call*/
	// if ((g_max_ec_in_cold_pool->total_erase_cnt< pm->GC_TH) || (g_max_ec_in_hot_pool->total_erase_cnt< pm->GC_TH)) {
	// 	check_min_max_pool(dftl);
	// }

	//  if (check_cold_data_migration(dftl)) {
	// 	//printk(KERN_INFO "?1111cold_data_migration %d %d \n",g_max_ec_in_hot_pool->total_erase_cnt,g_min_ec_in_cold_pool->total_erase_cnt);
	//  	cold_pool_migration(dftl, &ppa ,local_lpn);
	//  }
	//  if (check_cold_pool_adjustment(dftl)) {
	// //	printk(KERN_INFO "?cold?? %d %d %d %d\n", g_max_ec_in_cold_pool->total_erase_cnt,g_min_ec_in_cold_pool->total_erase_cnt,g_max_rec_in_cold_pool->nr_recent_erase_cnt,g_min_rec_in_cold_pool->nr_recent_erase_cnt);
	// //	printk(KERN_INFO "?hot?? %d %d %d %d\n", g_max_ec_in_hot_pool->total_erase_cnt,g_min_ec_in_hot_pool->total_erase_cnt,g_max_rec_in_hot_pool->nr_recent_erase_cnt,g_min_rec_in_hot_pool->nr_recent_erase_cnt);
	//  	cold_pool_adjustment(dftl);
	//  }
	//  if (check_hot_pool_adjustment(dftl)) {
	// //	printk(KERN_INFO "?33333hot_pool_adjustment\n");
	//  	hot_pool_adjustment(dftl);
	//  }

	return 0;
}

static void foreground_gc(struct dftl *dftl,uint64_t local_lpn)
{
	if (should_gc_high(dftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		/* perform GC here until !should_gc(dftl) */
		do_gc(dftl, 0, local_lpn);
	}
}

static bool is_same_flash_page(struct dftl *dftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &dftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

// 해시 테이블 생성
struct CMT* createHashTable(int table_size, int capacity) {
    struct CMT* ht = (struct CMT*)vmalloc(sizeof(struct CMT));
    ht->table = (struct Node*)vmalloc(table_size * sizeof(struct Node));
    ht->size = table_size;
    ht->capacity = capacity;
    ht->current_size = 0;
    ht->current_time = 0;
    return ht;
}

// 새로운 노드 생성
struct Node* createNode(const char* key, struct Dppn value, int dirty, int time) {
    struct Node* newNode = (struct Node*)malloc(sizeof(struct Node));
    newNode->key = strdup(key);
    newNode->value = value;
    newNode->dirty = dirty;
    newNode->time = time;
    newNode->next = NULL;
    return newNode;
}

// // 해시 테이블에서 값 찾기
// struct Node* search(struct CMT* cmt, const char* key) {
//     unsigned int index = hash(key, cmt->size);
//     struct Node* currentNode = cmt->table[index].next;

//     // 연결 리스트를 순회하며 키에 해당하는 값을 찾음
//     while (currentNode != NULL) {
//         if (strcmp(currentNode->key, key) == 0) {
//             return currentNode;
//         }
//         currentNode = currentNode->next;
//     }

//     // 키를 찾지 못한 경우 NULL 반환
//     return NULL;
// }

// 가장 오래된 엔트리를 삭제
void evictOldestEntry(struct CMT* cmt) {
    int oldest_time = cmt->current_time;
    struct Node* oldest_node = NULL;
    struct Node* prev_node = NULL;
    struct Node* current_node = NULL;

    // 모든 슬롯을 순회하며 가장 오래된 엔트리를 찾음
    for (int i = 0; i < cmt->size; ++i) {
        prev_node = &(cmt->table[i]);
        current_node = prev_node->next;
        
        // 연결 리스트를 순회하며 가장 오래된 엔트리를 찾음
        while (current_node != NULL) {
            if (current_node->time < oldest_time) {
                oldest_time = current_node->time;
                oldest_node = current_node;
                prev_node = prev_node->next;
            }
            current_node = current_node->next;
        }
    }

    // 가장 오래된 엔트리 삭제 + 만약 dirty가 1이면 업데이트 하고 나머지 block들도 copyback 해야됨..
    if (oldest_node != NULL) {
		if (oldest_node->dirty == 1) {
			
			int pmt_index = Dlpn/512;
	//struct PMTEntry pmt_entry;
	//pmt_entry.ch = 
    GTD[Dlpn] = pmt_entry;
			insertOrUpdateGTD
		}
        prev_node->next = oldest_node->next;
        free(oldest_node->key);
        free(oldest_node);
        cmt->current_size--;
    }
}

// 해시 테이블에 값 삽입 또는 업데이트 (LRU 캐시 교체 정책 포함)
void insertOrUpdate(struct CMT* cmt, const char* key, struct Dppn value) {
    unsigned int index = hash(key, cmt->size);

    // 이미 존재하는 키인 경우 업데이트 수행
    struct Node* existingNode = search(cmt, key);
    if (existingNode != NULL) {
        existingNode->value = value;
        existingNode->dirty = 1;
        existingNode->time = cmt->current_time++;
        return;
    }

    // 용량이 다 찼을 경우 가장 오래된 엔트리 삭제
    if (cmt->current_size >= cmt->capacity) {
        evictOldestEntry(cmt);
    }

    // 새로운 노드를 생성하여 삽입
    struct Node* newNode = createNode(key, value, 1, cmt->current_time++);
    newNode->next = cmt->table[index].next;
    cmt->table[index].next = newNode;
    cmt->current_size++;
}


static struct ppa find_oldest_cmt(struct dftl *dftl) {
	int i, oldest_num;
	struct ssdparams *spp = &dftl->ssd->sp;
	oldest_num = -1;
	
	for (i=0; i<spp->tt_cmt_entry; i++) {
		if (dftl->cmt[i].modi_time == -1) {
			continue;
		}
		if (oldest_num == -1) {
			oldest_num = i;
		} else {
			if (dftl->cmt[i].modi_time < dftl->cmt[oldest_num].modi_time) {
				oldest_num = i;
			}
		}
	}

	return oldest_num;
}

static uint32_t gtd_trace(struct dftl *dftl, uint64_t local_lpn ) {
	uint32_t gtd_index, gtd_offset, gtd_ppn, bank;
	struct ppa ppa;
	
	gtd_index = local_lpn / (KB(32)/sizeof(uint32_t));
	gtd_offset = local_lpn % (KB(32)/sizeof(uint32_t));
	gtd_vpn = dftl->gtd[gtd_index];
	bank = 


	mark_page_invalid(dftl,&,0);
	
}


static bool dftl_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct dftl *dftls = (struct dftl *)ns->ftls;
	struct dftl *dftl = &dftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &dftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	int oldest_cmt;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i, j;
	uint32_t nr_parts = ns->nr_parts;
	char* Dlpn;
	struct CMT *cmt;
	struct Dppn new_ppa;
	struct Node* target_node;
	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	//NVMEV_ASSERT(dftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn,
			    nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		dftl = &dftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(dftl, start_lpn / nr_parts);

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			Dlpn = local_lpn;
			cur_ppa = get_maptbl_ent(dftl, local_lpn);
			target_node = lookup_CMT(dftl, Dlpn);
			if (target_node == NULL) {
				new_ppa.blk = cur_ppa.g.blk;
				new_ppa.ch = cur_ppa.g.ch;
				new_ppa.lun = cur_ppa.g.lun;
				new_ppa.pg = cur_ppa.g.pg;
				new_ppa.pl = cur_ppa.g.pl;
				insertOrUpdate(cmt, Dlpn, new_ppa);
			} else {
				cur_ppa.g.blk = target_node->value.blk;
				cur_ppa.g.ch = target_node->value.ch;
				cur_ppa.g.lun = target_node->value.lun;
				cur_ppa.g.pg = target_node->value.pg;
				cur_ppa.g.pl = target_node->value.pl;
				update_cmt(dftl,Dlpn,ppa,0);
			}
			
			//cur_ppa.g.blk = get_maptbl_ent(dftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(dftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n",
							local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
							cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
							cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
				is_same_flash_page(dftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(dftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		
		}

		// issue remaining io
		if (xfer_size > 0) {
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa;
			nsecs_completed = ssd_advance_nand(dftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest);
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}


lpn -> ppn

// static int lookup_CMT(struct dftl *dftl ,uint64_t local_lpn) {	
// 	int i;
// 	struct ssdparams *spp = &dftl->ssd->sp;

// 	for (i=0; i< spp->tt_cmt_entry; i++) {
// 		if (local_lpn == dftl->cmt[i].lpn)
// 			return i;
// 	}
//     return -1;
// }

// static void update_modi_time(struct dftl *dftl ,int lpn_index) {
// 	dftl->cmt[lpn_index].modi_time = local_clock();
// }

// CMT에서 Dlpn을 찾는 함수
struct Node* findDlpn(struct CMT* cmt, const char* key) {
    unsigned int index = hash(key);
    struct Node* currentNode = cmt->table[index];

    // 연결 리스트를 순회하며 해당 Dlpn을 찾음
    while (currentNode != NULL) {
        if (strcmp(currentNode->key, key) == 0) {
            return currentNode; // Dlpn을 찾은 경우
        }
        currentNode = currentNode->next;
    }

    return NULL; // Dlpn을 찾지 못한 경우
}

// DFTL 내부의 CMT에서 Dlpn을 찾는 함수
struct Node* lookup_CMT(struct dftl* dftl, const char* key) {
    return findDlpn(&(dftl->cmt), key);
}

void update_cmt_ppn(struct dftl *dftl,const char* key,struct ppa ppa,int dirty) {
    unsigned int index = hash(key);
	//struct CMT* cmt = dftl->cmt;
    struct Node* currentNode = dftl->cmt->table[index];

	while (currentNode != NULL) {
        if (strcmp(currentNode->key, key) == 0) {
			if (dirty == 1) {
				currentNode->dirty = 1;
			}
			currentNode->time = dftl->cmt->current_time++;
			currentNode->value.blk = ppa.g.blk;
			currentNode->value.ch = ppa.g.ch;
			currentNode->value.lun = ppa.g.lun;
			currentNode->value.pg = ppa.g.pg;
			currentNode->value.pl = ppa.g.pl;
        }
        currentNode = currentNode->next;
    }
}


static bool dftl_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct dftl *dftls = (struct dftl *)ns->ftls;
	struct dftl *dftl = &dftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &dftl->ssd->sp;
	struct buffer *wbuf = dftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;
	uint32_t check_cmt;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;
	int 	lpn_index;
	char* Dlpn;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	nsecs_latest =
		ssd_advance_write_buffer(dftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		dftl = &dftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		Dlpn = local_lpn;

		ppa = get_maptbl_ent(dftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(dftl, &ppa,0);
			set_rmap_ent(dftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(dftl, &ppa));
		}

		/* new write */
		ppa = get_new_page(dftl, USER_IO);
		/* update maptbl */
		set_maptbl_ent(dftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(dftl, &ppa));
		/* update rmap */
		set_rmap_ent(dftl, local_lpn, &ppa);

		//lpn_index = lookup_CMT(dftl, Dlpn);

		if (lookup_CMT(dftl, Dlpn) != NULL) {
			update_cmt(dftl,Dlpn,ppa,1);
		}

		mark_page_valid(dftl, &ppa,0);

		/* need to advance the write pointer here */
		advance_write_pointer(dftl, USER_IO, 0);

		/* Aggregate write io in flash page */
		if (last_pg_in_wordline(dftl, &ppa)) {
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(dftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		consume_write_credit(dftl);
		check_and_refill_write_credit(dftl,local_lpn);
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}

static void dftl_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct dftl *dftls = (struct dftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(dftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool dftl_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!dftl_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!dftl_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		dftl_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
