
/*
 * dftp_ftl.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <limits.h>

#define NUMBER_OF_ADDRESSABLE_BLOCKS 1024
#define BLOCK_SIZE 1024
#define PAGE_SIZE 512
#define CACHE_DFTL_LIMIT 64

struct Controller {
    int dummy; // Dummy structure for placeholder
};

struct Event {
    int type;
    long logical_address;
    int size;
    double start_time;
};

struct BlockManager {
    int dummy; // Dummy structure for placeholder
};

struct FtlImpl_DftlParent {
    struct MPage {
        long vpn;
        long ppn;
        double create_ts;
        double modified_ts;
        double last_visited_time;
        int cached;
    };

    struct Controller *controller;
    struct BlockManager *block_manager;
    int addressPerPage;
    int currentDataPage;
    int currentTranslationPage;
    int addressSize;
    int totalCMTentries;
    struct MPage *trans_map;
    long *reverse_trans_map;
    int cmt;
};

typedef struct FtlImpl_DftlParent FtlImpl_DftlParent;
typedef struct Event Event;

void MPage_init(FtlImpl_DftlParent *ftl_parent, struct MPage *mpage, long vpn) {
    mpage->vpn = vpn;
    mpage->ppn = -1;
    mpage->create_ts = -1;
    mpage->modified_ts = -1;
    mpage->last_visited_time = -1;
    mpage->cached = 0;
}

double mpage_last_visited_time_compare(const struct MPage *mpage) {
    if (!mpage->cached)
        return DBL_MAX;

    return mpage->last_visited_time;
}

void FtlImpl_DftlParent_init(FtlImpl_DftlParent *ftl_parent, struct Controller *controller) {
    ftl_parent->controller = controller;
    ftl_parent->addressPerPage = 0;
    ftl_parent->cmt = 0;
    ftl_parent->currentDataPage = -1;
    ftl_parent->currentTranslationPage = -1;
    ftl_parent->addressSize = log(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE) / log(2);
    ftl_parent->addressPerPage = (PAGE_SIZE / ceil(ftl_parent->addressSize / 8.0));
    ftl_parent->totalCMTentries = CACHE_DFTL_LIMIT * ftl_parent->addressPerPage;
    ftl_parent->trans_map = malloc(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE * sizeof(struct MPage));
    ftl_parent->reverse_trans_map = malloc(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE * sizeof(long));

    for (int i = 0; i < NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE; i++) {
        MPage_init(ftl_parent, &ftl_parent->trans_map[i], i);
    }
}

void consult_GTD(long dlpn, Event *event, FtlImpl_DftlParent *ftl_parent) {
    // Simulate that we go to translation map and read the mapping page.
    Event readEvent = {0};
    readEvent.type = 0;
    readEvent.logical_address = event->logical_address;
    readEvent.size = 1;
    readEvent.start_time = event->start_time;
    readEvent.noop = 1;

    // Issue read event
    // controller_issue(ftl_parent->controller, &readEvent);
    // event_consolidate_metaevent(event, &readEvent);
    event->start_time += readEvent.start_time;
    // controller_stats_numFTLRead++;
}

void reset_MPage(struct MPage *mpage) {
    mpage->create_ts = -2;
    mpage->modified_ts = -2;
    mpage->last_visited_time = -2;
}

int lookup_CMT(long dlpn, Event *event, FtlImpl_DftlParent *ftl_parent) {
    if (!ftl_parent->trans_map[dlpn].cached)
        return 0;

    event->start_time += 1; // RAM_READ_DELAY
    // controller_stats_numMemoryRead++;

    return 1;
}

long get_free_data_page(Event *event, FtlImpl_DftlParent *ftl_parent) {
    return get_free_data_page(event, ftl_parent, 1);
}

long get_free_data_page(Event *event, FtlImpl_DftlParent *ftl_parent, int insert_events) {
    if (ftl_parent->currentDataPage == -1 || (ftl_parent->currentDataPage % BLOCK_SIZE == BLOCK_SIZE - 1 && insert_events)) {
        // Block_manager_insert_events(ftl_parent->block_manager, event);
    }

    if (ftl_parent->currentDataPage == -1 || ftl_parent->currentDataPage % BLOCK_SIZE == BLOCK_SIZE - 1) {
        // ftl_parent->currentDataPage = Block_manager_get_free_block(DATA, event).linear_address;
    } else {
        ftl_parent->currentDataPage++;
    }

    return ftl_parent->currentDataPage;
}

void resolve_mapping(Event *event, int isWrite, FtlImpl_DftlParent *ftl_parent) {
    long dlpn = event->logical_address;
    /* 1. Lookup in CMT if the mapping exist
     * 2. If, then serve
     * 3. If not, then goto GDT, lookup page
     * 4. If CMT full, evict a page
     * 5. Add mapping to CMT
     */
    if (lookup_CMT(dlpn, event, ftl_parent)) {
        // controller_stats_numCacheHits++;

        struct MPage *current = &ftl_parent->trans_map[dlpn];
        if (isWrite) {
            current->modified_ts = event->start_time;
        }
        current->last_visited_time = event->start_time;

        // evict_page_from_cache(event);    // no need to evict page from cache
    } else {
        // controller_stats_numCacheFaults++;

        // evict_page_from_cache(event);

        consult_GTD(dlpn, event, ftl_parent);

        struct MPage *current = &ftl_parent->trans_map[dlpn];
        current->modified_ts = event->start_time;
        current->last_visited_time = event->start_time;
        if (isWrite) {
            current->modified_ts++;
        }
        current->create_ts = event->start_time;
        current->cached = 1;

        ftl_parent->cmt++;
    }
}

void evict_page_from_cache(Event *event, FtlImpl_DftlParent *ftl_parent) {
    while (ftl_parent->cmt >= ftl_parent->totalCMTentries) {
        // Find page to evict
        // MpageByLastVisited::iterator evictit = boost::multi_index::get<1>(trans_map).begin();
        // MPage evictPage = *ev

ictit;

        struct MPage *evictPage = &ftl_parent->trans_map[0];

        assert(evictPage->cached && evictPage->create_ts >= 0 && evictPage->modified_ts >= 0);

        if (evictPage->create_ts != evictPage->modified_ts) {
            // Evict page
            // Inform the ssd model that it should invalidate the previous page.
            // Calculate the start address of the translation page.
            int vpnBase = evictPage->vpn - evictPage->vpn % ftl_parent->addressPerPage;

            for (int i = 0; i < ftl_parent->addressPerPage; i++) {
                struct MPage *cur = &ftl_parent->trans_map[vpnBase + i];
                if (cur->cached) {
                    cur->create_ts = cur->modified_ts;
                }
            }

            // Simulate the write to translate page
            Event write_event = {0};
            write_event.type = 1;
            write_event.logical_address = event->logical_address;
            write_event.size = 1;
            write_event.start_time = event->start_time;
            write_event.noop = 1;

            // controller_issue(ftl_parent->controller, &write_event);
            event->start_time += write_event.start_time;
            // controller_stats_numFTLWrite++;
            // controller_stats_numGCWrite++;
        }

        // Remove page from cache.
        ftl_parent->cmt--;

        evictPage->cached = 0;
        reset_MPage(evictPage);
    }
}

void evict_specific_page_from_cache(Event *event, long lba, FtlImpl_DftlParent *ftl_parent) {
    // Find page to evict
    struct MPage *evictPage = &ftl_parent->trans_map[lba];

    if (!evictPage->cached)
        return;

    assert(evictPage->cached && evictPage->create_ts >= 0 && evictPage->modified_ts >= 0);

    if (evictPage->create_ts != evictPage->modified_ts) {
        // Evict page
        // Inform the ssd model that it should invalidate the previous page.
        // Calculate the start address of the translation page.
        int vpnBase = evictPage->vpn - evictPage->vpn % ftl_parent->addressPerPage;

        for (int i = 0; i < ftl_parent->addressPerPage; i++) {
            struct MPage *cur = &ftl_parent->trans_map[vpnBase + i];
            if (cur->cached) {
                cur->create_ts = cur->modified_ts;
            }
        }

        // Simulate the write to translate page
        Event write_event = {0};
        write_event.type = 1;
        write_event.logical_address = event->logical_address;
        write_event.size = 1;
        write_event.start_time = event->start_time;
        write_event.noop = 1;

        // controller_issue(ftl_parent->controller, &write_event);
        event->start_time += write_event.start_time;
        // controller_stats_numFTLWrite++;
        // controller_stats_numGCWrite++;
    }

    // Remove page from cache.
    ftl_parent->cmt--;

    evictPage->cached = 0;
    reset_MPage(evictPage);
}

void update_translation_map(struct MPage *mpage, long ppn) {
    mpage->ppn = ppn;
    // reverse_trans_map[ppn] = mpage->vpn; // Not implemented in C version
}

void FtlImpl_DftlParent_destroy(FtlImpl_DftlParent *ftl_parent) {
    free(ftl_parent->trans_map);
    free(ftl_parent->reverse_trans_map);
}

int main() {
    struct Controller controller;
    FtlImpl_DftlParent ftl_parent;

    FtlImpl_DftlParent_init(&ftl_parent, &controller);

    // Your code logic goes here

    FtlImpl_DftlParent_destroy(&ftl_parent);

    return 0;
}

/* from here dftl.c */

아래는 주어진 C++ 코드를 C 코드로 변환한 결과입니다.

```c
/* dftp_ftl.c */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <limits.h>

#define NUMBER_OF_ADDRESSABLE_BLOCKS 1024
#define BLOCK_SIZE 1024
#define PAGE_SIZE 512
#define CACHE_DFTL_LIMIT 64

struct Controller {
    int dummy; // Dummy structure for placeholder
};

struct Event {
    int type;
    long logical_address;
    int size;
    double start_time;
    int noop; // Dummy field for placeholder
};

struct BlockManager {
    int dummy; // Dummy structure for placeholder
};

struct MPage {
    long vpn;
    long ppn;
    double create_ts;
    double modified_ts;
    double last_visited_time;
    int cached;
};

struct FtlImpl_DftlParent {
    struct Controller *controller;
    struct BlockManager *block_manager;
    int addressPerPage;
    int currentDataPage;
    int currentTranslationPage;
    int addressSize;
    int totalCMTentries;
    struct MPage *trans_map;
    long *reverse_trans_map;
    int cmt;
};

typedef struct FtlImpl_DftlParent FtlImpl_DftlParent;
typedef struct Event Event;

void MPage_init(struct MPage *mpage, long vpn) {
    mpage->vpn = vpn;
    mpage->ppn = -1;
    mpage->create_ts = -1;
    mpage->modified_ts = -1;
    mpage->last_visited_time = -1;
    mpage->cached = 0;
}

double mpage_last_visited_time_compare(const struct MPage *mpage) {
    if (!mpage->cached)
        return DBL_MAX;

    return mpage->last_visited_time;
}

void FtlImpl_DftlParent_init(FtlImpl_DftlParent *ftl_parent, struct Controller *controller) {
    ftl_parent->controller = controller;
    ftl_parent->addressPerPage = 0;
    ftl_parent->cmt = 0;
    ftl_parent->currentDataPage = -1;
    ftl_parent->currentTranslationPage = -1;
    ftl_parent->addressSize = log(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE) / log(2);
    ftl_parent->addressPerPage = (PAGE_SIZE / ceil(ftl_parent->addressSize / 8.0));
    ftl_parent->totalCMTentries = CACHE_DFTL_LIMIT * ftl_parent->addressPerPage;
    ftl_parent->trans_map = malloc(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE * sizeof(struct MPage));
    ftl_parent->reverse_trans_map = malloc(NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE * sizeof(long));

    for (int i = 0; i < NUMBER_OF_ADDRESSABLE_BLOCKS * BLOCK_SIZE; i++) {
        MPage_init(&ftl_parent->trans_map[i], i);
    }
}

void consult_GTD(long dlpn, Event *event, FtlImpl_DftlParent *ftl_parent) {
    // Simulate that we go to translation map and read the mapping page.
    Event readEvent = {0};
    readEvent.type = 0;
    readEvent.logical_address = event->logical_address;
    readEvent.size = 1;
    readEvent.start_time = event->start_time;
    readEvent.noop = 1;

    // Issue read event
    // controller_issue(ftl_parent->controller, &readEvent);
    // event_consolidate_metaevent(event, &readEvent);
    event->start_time += readEvent.start_time;
    // controller_stats_numFTLRead++;
}

void reset_MPage(struct MPage *mpage) {
    mpage->create_ts = -2;
    mpage->modified_ts = -2;
    mpage->last_visited_time = -2;
}

int lookup_CMT(long dlpn, Event *event, FtlImpl_DftlParent *ftl_parent) {
    if (!ftl_parent->trans_map[dlpn].cached)
        return 0;

    event->start_time += 1; // RAM_READ_DELAY
    // controller_stats_numMemoryRead++;

    return 1;
}

long get_free_data_page(Event *event, FtlImpl_DftlParent *ftl_parent) {
    return get_free_data_page(event, ftl_parent, 1);
}

long get_free_data_page(Event *event, FtlImpl_DftlParent *ftl_parent, int insert_events) {
    if (ftl_parent->currentDataPage == -1 || (ftl_parent->currentDataPage % BLOCK_SIZE == BLOCK_SIZE - 1 && insert_events)) {
        // Block_manager_insert_events(ftl_parent->block_manager, event);
    }

    if (ftl_parent->currentDataPage == -1 || ftl_parent->currentDataPage % BLOCK_SIZE == BLOCK_SIZE - 1) {
        // ftl_parent->currentDataPage = Block_manager_get_free_block(DATA, event).linear_address;
    } else {
        ftl_parent->currentDataPage++;
    }

    return ftl_parent->currentDataPage;
}

void resolve_mapping(Event *event, int isWrite, FtlImpl_DftlParent *ftl_parent) {
    long dlpn = event->logical_address;
    /* 1. Lookup in CMT if the mapping exist
     * 2. If, then serve
     * 3. If not, then goto GDT, lookup page
     * 4. If CMT full, evict a page
     * 5. Add mapping to CMT
     */
    if (lookup_CMT(dlpn, event, ftl_parent)) {
        // controller_stats_numCacheHits++;

        struct MPage *current = &ftl_parent->trans_map[dlpn];
        if (isWrite) {
            current->modified_ts = event->start_time;
        }
        current->last_visited_time = event->start_time;

        // evict_page_from_cache(event);    // no need to evict page from cache
    } else {
        // controller_stats_numCacheFaults++;

        // evict_page_from_cache(event);

        consult_GTD(dlpn, event, ftl_parent);

        struct MPage *current = &ftl_parent->trans_map[dlpn];
        current->modified_ts = event->start_time;
        current->last_visited_time = event->start_time;
        if (isWrite) {
            current->modified_ts++;
        }
        current->create_ts = event->start_time;
        current->cached = 1;

        ftl_parent->cmt++;
    }
}

void evict_page_from_cache(Event *event, FtlImpl_DftlParent *ftl_parent) {
    while (ftl_parent->cmt >= ftl_parent->totalCMTentries) {
        // Find page to evict
        // MpageByLastVisited::iterator evictit = boost::multi_index::get<1>(trans_map).begin();
        // MPage evictPage = *evictit;

        struct MPage *evictPage = &ftl_parent->trans_map[0];

        assert(evictPage->

cached && evictPage->create_ts >= 0 && evictPage->modified_ts >= 0);

        if (evictPage->create_ts != evictPage->modified_ts) {
            // Evict page
            // Inform the ssd model that it should invalidate the previous page.
            // Calculate the start address of the translation page.
            int vpnBase = evictPage->vpn - evictPage->vpn % ftl_parent->addressPerPage;

            for (int i = 0; i < ftl_parent->addressPerPage; i++) {
                struct MPage *cur = &ftl_parent->trans_map[vpnBase + i];
                if (cur->cached) {
                    cur->create_ts = cur->modified_ts;
                }
            }

            // Simulate the write to translate page
            Event write_event = {0};
            write_event.type = 1;
            write_event.logical_address = event->logical_address;
            write_event.size = 1;
            write_event.start_time = event->start_time;
            write_event.noop = 1;

            // controller_issue(ftl_parent->controller, &write_event);
            event->start_time += write_event.start_time;
            // controller_stats_numFTLWrite++;
            // controller_stats_numGCWrite++;
        }

        // Remove page from cache.
        ftl_parent->cmt--;

        evictPage->cached = 0;
        reset_MPage(evictPage);
    }
}

void evict_specific_page_from_cache(Event *event, long lba, FtlImpl_DftlParent *ftl_parent) {
    // Find page to evict
    struct MPage *evictPage = &ftl_parent->trans_map[lba];

    if (!evictPage->cached)
        return;

    assert(evictPage->cached && evictPage->create_ts >= 0 && evictPage->modified_ts >= 0);

    if (evictPage->create_ts != evictPage->modified_ts) {
        // Evict page
        // Inform the ssd model that it should invalidate the previous page.
        // Calculate the start address of the translation page.
        int vpnBase = evictPage->vpn - evictPage->vpn % ftl_parent->addressPerPage;

        for (int i = 0; i < ftl_parent->addressPerPage; i++) {
            struct MPage *cur = &ftl_parent->trans_map[vpnBase + i];
            if (cur->cached) {
                cur->create_ts = cur->modified_ts;
            }
        }

        // Simulate the write to translate page
        Event write_event = {0};
        write_event.type = 1;
        write_event.logical_address = event->logical_address;
        write_event.size = 1;
        write_event.start_time = event->start_time;
        write_event.noop = 1;

        // controller_issue(ftl_parent->controller, &write_event);
        event->start_time += write_event.start_time;
        // controller_stats_numFTLWrite++;
        // controller_stats_numGCWrite++;
    }

    // Remove page from cache.
    ftl_parent->cmt--;

    evictPage->cached = 0;
    reset_MPage(evictPage);
}

void update_translation_map(struct MPage *mpage, long ppn) {
    mpage->ppn = ppn;
    // reverse_trans_map[ppn] = mpage->vpn; // Not implemented in C version
}

void FtlImpl_DftlParent_destroy(FtlImpl_DftlParent *ftl_parent) {
    free(ftl_parent->trans_map);
    free(ftl_parent->reverse_trans_map);
}

int main() {
    struct Controller controller;
    FtlImpl_DftlParent ftl_parent;

    FtlImpl_DftlParent_init(&ftl_parent, &controller);

    // Your code logic goes here

    FtlImpl_DftlParent_destroy(&ftl_parent);

    return 0;
}