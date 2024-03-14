// hjkim


#ifndef _NVMEVIRT_DFTL_H
#define _NVMEVIRT_DFTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"


struct dftlparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100*/
};

struct line {
	int id; /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	struct list_head entry;
	/* position in the priority queue for victim lines */
	size_t pos;
};


/* wp: record next write addr */
struct write_pointer {
	struct line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
};

struct line_mgmt {
	struct line *lines;

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list;
	pqueue_t *victim_line_pq;
	struct list_head full_line_list;

	uint32_t tt_lines;
	uint32_t free_line_cnt;
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;
};

struct write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};

// hjkim cmt , gtd //

// Dppn 구조체 정의
struct Dppn {
    int pg;
    int blk;
    int pl;
    int lun;
    int ch;
};

// 해시 테이블 항목을 나타내는 구조체
struct Node {
    int key; // Dlpn
    struct Dppn value; // Dppn
    int dirty; // dirty flag
    int time; // 시간 정보
    struct Node* next;
};

// 해시 테이블 구조체
struct CMT {
    struct Node* table;
	struct Node** cmt_bucket;
    int size; // 전체 사이즈 (바이트 단위)
    int capacity; // 용량 (노드 수)
    int current_size; // 현재 크기 (삽입된 노드 수)
    int current_time; // 현재 시간
};

// 해시 함수
unsigned int hash(const char* key, int table_size);


// GTD를 만들어보자..
#define GTD_SIZE 1024

// PMT 페이지 번호(pmt_vpn)를 나타내는 구조체
struct PMTEntry {
//    int pg;
    //int blk;
	int vpn;
    int pl;
    int lun;
    int ch;
};


// hjkim end //


struct dftl {
	struct ssd *ssd;
	struct dftlparams cp;
	struct CMT *cmt;
	struct ppa *maptbl;
	struct PMTEntry *GTD; /* global translation directory */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */
	//uint64_t *r_cmt; /* reverse cmt */
	//uint32_t *r_gtd; /* reverse gtdtbl */
	struct write_pointer wp;
	struct write_pointer gc_wp;
	struct line_mgmt lm;
	struct write_flow_control wfc;
	uint64_t c_cmt_tt;
	uint32_t c_gtd_tt;
};


void dftl_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void dftl_remove_namespace(struct nvmev_ns *ns);

bool dftl_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif
